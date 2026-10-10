/* IPC：unix socket + 一行一个 JSON 对象（协议见 ipc.h）。
 *
 * 这个文件负责三件事：
 *   ① 传输 —— 监听 socket、每个连接一个缓冲区、一行一条请求、一行一条应答；
 *   ② 命令 —— 把请求派发到合成器的功能上（查询和操作都在这里）；
 *   ③ 订阅 —— 把 ipcstate.c 生成的事件行推给所有订阅者。
 *
 * 状态序列化在 ipcstate.c（"内部状态 -> JSON"那一半），这里只碰传输和操作。 */
#include "ipc.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <wayland-server.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>

#include "hooks.h"
#include "ipcstate.h"
#include "json.h"
#include "server.h"

/* 一条请求最多这么大（含结尾换行）。请求都是命令，正常只有几十字节；
 * 留 1 MiB 是给「不设上限会被人一条连接吃光内存」这件事一个边界。 */
#define IPC_MAX_LINE (1u << 20)
/* 一个客户端攒着没发出去的数据上限：订阅者卡住不动（不读 socket）时，
 * 攒到这么多就把它踢掉，免得合成器跟着它一起涨内存。 */
#define IPC_MAX_OUT (8u << 20)
#define IPC_MAX_CLIENTS 64

/* --------------------------------------------------------------- 连接 */

struct ipc_client {
	struct wl_list link; /* vidow_server.ipc_clients */
	struct vidow_server *server;
	int fd;
	struct wl_event_source *source;

	char *in;  /* 还没凑成一行的输入 */
	size_t in_len, in_cap;

	char *out; /* 还没写出去的数据 */
	size_t out_len, out_cap, out_sent;

	/* 订阅了哪些事件；空 = 全都订（订阅了才收事件，没订阅的连接只问答）。 */
	bool subscribed;
	char **filter;
	size_t filter_n;

	bool want_close; /* 应答写完就关（出错 / 超长） */
	bool dead;       /* 读写出错了，收摊 */
};

static void ipc_client_destroy(struct ipc_client *c);

static void ipc_client_flush(struct ipc_client *c);

static bool ipc_client_out(struct ipc_client *c, const char *data, size_t len) {
	if (c->dead || len == 0) {
		return false;
	}
	if (c->out_len + len > IPC_MAX_OUT) {
		wlr_log(WLR_ERROR, "ipc: 客户端跟不上了（攒了 %zu 字节），断开",
			c->out_len);
		c->dead = true;
		return false;
	}
	/* 把已经写出去的那截丢掉，免得缓冲区只增不减 */
	if (c->out_sent > 0 && c->out_sent == c->out_len) {
		c->out_len = 0;
		c->out_sent = 0;
	}
	size_t need = c->out_len + len;
	if (need > c->out_cap) {
		size_t cap = c->out_cap ? c->out_cap : 4096;
		while (cap < need) {
			cap *= 2;
		}
		char *p = realloc(c->out, cap);
		if (p == NULL) {
			c->dead = true;
			return false;
		}
		c->out = p;
		c->out_cap = cap;
	}
	memcpy(c->out + c->out_len, data, len);
	c->out_len += len;
	ipc_client_flush(c);
	return true;
}

static void ipc_client_flush(struct ipc_client *c) {
	if (c->dead) {
		return;
	}
	while (c->out_sent < c->out_len) {
		ssize_t n = write(c->fd, c->out + c->out_sent, c->out_len - c->out_sent);
		if (n > 0) {
			c->out_sent += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR) {
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		}
		c->dead = true; /* EPIPE / ECONNRESET：对面没了 */
		return;
	}
	if (c->out_sent == c->out_len) {
		c->out_len = 0;
		c->out_sent = 0;
	}
	uint32_t mask = WL_EVENT_READABLE;
	if (c->out_sent < c->out_len) {
		mask |= WL_EVENT_WRITABLE;
	}
	if (c->source != NULL) {
		wl_event_source_fd_update(c->source, mask);
	}
}

static void ipc_client_destroy(struct ipc_client *c) {
	if (c->subscribed) {
		c->server->ipc_subscribers--;
	}
	if (c->source != NULL) {
		wl_event_source_remove(c->source);
	}
	close(c->fd);
	wl_list_remove(&c->link);
	for (size_t i = 0; i < c->filter_n; i++) {
		free(c->filter[i]);
	}
	free(c->filter);
	free(c->in);
	free(c->out);
	free(c);
}

/* ------------------------------------------------------------- 请求派发 */

static bool icontains(const char *hay, const char *needle) {
	if (hay == NULL || needle == NULL || needle[0] == '\0') {
		return false;
	}
	size_t nl = strlen(needle);
	for (const char *p = hay; *p != '\0'; p++) {
		size_t i = 0;
		while (i < nl && p[i] != '\0' &&
				tolower((unsigned char)p[i]) ==
				tolower((unsigned char)needle[i])) {
			i++;
		}
		if (i == nl) {
			return true;
		}
	}
	return false;
}

/* 请求里的文本参数：字符串、数字、布尔都当文本读。
 * （vidowmsg 把纯数字写成 JSON 数字 —— focus-workspace 2 里的 "2" 就是数字，
 * 用 jget_str 读会拿到 NULL。） */
static const char *jtext(struct jval *req, const char *key, char *buf,
		size_t len) {
	struct jval *v = jget(req, key);
	if (v == NULL) {
		return NULL;
	}
	switch (v->t) {
	case J_STR:
		snprintf(buf, len, "%s", v->str);
		return buf;
	case J_NUM:
		if (v->num == (double)(long long)v->num) {
			snprintf(buf, len, "%lld", (long long)v->num);
		} else {
			snprintf(buf, len, "%.6g", v->num);
		}
		return buf;
	case J_BOOL:
		snprintf(buf, len, "%s", v->b ? "true" : "false");
		return buf;
	default:
		return NULL;
	}
}

/* 纯数字？（"2" 当工作区序号用） */
static bool is_number(const char *s) {
	if (s == NULL || s[0] == '\0') {
		return false;
	}
	for (const char *p = s; *p != '\0'; p++) {
		if (*p < '0' || *p > '9') {
			return false;
		}
	}
	return true;
}

static struct vidow_view *view_by_app_id(struct vidow_server *server,
		const char *want) {
	struct vidow_view *view, *sub = NULL;
	wl_list_for_each(view, &server->views, link) {
		const char *aid = vidow_view_app_id(view);
		if (aid == NULL) {
			continue;
		}
		if (strcasecmp(aid, want) == 0) {
			return view;
		}
		if (sub == NULL && icontains(aid, want)) {
			sub = view;
		}
	}
	return sub;
}

static struct vidow_view *view_by_title(struct vidow_server *server,
		const char *want) {
	struct vidow_view *view, *sub = NULL;
	wl_list_for_each(view, &server->views, link) {
		const char *title = vidow_view_title(view);
		if (title == NULL) {
			continue;
		}
		if (strcasecmp(title, want) == 0) {
			return view;
		}
		if (sub == NULL && icontains(title, want)) {
			sub = view;
		}
	}
	return sub;
}

/* 要动哪个窗口：id > app_id > title > 焦点窗口。说不清就是焦点窗口，没有就报错。 */
static struct vidow_view *req_view(struct vidow_server *server, struct jval *req,
		char *err, size_t errlen) {
	long long id = 0;
	if (jget_int(req, "id", &id)) {
		struct vidow_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->id == (uint64_t)id) {
				return view;
			}
		}
		snprintf(err, errlen, "没有 id = %lld 的窗口", id);
		return NULL;
	}
	char text[256];
	const char *app_id = jtext(req, "app_id", text, sizeof(text));
	if (app_id != NULL) {
		struct vidow_view *view = view_by_app_id(server, app_id);
		if (view == NULL) {
			snprintf(err, errlen, "没有 app_id 匹配 '%s' 的窗口", app_id);
		}
		return view;
	}
	const char *title = jtext(req, "title", text, sizeof(text));
	if (title != NULL) {
		struct vidow_view *view = view_by_title(server, title);
		if (view == NULL) {
			snprintf(err, errlen, "没有标题匹配 '%s' 的窗口", title);
		}
		return view;
	}
	if (server->focused == NULL) {
		snprintf(err, errlen,
			"没有聚焦的窗口（说清楚动哪个：id / app_id / title）");
		return NULL;
	}
	return server->focused;
}

static struct vidow_output *output_from_wlr(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == wlr_output) {
			return output;
		}
	}
	return NULL;
}

static struct vidow_output *req_output(struct vidow_server *server,
		struct jval *req, char *err, size_t errlen) {
	char text[256];
	const char *name = jtext(req, "output", text, sizeof(text));
	if (name != NULL) {
		struct vidow_output *output, *sub = NULL;
		wl_list_for_each(output, &server->outputs, link) {
			if (strcasecmp(output->wlr_output->name, name) == 0) {
				return output;
			}
			if (sub == NULL && icontains(output->wlr_output->name, name)) {
				sub = output;
			}
		}
		if (sub == NULL) {
			snprintf(err, errlen, "没有叫 '%s' 的输出", name);
		}
		return sub;
	}
	if (server->focused_output == NULL) {
		snprintf(err, errlen, "没有聚焦的输出（说清楚哪块：output）");
		return NULL;
	}
	struct vidow_output *output = output_from_wlr(server,
		server->focused_output);
	if (output == NULL) {
		snprintf(err, errlen, "聚焦的输出不在列表里");
	}
	return output;
}

/* 要动哪块工作区：
 *   纯数字      —— 聚焦那块屏上的第几块（1 起，越界夹到最后一块；niri 的序号语义）
 *   名字        —— 具名工作区 / 特殊工作区
 *   next / prev / up / down / previous —— 相对切换（就地做掉，返回 NULL）
 *   index = N   —— 老写法（0 起），同样按聚焦那块屏算 */
static struct vidow_workspace *req_workspace(struct vidow_server *server,
		struct jval *req, char *err, size_t errlen) {
	char text[256];
	const char *name = jtext(req, "workspace", text, sizeof(text));
	if (name != NULL) {
		if (strcasecmp(name, "next") == 0 || strcasecmp(name, "prev") == 0 ||
				strcasecmp(name, "up") == 0 ||
				strcasecmp(name, "down") == 0 ||
				strcasecmp(name, "previous") == 0) {
			/* 相对切换交给 action 那条路（它知道「同屏相邻」的规矩）。 */
			char action[64];
			snprintf(action, sizeof(action), "workspace %s", name);
			vidow_action_run(server, action);
			return NULL; /* 已经做了，调用者按「特殊路径」处理 */
		}
		if (is_number(name)) {
			int n = atoi(name);
			int cnt = vidow_workspace_count(server,
				server->focused_output);
			if (cnt == 0) {
				snprintf(err, errlen, "这块屏上一块工作区都没有");
				return NULL;
			}
			if (n < 1) {
				n = 1;
			}
			if (n > cnt) {
				n = cnt; /* niri：序号越界夹到最后一 块 */
			}
			return vidow_workspace_at(server, server->focused_output, n);
		}
		struct vidow_workspace *ws = vidow_workspace_lookup(server, name);
		if (ws == NULL) {
			struct vidow_workspace *other;
			wl_list_for_each(other, &server->workspaces, link) {
				if (other->special && strcmp(other->name, name) == 0) {
					return other;
				}
			}
		}
		if (ws == NULL) {
			snprintf(err, errlen, "没有叫 '%s' 的工作区（具名工作区要先在配置里"
				" vidow.workspace_create() 建）", name);
		}
		return ws;
	}
	long long index = 0;
	if (jget_int(req, "index", &index)) {
		int cnt = vidow_workspace_count(server, server->focused_output);
		if (index < 0 || index >= cnt) {
			snprintf(err, errlen, "工作区序号 %lld 不存在（0 起，这块屏上 %d 块）",
				index, cnt);
			return NULL;
		}
		return vidow_workspace_at(server, server->focused_output,
			(int)index + 1);
	}
	snprintf(err, errlen, "要哪块工作区？（workspace = 序号 / 名字，"
		"index = 序号 0 起）");
	return NULL;
}

/* ------------------------------------------------------------ 查询命令 */

static void json_version(struct jbuf *out, struct vidow_server *server) {
	jb_obj(out);
	jb_kv_str(out, "version", "vidow 0.1.0");
	jb_kv_int(out, "pid", (long long)getpid());
	jb_kv_str(out, "socket", server->ipc_path);
	jb_kv_str(out, "wayland_display", getenv("WAYLAND_DISPLAY"));
	jb_kv_str(out, "log_level", getenv("VIDOW_LOG"));
	jb_obj_end(out);
}

/* ------------------------------------------------------------ 操作命令 */

static void changed_window(struct jbuf *out, struct vidow_server *server,
		struct vidow_view *view) {
	jb_obj(out);
	jb_key(out, "window");
	vidow_ipc_json_window(out, server, view);
	jb_obj_end(out);
}

static bool cmd_focus_window(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	vidow_focus_view(view);
	changed_window(out, server, view);
	return true;
}

static bool cmd_activate_window(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	/* 跟点通知一样：先把它的工作区端出来，再给焦点。 */
	vidow_view_activate(view);
	changed_window(out, server, view);
	return true;
}

static bool cmd_close_window(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	uint64_t id = view->id;
	vidow_view_close(view);
	jb_obj(out);
	jb_kv_uint(out, "closed", id);
	jb_obj_end(out);
	return true;
}

/* 最小化：窗口留在原来的列里，只是不占位、不显示、不参与焦点（见 view.c）。 */
static bool cmd_minimize_window(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	if (view->minimized) {
		snprintf(err, errlen, "id = %llu 的窗口已经最小化了",
			(unsigned long long)view->id);
		return false;
	}
	uint64_t id = view->id;
	vidow_view_set_minimized(view, true);
	jb_obj(out);
	jb_kv_uint(out, "minimized", id);
	jb_obj_end(out);
	return true;
}

/* 放回：没指认窗口（没有 id / app_id / title）就是「最近收起来的那个」——
 * 键位要的正是这个，不用先知道收起来的是谁。 */
static bool cmd_unminimize_window(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	long long want_id = 0;
	char text[256];
	bool has_target = jget_int(req, "id", &want_id) ||
		jtext(req, "app_id", text, sizeof(text)) != NULL ||
		jtext(req, "title", text, sizeof(text)) != NULL;
	if (!has_target) {
		if (!vidow_view_restore_last(server)) {
			snprintf(err, errlen, "现在没有最小化的窗口");
			return false;
		}
		jb_obj(out);
		jb_kv_str(out, "restored", "最近收起来的那个");
		jb_obj_end(out);
		return true;
	}
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	if (!view->minimized) {
		snprintf(err, errlen, "id = %llu 的窗口没最小化",
			(unsigned long long)view->id);
		return false;
	}
	uint64_t id = view->id;
	vidow_view_restore(view);
	jb_obj(out);
	jb_kv_uint(out, "restored", id);
	jb_obj_end(out);
	return true;
}

static bool cmd_toggle_minimize(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	uint64_t id = view->id;
	bool was_minimized = view->minimized;
	vidow_view_toggle_minimized(view);
	jb_obj(out);
	jb_kv_uint(out, "id", id);
	jb_kv_bool(out, "minimized", !was_minimized);
	jb_obj_end(out);
	return true;
}

static bool cmd_toggle_floating(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	if (view->popup) {
		snprintf(err, errlen, "弹出物（菜单 / 提示）本来就不进布局，没有浮动一说");
		return false;
	}
	vidow_float_toggle(view);
	changed_window(out, server, view);
	return true;
}

static bool cmd_set_floating(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	bool want = false;
	if (!jget_bool(req, "floating", &want)) {
		snprintf(err, errlen, "要 floating = true / false");
		return false;
	}
	if (view->popup) {
		snprintf(err, errlen, "弹出物不进布局，改不了");
		return false;
	}
	bool floating = view->float_mode != VIDOW_FLOAT_NONE;
	if (floating != want) {
		vidow_float_toggle(view);
	}
	changed_window(out, server, view);
	return true;
}

static bool cmd_toggle_fullscreen(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	vidow_view_toggle_fullscreen(view);
	changed_window(out, server, view);
	return true;
}

static bool cmd_set_fullscreen(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	bool want = false;
	if (!jget_bool(req, "fullscreen", &want)) {
		snprintf(err, errlen, "要 fullscreen = true / false");
		return false;
	}
	if (view->fullscreen != want) {
		vidow_view_toggle_fullscreen(view);
	}
	changed_window(out, server, view);
	return true;
}

static bool cmd_move_window_to_workspace(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	char text[256];
	const char *name = jtext(req, "workspace", text, sizeof(text));
	if (name == NULL) {
		snprintf(err, errlen, "要 workspace = 名字（特殊工作区也认）");
		return false;
	}
	if (!vidow_workspace_move_view_to_name(server, view, name)) {
		snprintf(err, errlen, "搬不动（工作区 '%s' 就是它现在待的那块？）", name);
		return false;
	}
	changed_window(out, server, view);
	return true;
}

static bool cmd_move_window_to_special(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	char text[256];
	const char *name = jtext(req, "name", text, sizeof(text));
	if (name == NULL) {
		name = "special";
	}
	if (!vidow_workspace_move_view_to_name(server, view, name)) {
		snprintf(err, errlen, "搬不动（特殊工作区 '%s'）", name);
		return false;
	}
	changed_window(out, server, view);
	return true;
}

static bool cmd_move_window_to_output(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	struct vidow_output *output = req_output(server, req, err, errlen);
	if (output == NULL) {
		return false;
	}
	/* 搬到那块屏上现在显示着的工作区（列里的窗口整列搬过去）。 */
	struct vidow_workspace *target =
		vidow_workspace_active_for(server, output->wlr_output);
	if (target == NULL ||
			!vidow_workspace_move_view(server, view, target)) {
		snprintf(err, errlen, "搬到 %s 失败", output->wlr_output->name);
		return false;
	}
	changed_window(out, server, view);
	return true;
}

static bool cmd_set_window_size(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	long long w = 0, h = 0;
	if (!jget_int(req, "width", &w) || !jget_int(req, "height", &h) ||
			w <= 0 || h <= 0) {
		snprintf(err, errlen, "要 width / height（正整数，逻辑像素）");
		return false;
	}
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		snprintf(err, errlen,
			"平铺窗口的尺寸由布局定（先 toggle-floating，或者用 set_column_width）");
		return false;
	}
	vidow_float_set_size(view, (int)w, (int)h);
	changed_window(out, server, view);
	return true;
}

static bool cmd_set_window_position(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	long long x = 0, y = 0;
	if (!jget_int(req, "x", &x) || !jget_int(req, "y", &y)) {
		snprintf(err, errlen, "要 x / y（布局坐标）");
		return false;
	}
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		snprintf(err, errlen, "平铺窗口的位置由布局定（先 toggle-floating）");
		return false;
	}
	vidow_float_move_to(view, (int)x, (int)y);
	changed_window(out, server, view);
	return true;
}

static bool cmd_center_window(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		snprintf(err, errlen, "平铺窗口的位置由布局定（先 toggle-floating）");
		return false;
	}
	vidow_float_center(view);
	changed_window(out, server, view);
	return true;
}

static bool cmd_set_window_force_render(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	bool on = false;
	if (!jget_bool(req, "on", &on)) {
		snprintf(err, errlen, "要 on = true / false");
		return false;
	}
	view->force_render = on;
	wlr_log(WLR_INFO, "ipc: %s 视口外补帧 %s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		on ? "开" : "关");
	changed_window(out, server, view);
	return true;
}

/* 方向参数：-1 / 1（也可以写 "left" "right" "up" "down" "prev" "next"）。 */
static bool req_direction(struct jval *req, int *out, char *err, size_t errlen) {
	long long d = 0;
	if (jget_int(req, "direction", &d)) {
		if (d == 0) {
			snprintf(err, errlen, "direction 不能是 0");
			return false;
		}
		*out = d < 0 ? -1 : 1;
		return true;
	}
	const char *name = jget_str(req, "direction");
	if (name != NULL) {
		if (strcasecmp(name, "left") == 0 || strcasecmp(name, "up") == 0 ||
				strcasecmp(name, "prev") == 0) {
			*out = -1;
			return true;
		}
		if (strcasecmp(name, "right") == 0 || strcasecmp(name, "down") == 0 ||
				strcasecmp(name, "next") == 0) {
			*out = 1;
			return true;
		}
	}
	snprintf(err, errlen, "要 direction = -1 / 1（left / right / up / down 也认）");
	return false;
}

static bool cmd_focus_column(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	int direction = 0;
	if (!req_direction(req, &direction, err, errlen)) {
		return false;
	}
	vidow_focus_column(server, direction);
	jb_obj(out);
	jb_kv_int(out, "direction", direction);
	jb_key(out, "focused");
	vidow_ipc_json_window(out, server, server->focused);
	jb_obj_end(out);
	return true;
}

static bool cmd_focus_window_direction(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	int direction = 0;
	if (!req_direction(req, &direction, err, errlen)) {
		return false;
	}
	vidow_focus_window(server, direction);
	jb_obj(out);
	jb_kv_int(out, "direction", direction);
	jb_key(out, "focused");
	vidow_ipc_json_window(out, server, server->focused);
	jb_obj_end(out);
	return true;
}

static bool cmd_move_column(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	int direction = 0;
	if (!req_direction(req, &direction, err, errlen)) {
		return false;
	}
	vidow_move_column(server, direction);
	jb_obj(out);
	jb_kv_int(out, "direction", direction);
	jb_key(out, "focused");
	vidow_ipc_json_window(out, server, server->focused);
	jb_obj_end(out);
	return true;
}

static bool cmd_move_window_direction(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	int direction = 0;
	if (!req_direction(req, &direction, err, errlen)) {
		return false;
	}
	vidow_move_window(server, direction);
	jb_obj(out);
	jb_kv_int(out, "direction", direction);
	jb_key(out, "focused");
	vidow_ipc_json_window(out, server, server->focused);
	jb_obj_end(out);
	return true;
}

static bool cmd_cycle_column_width(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	if (server->focused == NULL || server->focused->column == NULL) {
		snprintf(err, errlen, "没有聚焦的列");
		return false;
	}
	vidow_cycle_column_width(server);
	jb_obj(out);
	jb_key(out, "column");
	vidow_ipc_json_column(out, server, server->focused->column);
	jb_obj_end(out);
	return true;
}

/* 列宽：fraction（绝对比例）或 delta（相对加减）。夹在 5%..100%。 */
static bool cmd_set_column_width(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	struct vidow_column *col = view->column;
	if (col == NULL) {
		snprintf(err, errlen, "这个窗口不在列里（浮窗没有列宽）");
		return false;
	}
	double fraction = 0.0;
	double delta = 0.0;
	bool have_fraction = jget_double(req, "fraction", &fraction);
	bool have_delta = jget_double(req, "delta", &delta);
	if (!have_fraction && !have_delta) {
		snprintf(err, errlen, "要 fraction = 0.5（绝对）或 delta = 0.1（相对）");
		return false;
	}
	double want = have_fraction ? fraction : col->width_fraction + delta;
	if (want < 0.05) {
		want = 0.05;
	}
	if (want > 1.0) {
		want = 1.0;
	}
	col->width_fraction = want;
	/* 手动指定宽度 = 退出「列最大化」这个状态（最大化是「记住原宽度再还原」，
	 * 用户自己给了宽度就以用户的为准，见 server.h 的 vidow_column.maximized）。 */
	col->maximized = false;
	vidow_arrange(server);
	jb_obj(out);
	jb_key(out, "column");
	vidow_ipc_json_column(out, server, col);
	jb_obj_end(out);
	return true;
}

static bool cmd_expand_column_to_available_width(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	if (server->focused == NULL || server->focused->column == NULL) {
		snprintf(err, errlen, "没有聚焦的列");
		return false;
	}
	server->focused->column->width_fraction = 1.0;
	/* 同上：手动改宽度退出最大化状态（要「记住原宽度、还能还原」就用
	 * toggle-maximize-column）。 */
	server->focused->column->maximized = false;
	vidow_arrange(server);
	jb_obj(out);
	jb_key(out, "column");
	vidow_ipc_json_column(out, server, server->focused->column);
	jb_obj_end(out);
	return true;
}

static bool cmd_toggle_maximize_column(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	/* 指认窗口的选项（--id / --title / …）认；不给就动聚焦的那个。 */
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	struct vidow_column *col = view->column;
	if (col == NULL) {
		snprintf(err, errlen, "这个窗口不在列里（浮窗没有列）");
		return false;
	}
	vidow_column_set_maximized(col, !col->maximized);
	jb_obj(out);
	jb_key(out, "column");
	vidow_ipc_json_column(out, server, col);
	jb_obj_end(out);
	return true;
}

static bool cmd_set_maximize_column(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	struct vidow_column *col = view->column;
	if (col == NULL) {
		snprintf(err, errlen, "这个窗口不在列里（浮窗没有列）");
		return false;
	}
	bool want = false;
	if (!jget_bool(req, "maximized", &want)) {
		snprintf(err, errlen, "要 maximized = true / false");
		return false;
	}
	vidow_column_set_maximized(col, want);
	jb_obj(out);
	jb_key(out, "column");
	vidow_ipc_json_column(out, server, col);
	jb_obj_end(out);
	return true;
}

static bool cmd_center_column(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL || view->column->ws == NULL) {
		snprintf(err, errlen, "没有聚焦的列");
		return false;
	}
	struct vidow_column *col = view->column;
	struct vidow_output *output = output_from_wlr(server,
		col->output != NULL ? col->output : server->focused_output);
	if (output == NULL) {
		snprintf(err, errlen, "这条列不在任何一块屏上");
		return false;
	}
	struct wlr_box work = vidow_work_area(server, output->wlr_output);
	if (work.width < 1) {
		snprintf(err, errlen, "那块屏没有可用区域");
		return false;
	}
	/* 先 arrange 一次（保证它可见），再把视口居中：居中之后 arrange 的
	 * 「最小滚动」条件不再成立，所以它会保留这个位置。 */
	vidow_arrange(server);
	double want = (double)col->offset + col->width / 2.0 - work.width / 2.0;
	if (want < 0.0) {
		want = 0.0;
	}
	output->viewport_x = want;
	vidow_arrange(server);
	jb_obj(out);
	jb_kv_double(out, "viewport_x", output->viewport_x);
	jb_key(out, "column");
	vidow_ipc_json_column(out, server, col);
	jb_obj_end(out);
	return true;
}

static bool cmd_focus_workspace(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_workspace *ws = req_workspace(server, req, err, errlen);
	if (ws == NULL) {
		/* "next" / "prev" 已经在 req_workspace 里直接做掉了 */
		if (err[0] != '\0') {
			return false;
		}
		jb_obj(out);
		jb_key(out, "workspace");
		vidow_ipc_json_workspace(out, server, vidow_workspace_shown(server));
		jb_obj_end(out);
		return true;
	}
	if (ws->special) {
		if (!vidow_workspace_show_special(server, ws, server->focused_output)) {
			snprintf(err, errlen, "打不开特殊工作区 '%s'", ws->name);
			return false;
		}
	} else if (!vidow_workspace_activate(server, ws)) {
		snprintf(err, errlen, "切不到工作区 '%s'", ws->name);
		return false;
	}
	jb_obj(out);
	jb_key(out, "workspace");
	vidow_ipc_json_workspace(out, server, ws);
	jb_obj_end(out);
	return true;
}

static bool cmd_focus_workspace_previous(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	if (!vidow_workspace_focus_previous(server)) {
		snprintf(err, errlen, "没有上一块工作区（还没切过 / 那块已经没了）");
		return false;
	}
	jb_obj(out);
	jb_key(out, "workspace");
	vidow_ipc_json_workspace(out, server, vidow_workspace_shown(server));
	jb_obj_end(out);
	return true;
}

/* 整块工作区在这块屏上上/下移一位（niri 的 move-workspace-up/down）。 */
static bool cmd_move_workspace_step(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen,
		int direction) {
	char text[256];
	long long index = 0;
	bool given = jtext(req, "workspace", text, sizeof(text)) != NULL ||
		jget_int(req, "index", &index);
	struct vidow_workspace *ws = given ?
		req_workspace(server, req, err, errlen) :
		vidow_workspace_active(server);
	if (ws == NULL) {
		if (err[0] != '\0') {
			return false;
		}
		snprintf(err, errlen, "没有可以重排的工作区");
		return false;
	}
	if (ws == NULL || ws->special) {
		snprintf(err, errlen, "特殊工作区不能重排");
		return false;
	}
	if (!vidow_workspace_move_step(server, ws, direction)) {
		snprintf(err, errlen, "已经在%s了", direction < 0 ? "最上面" : "最下面");
		return false;
	}
	jb_obj(out);
	jb_key(out, "workspace");
	vidow_ipc_json_workspace(out, server, ws);
	jb_obj_end(out);
	return true;
}

/* 给工作区起名 / 去掉名字（niri 的 set-workspace-name / unset-workspace-name）。 */
static bool cmd_set_workspace_name(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	char ws_text[256];
	long long index = 0;
	bool given = jtext(req, "workspace", ws_text, sizeof(ws_text)) != NULL ||
		jget_int(req, "index", &index);
	struct vidow_workspace *ws = given ?
		req_workspace(server, req, err, errlen) :
		vidow_workspace_active(server);
	if (ws == NULL) {
		if (err[0] != '\0') {
			return false;
		}
		snprintf(err, errlen, "没有可以改名的工作区");
		return false;
	}
	if (ws == NULL || ws->special) {
		snprintf(err, errlen, "特殊工作区不能在运行时改名");
		return false;
	}
	char text[256];
	const char *name = jtext(req, "name", text, sizeof(text));
	if (!vidow_workspace_set_name(server, ws, name)) {
		snprintf(err, errlen, "改不了名字（重名？）");
		return false;
	}
	jb_obj(out);
	jb_key(out, "workspace");
	vidow_ipc_json_workspace(out, server, ws);
	jb_obj_end(out);
	return true;
}

/* 把窗口搬到相邻的那块工作区（niri 的 move-window-to-workspace-up/down）。 */
static bool cmd_move_window_to_workspace_step(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen,
		int direction) {
	struct vidow_view *view = req_view(server, req, err, errlen);
	if (view == NULL) {
		return false;
	}
	struct vidow_workspace *from = view->ws;
	if (from == NULL || from->special || from->output == NULL) {
		snprintf(err, errlen, "这个窗口现在不在普通工作区上");
		return false;
	}
	int idx = vidow_workspace_index(from) + direction;
	int cnt = vidow_workspace_count(server, from->output);
	if (idx < 1 || idx > cnt) {
		snprintf(err, errlen, "那边没有工作区了");
		return false;
	}
	struct vidow_workspace *target =
		vidow_workspace_at(server, from->output, idx);
	if (target == NULL || !vidow_workspace_move_view(server, view, target)) {
		snprintf(err, errlen, "搬不动");
		return false;
	}
	changed_window(out, server, view);
	return true;
}

static bool cmd_toggle_special(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	char text[256];
	const char *name = jtext(req, "name", text, sizeof(text));
	if (name == NULL) {
		name = "special";
	}
	if (!vidow_workspace_toggle_special(server, name)) {
		snprintf(err, errlen, "切不动特殊工作区 '%s'", name);
		return false;
	}
	jb_obj(out);
	jb_kv_str(out, "name", name);
	jb_key(out, "shown");
	{
		struct vidow_workspace *ws = vidow_workspace_special_shown(server);
		vidow_ipc_json_workspace(out, server, ws);
	}
	jb_obj_end(out);
	return true;
}

static bool cmd_move_workspace_to_output(struct vidow_server *server,
		struct jval *req, struct jbuf *out, char *err, size_t errlen) {
	struct vidow_workspace *ws = req_workspace(server, req, err, errlen);
	if (ws == NULL) {
		return false;
	}
	struct vidow_output *output = req_output(server, req, err, errlen);
	if (output == NULL) {
		return false;
	}
	if (ws->special) {
		snprintf(err, errlen, "特殊工作区不绑屏（它跟着聚焦的屏走）");
		return false;
	}
	if (!vidow_workspace_move_to_output(server, ws, output->wlr_output)) {
		snprintf(err, errlen, "搬不动");
		return false;
	}
	jb_obj(out);
	jb_key(out, "workspace");
	vidow_ipc_json_workspace(out, server, ws);
	jb_obj_end(out);
	return true;
}

static bool cmd_create_workspace(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	char text[256];
	const char *name = jtext(req, "name", text, sizeof(text));
	struct vidow_output *output = NULL;
	char out_text[256];
	const char *out_name = jtext(req, "output", out_text, sizeof(out_text));
	if (out_name != NULL) {
		output = req_output(server, req, err, errlen);
		if (output == NULL) {
			return false;
		}
	} else if (server->focused_output != NULL) {
		output = output_from_wlr(server, server->focused_output);
	}
	struct wlr_output *wlr_output =
		output != NULL ? output->wlr_output : server->focused_output;
	struct vidow_workspace *ws;
	if (name == NULL || name[0] == '\0') {
		/* 没给名字：返回那块「新工作区」位（栈底那块空的；它不空了就补一块） */
		vidow_workspace_ensure_bottom(server, wlr_output);
		int cnt = vidow_workspace_count(server, wlr_output);
		ws = vidow_workspace_at(server, wlr_output, cnt);
	} else {
		ws = vidow_workspace_create(server, name, wlr_output);
	}
	if (ws == NULL) {
		snprintf(err, errlen, "建不出来");
		return false;
	}
	jb_obj(out);
	jb_key(out, "workspace");
	vidow_ipc_json_workspace(out, server, ws);
	jb_obj_end(out);
	return true;
}

/* ------------------------------------------------------------- 输出命令 */

static bool cmd_output_power(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_output *output = req_output(server, req, err, errlen);
	if (output == NULL) {
		return false;
	}
	bool on = !output->wlr_output->enabled;
	bool want = false;
	if (jget_bool(req, "on", &want)) {
		on = want;
	} else {
		char text[64];
		const char *mode = jtext(req, "mode", text, sizeof(text));
		if (mode != NULL) {
			if (strcasecmp(mode, "on") == 0) {
				on = true;
			} else if (strcasecmp(mode, "off") == 0) {
				on = false;
			} else if (strcasecmp(mode, "toggle") != 0) {
				snprintf(err, errlen, "mode 只能是 on / off / toggle");
				return false;
			}
		}
	}
	if (!vidow_output_set_enabled(server, output->wlr_output, on)) {
		snprintf(err, errlen, "%s %s 失败", output->wlr_output->name,
			on ? "打开" : "关掉");
		return false;
	}
	jb_obj(out);
	jb_key(out, "output");
	vidow_ipc_json_output(out, server, output);
	jb_obj_end(out);
	return true;
}

static bool cmd_output_scale(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_output *output = req_output(server, req, err, errlen);
	if (output == NULL) {
		return false;
	}
	double scale = 0.0;
	char scale_buf[64];
	const char *text = jtext(req, "scale", scale_buf, sizeof(scale_buf));
	if (text != NULL) {
		if (strcasecmp(text, "auto") == 0) {
			/* "auto" = 按配置（scale + 每块屏那组）重算一遍。 */
			vidow_output_apply_scale(server, output);
			jb_obj(out);
			jb_key(out, "output");
			vidow_ipc_json_output(out, server, output);
			jb_obj_end(out);
			return true;
		}
		scale = atof(text);
	} else if (!jget_double(req, "scale", &scale)) {
		snprintf(err, errlen, "要 scale = 1.5（或者 auto = 按配置重算）");
		return false;
	}
	if (scale < 0.25 || scale > 8.0) {
		snprintf(err, errlen, "缩放得在 0.25 .. 8 之间（给的 %.3g）", scale);
		return false;
	}
	if (!vidow_output_set_scale(server, output->wlr_output, scale,
			"IPC 指定")) {
		snprintf(err, errlen, "设缩放失败");
		return false;
	}
	jb_obj(out);
	jb_key(out, "output");
	vidow_ipc_json_output(out, server, output);
	jb_obj_end(out);
	return true;
}

static bool cmd_output_transform(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_output *output = req_output(server, req, err, errlen);
	if (output == NULL) {
		return false;
	}
	char transform_buf[64];
	const char *text = jtext(req, "transform", transform_buf,
		sizeof(transform_buf));
	enum wl_output_transform transform;
	if (text == NULL || !vidow_output_parse_transform(text, &transform)) {
		snprintf(err, errlen, "要 transform = normal / 90 / 180 / 270 / flipped…");
		return false;
	}
	if (!vidow_output_set_transform(server, output->wlr_output, transform)) {
		snprintf(err, errlen, "%s 不支持这个方向", output->wlr_output->name);
		return false;
	}
	jb_obj(out);
	jb_key(out, "output");
	vidow_ipc_json_output(out, server, output);
	jb_obj_end(out);
	return true;
}

static bool cmd_output_mode(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_output *output = req_output(server, req, err, errlen);
	if (output == NULL) {
		return false;
	}
	long long w = 0, h = 0;
	double hz = 0.0;
	if (!jget_int(req, "width", &w) || !jget_int(req, "height", &h)) {
		snprintf(err, errlen, "要 width / height（hz 可选）");
		return false;
	}
	jget_double(req, "hz", &hz);
	if (!vidow_output_set_mode(server, output->wlr_output, (int)w, (int)h, hz)) {
		snprintf(err, errlen, "%s 没有 %lldx%lld%s 这个模式",
			output->wlr_output->name, w, h,
			hz > 0.0 ? "（或刷新率对不上）" : "");
		return false;
	}
	jb_obj(out);
	jb_key(out, "output");
	vidow_ipc_json_output(out, server, output);
	jb_obj_end(out);
	return true;
}

static bool cmd_output_position(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_output *output = req_output(server, req, err, errlen);
	if (output == NULL) {
		return false;
	}
	long long x = 0, y = 0;
	if (!jget_int(req, "x", &x) || !jget_int(req, "y", &y)) {
		snprintf(err, errlen, "要 x / y（布局坐标，逻辑像素）");
		return false;
	}
	if (!vidow_output_set_position(server, output->wlr_output, (int)x, (int)y)) {
		snprintf(err, errlen, "摆不动");
		return false;
	}
	jb_obj(out);
	jb_key(out, "output");
	vidow_ipc_json_output(out, server, output);
	jb_obj_end(out);
	return true;
}

static bool cmd_output_focus(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	struct vidow_output *output = req_output(server, req, err, errlen);
	if (output == NULL) {
		return false;
	}
	server->focused_output = output->wlr_output;
	jb_obj(out);
	jb_key(out, "output");
	vidow_ipc_json_output(out, server, output);
	jb_obj_end(out);
	return true;
}

/* -------------------------------------------------------- 合成器命令 */

static bool cmd_action(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	char text[256];
	const char *action = jtext(req, "action", text, sizeof(text));
	if (action == NULL) {
		snprintf(err, errlen,
			"要 action = \"workspace 2\"（跟键位绑定里写的是同一套）");
		return false;
	}
	if (!vidow_action_run(server, action)) {
		snprintf(err, errlen, "动作 '%s' 没做成", action);
		return false;
	}
	jb_obj(out);
	jb_kv_str(out, "action", action);
	jb_kv_bool(out, "done", true);
	jb_obj_end(out);
	return true;
}

/* 配置里挂着的钩子（vidow.on）：谁在听哪个事件、跑过几次。 */
static bool cmd_hooks(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	(void)err;
	(void)errlen;
	vidow_hooks_json(out, server);
	return true;
}

/* 全部事件名（合起来就是「vidow 会发哪些事件」的权威清单）。 */
static bool cmd_events(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	(void)server;
	(void)req;
	(void)err;
	(void)errlen;
	const char *const *names = NULL;
	size_t n = vidow_event_names(&names);
	jb_obj(out);
	jb_kv_uint(out, "count", n);
	jb_key(out, "events");
	jb_arr(out);
	for (size_t i = 0; i < n; i++) {
		jb_str(out, names[i]);
	}
	jb_arr_end(out);
	jb_obj_end(out);
	return true;
}

/* 自己发一个事件：订阅者和钩子都会收到（名字随便起，data 原样带上）。
 * 脚本之间、脚本和配置钩子之间的一条自定义通道。 */
static bool cmd_emit(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	char text[256];
	const char *event = jtext(req, "event", text, sizeof(text));
	if (event == NULL || event[0] == '\0') {
		snprintf(err, errlen, "要 event = \"my-event\"（名字随便起）");
		return false;
	}
	struct jval *data = jget(req, "data");
	/* 客户端可能把 data 当字符串传（"--data '{"a":1}"' 被引起来那种）：
	 * 再解一次，能解就当 JSON 用 —— 订阅者和钩子拿到的才是字段而不是一串文本。 */
	struct jval *parsed = NULL;
	if (data != NULL && data->t == J_STR && data->str != NULL) {
		char jerr[128] = "";
		struct jval *again = jparse(data->str, jerr, sizeof(jerr));
		if (again != NULL) {
			parsed = again;
			data = again;
		}
	}
	vidow_ipc_emit_custom(server, event, data);
	jfree(parsed);
	jb_obj(out);
	jb_kv_str(out, "event", event);
	jb_kv_int(out, "subscribers", server->ipc_subscribers);
	jb_kv_int(out, "hooks", vidow_hooks_matching(server, event));
	jb_obj_end(out);
	return true;
}

static bool cmd_set_config(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	char key_text[256];
	char value_text[256];
	const char *key = jtext(req, "key", key_text, sizeof(key_text));
	const char *value = jtext(req, "value", value_text, sizeof(value_text));
	if (key == NULL || value == NULL) {
		snprintf(err, errlen, "要 key / value（value 一律当文本写）");
		return false;
	}
	if (!vidow_config_set(&server->config, key, value)) {
		snprintf(err, errlen, "改不了 %s（没有这个键，或者值不合法）", key);
		return false;
	}
	wlr_log(WLR_INFO, "ipc: 配置 %s = %s（运行时改的，SIGHUP 会按配置文件重来）",
		key, value);
	/* 改了外观类的东西：把缓存的背景 / 布局都刷新一遍。缩放类还要重算输出。 */
	vidow_backdrop_damage(server, true);
	if (strcmp(key, "scale") == 0 ||
			strcmp(key, "xwayland_scale") == 0) {
		vidow_output_apply_scale_all(server);
		vidow_xwayland_scale_apply(server);
	}
	/* decor 开关改了：客户端要不要自己画标题栏得重新通知 */
	if (strcmp(key, "decor") == 0) {
		vidow_decoration_apply_config(server);
	}
	/* 剪贴板那几个键改了要重新评估「接不接管」 */
	if (strcmp(key, "clipboard") == 0 ||
			strcmp(key, "clipboard_primary") == 0) {
		vidow_clipboard_apply_config(server);
	}
	/* 激活令牌那两个键改了要重设定时器 */
	if (strcmp(key, "activation_token_timeout") == 0 ||
			strcmp(key, "honor_xdg_activation_with_invalid_serial") == 0) {
		vidow_activation_apply_config(server);
	}
	vidow_arrange(server);
	const char *now = vidow_config_get(&server->config, key);
	jb_obj(out);
	jb_kv_str(out, "key", key);
	jb_kv_str(out, "value", now != NULL ? now : value);
	jb_obj_end(out);
	return true;
}

static bool cmd_get_config(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	char key_text[256];
	const char *key = jtext(req, "key", key_text, sizeof(key_text));
	if (key == NULL) {
		vidow_ipc_json_config(out, server);
		return true;
	}
	const char *value = vidow_config_get(&server->config, key);
	if (value == NULL) {
		snprintf(err, errlen, "没有 %s 这个配置键", key);
		return false;
	}
	jb_obj(out);
	jb_kv_str(out, "key", key);
	jb_kv_str(out, "value", value);
	jb_obj_end(out);
	return true;
}

static bool cmd_reload_config(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	(void)err;
	(void)errlen;
	vidow_config_reload(server);
	jb_obj(out);
	jb_kv_bool(out, "reloaded", true);
	jb_obj_end(out);
	return true;
}

static bool cmd_quit(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	(void)err;
	(void)errlen;
	wlr_log(WLR_INFO, "ipc: 收到 quit，退出");
	jb_obj(out);
	jb_kv_bool(out, "quitting", true);
	jb_obj_end(out);
	wl_display_terminate(server->display);
	return true;
}

/* ------------------------------------------------- 摇晃找光标（见 cursorfx.c）
 *
 * 摇晃是「找指针」用的：快速来回晃几下，指针就地放大一会儿。平时不用调它 ——
 * 这条命令是给脚本 / 远程用的（也是测试的确定性入口）。 */
static bool cmd_cursor_shake(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	const char *why = NULL;
	if (!vidow_cursor_fx_shake(server, &why)) {
		snprintf(err, errlen, "%s", why != NULL ? why : "放大不了");
		return false;
	}
	jb_obj(out);
	vidow_cursor_fx_json(out, server);
	jb_obj_end(out);
	return true;
}

/* ------------------------------------------------------- 输入注入（见 input.c）
 *
 * 这几条命令能合成鼠标 / 键盘事件 —— 谁连上 socket 谁就能操控整个桌面。默认
 * 开着（config.input_injection），每次注入都会打一条 info 日志留痕。
 * 注入的按键只给**客户端**，不触发 vidow 自己的快捷键。
 */

static bool cmd_move_cursor(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	double x = 0.0, y = 0.0;
	if (!jget_double(req, "x", &x) || !jget_double(req, "y", &y)) {
		snprintf(err, errlen, "要 x / y（布局坐标）");
		return false;
	}
	bool relative = false;
	jget_bool(req, "relative", &relative);
	/* 顺带把指针焦点也更新了（wlr_cursor_warp 自己只挪位置，见 input.c） */
	if (!vidow_inject_pointer_move(server, x, y, relative, err, errlen)) {
		return false;
	}
	jb_obj(out);
	jb_key(out, "cursor");
	jb_obj(out);
	jb_kv_double(out, "x", server->cursor->x);
	jb_kv_double(out, "y", server->cursor->y);
	jb_obj_end(out);
	jb_obj_end(out);
	return true;
}

/* state = click / press / release；button = left（默认）/ right / middle /
 * back / forward / 数字；count > 1 连点（时间戳隔 40ms，客户端才认双击）。 */
static bool cmd_pointer_button(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	const char *state = jget_str(req, "state");
	int mode = -1;
	if (state != NULL) {
		if (strcasecmp(state, "click") == 0 || strcasecmp(state, "tap") == 0) {
			mode = 0;
		} else if (strcasecmp(state, "press") == 0 ||
				strcasecmp(state, "down") == 0) {
			mode = 1;
		} else if (strcasecmp(state, "release") == 0 ||
				strcasecmp(state, "up") == 0) {
			mode = 2;
		}
	}
	if (mode < 0) {
		snprintf(err, errlen, "要 state = click / press / release");
		return false;
	}
	const char *button = jget_str(req, "button");
	if (button == NULL || button[0] == '\0') {
		button = "left";
	}
	long long count = 1;
	jget_int(req, "count", &count);
	if (count < 1) {
		count = 1;
	}
	if (count > 100) {
		count = 100;
	}
	if (!vidow_inject_pointer_button(server, button, mode, (int)count, err,
			errlen)) {
		return false;
	}
	jb_obj(out);
	jb_key(out, "pointer");
	jb_obj(out);
	jb_kv_str(out, "button", button);
	jb_kv_uint(out, "code", vidow_pointer_button_from_name(button));
	jb_kv_str(out, "state", mode == 0 ? "click" : mode == 1 ? "press" : "release");
	jb_kv_int(out, "count", count);
	jb_obj_end(out);
	jb_obj_end(out);
	return true;
}

/* dx / dy：discrete（默认 true）= 格数，一格 = delta 15 像素 + 离散值 120；
 * false = 连续滚动的像素值。 */
static bool cmd_pointer_scroll(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	double dx = 0.0, dy = 0.0;
	bool has_dx = jget_double(req, "dx", &dx) || jget_double(req, "x", &dx);
	bool has_dy = jget_double(req, "dy", &dy) || jget_double(req, "y", &dy);
	if (!has_dx && !has_dy) {
		snprintf(err, errlen, "要 dx / dy（滚轮模式下是格数）");
		return false;
	}
	bool discrete = true;
	jget_bool(req, "discrete", &discrete);
	if (!vidow_inject_pointer_scroll(server, dx, dy, discrete, err, errlen)) {
		return false;
	}
	jb_obj(out);
	jb_key(out, "scroll");
	jb_obj(out);
	jb_kv_double(out, "dx", dx);
	jb_kv_double(out, "dy", dy);
	jb_kv_bool(out, "discrete", discrete);
	jb_obj_end(out);
	jb_obj_end(out);
	return true;
}

/* state = tap（默认）/ press / release；keys = "ctrl+shift+t" / "Return" / "a"。 */
static bool cmd_send_key(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	const char *keys = jget_str(req, "keys");
	if (keys == NULL || keys[0] == '\0') {
		keys = jget_str(req, "key");
	}
	if (keys == NULL || keys[0] == '\0') {
		snprintf(err, errlen, "要 keys（ctrl+shift+t / Return / a）");
		return false;
	}
	const char *state = jget_str(req, "state");
	int mode = 0;
	if (state != NULL) {
		if (strcasecmp(state, "tap") == 0 || strcasecmp(state, "click") == 0) {
			mode = 0;
		} else if (strcasecmp(state, "press") == 0 ||
				strcasecmp(state, "down") == 0) {
			mode = 1;
		} else if (strcasecmp(state, "release") == 0 ||
				strcasecmp(state, "up") == 0) {
			mode = 2;
		} else {
			snprintf(err, errlen, "state 只能是 tap / press / release");
			return false;
		}
	}
	if (!vidow_inject_key_combo(server, keys, mode, err, errlen)) {
		return false;
	}
	jb_obj(out);
	jb_key(out, "key");
	jb_obj(out);
	jb_kv_str(out, "keys", keys);
	jb_kv_str(out, "state", mode == 0 ? "tap" : mode == 1 ? "press" : "release");
	jb_obj_end(out);
	jb_obj_end(out);
	return true;
}

static bool cmd_type_text(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	const char *text = jget_str(req, "text");
	if (text == NULL || text[0] == '\0') {
		snprintf(err, errlen, "要 text（要打的那段字）");
		return false;
	}
	if (!vidow_inject_text(server, text, err, errlen)) {
		return false;
	}
	jb_obj(out);
	jb_key(out, "typed");
	jb_obj(out);
	jb_kv_str(out, "text", text);
	jb_obj_end(out);
	jb_obj_end(out);
	return true;
}

static bool cmd_toggle_overview(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	(void)err;
	(void)errlen;
	vidow_overview_toggle(server);
	jb_obj(out);
	jb_key(out, "overview");
	vidow_ipc_json_overview(out, server);
	jb_obj_end(out);
	return true;
}

static bool cmd_pick_window(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	double x = 0.0, y = 0.0;
	if (!jget_double(req, "x", &x) || !jget_double(req, "y", &y)) {
		snprintf(err, errlen, "要 x / y（布局坐标）");
		return false;
	}
	struct wlr_surface *surface = NULL;
	double sx = 0.0, sy = 0.0;
	struct vidow_view *view = vidow_view_at(server, x, y, &surface, &sx, &sy);
	jb_obj(out);
	jb_key(out, "window");
	vidow_ipc_json_window(out, server, view);
	jb_kv_double(out, "surface_x", sx);
	jb_kv_double(out, "surface_y", sy);
	jb_obj_end(out);
	return true;
}

/* ------------------------------------------------------------- 剪贴板 */

/* base64：JSON 里塞二进制只能这么来（剪贴板里什么都有）。 */
static const char b64_chars[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void jb_kv_base64(struct jbuf *b, const char *key,
		const unsigned char *data, size_t len) {
	jb_key(b, key);
	if (len == 0) {
		jb_str(b, "");
		return;
	}
	size_t out_len = ((len + 2) / 3) * 4;
	char *out = malloc(out_len + 1);
	if (out == NULL) {
		jb_null(b);
		return;
	}
	size_t j = 0;
	for (size_t i = 0; i < len; i += 3) {
		unsigned int v = (unsigned int)data[i] << 16;
		if (i + 1 < len) {
			v |= (unsigned int)data[i + 1] << 8;
		}
		if (i + 2 < len) {
			v |= (unsigned int)data[i + 2];
		}
		out[j++] = b64_chars[(v >> 18) & 0x3f];
		out[j++] = b64_chars[(v >> 12) & 0x3f];
		out[j++] = i + 1 < len ? b64_chars[(v >> 6) & 0x3f] : '=';
		out[j++] = i + 2 < len ? b64_chars[v & 0x3f] : '=';
	}
	out[j] = '\0';
	jb_str(b, out);
	free(out);
}

static int b64_val(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/* 失败返回 NULL（调用方负责报错）。 */
static unsigned char *b64_decode(const char *s, size_t *out_len) {
	size_t slen = strlen(s);
	unsigned char *out = malloc(slen / 4 * 3 + 4);
	if (out == NULL) {
		return NULL;
	}
	size_t n = 0;
	unsigned int acc = 0;
	int bits = 0;
	for (size_t i = 0; i < slen; ++i) {
		if (s[i] == '=' || s[i] == '\n' || s[i] == '\r' || s[i] == ' ') {
			continue;
		}
		int v = b64_val(s[i]);
		if (v < 0) {
			free(out);
			return NULL;
		}
		acc = (acc << 6) | (unsigned int)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out[n++] = (unsigned char)((acc >> bits) & 0xff);
		}
	}
	*out_len = n;
	return out;
}

static bool clip_kind(struct vidow_server *server, struct jval *req, int *kind,
		char *err, size_t errlen) {
	char buf[32];
	const char *sel = jtext(req, "selection", buf, sizeof(buf));
	if (sel == NULL) {
		sel = jtext(req, "clipboard", buf, sizeof(buf));
	}
	if (sel == NULL) {
		*kind = VIDOW_CLIP_CLIPBOARD;
		return true;
	}
	if (strcmp(sel, "primary") == 0 || strcmp(sel, "p") == 0 ||
			strcmp(sel, "PRIMARY") == 0) {
		if (!server->config.clipboard_primary) {
			snprintf(err, errlen, "配置里 clipboard_primary = off");
			return false;
		}
		*kind = VIDOW_CLIP_PRIMARY;
		return true;
	}
	if (strcmp(sel, "clipboard") == 0 || strcmp(sel, "c") == 0 ||
			strcmp(sel, "CLIPBOARD") == 0) {
		*kind = VIDOW_CLIP_CLIPBOARD;
		return true;
	}
	snprintf(err, errlen, "selection 只能是 clipboard / primary");
	return false;
}

static bool cmd_clipboard(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	(void)req;
	(void)err;
	(void)errlen;
	jb_obj(out);
	jb_kv_bool(out, "enabled", server->config.clipboard);
	jb_kv_bool(out, "primary_enabled", server->config.clipboard_primary);
	jb_key(out, "clipboard");
	vidow_ipc_json_clipboard(out, server, &server->clipboard.clip, "state");
	jb_key(out, "primary");
	vidow_ipc_json_clipboard(out, server, &server->clipboard.primary,
		"state");
	jb_obj_end(out);
	return true;
}

static bool cmd_clipboard_get(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	int kind = VIDOW_CLIP_CLIPBOARD;
	if (!clip_kind(server, req, &kind, err, errlen)) {
		return false;
	}
	char mime_buf[256];
	const char *mime = jtext(req, "mime", mime_buf, sizeof(mime_buf));
	if (mime == NULL) {
		mime = jtext(req, "type", mime_buf, sizeof(mime_buf));
	}

	unsigned char *data = NULL;
	size_t len = 0;
	const char *mime_out = NULL;
	if (!vidow_clipboard_get(server, kind, mime, &data, &len, &mime_out)) {
		snprintf(err, errlen, "%s 里没有%s",
			kind == VIDOW_CLIP_PRIMARY ? "primary" : "剪贴板",
			mime != NULL ? mime : "内容");
		return false;
	}

	jb_obj(out);
	jb_kv_str(out, "selection",
		kind == VIDOW_CLIP_PRIMARY ? "primary" : "clipboard");
	jb_kv_str(out, "mime", mime_out);
	jb_kv_uint(out, "size", len);
	if (vidow_clipboard_mime_is_text(mime_out)) {
		size_t n = len;
		while (n > 0 && data[n - 1] == '\0') {
			n--;
		}
		char *text = malloc(n + 1);
		if (text != NULL) {
			memcpy(text, data, n);
			text[n] = '\0';
			jb_kv_str(out, "text", text);
			free(text);
		}
	}
	if (jget_bool(req, "base64", false) ||
			!vidow_clipboard_mime_is_text(mime_out)) {
		jb_kv_base64(out, "base64", data, len);
	}
	jb_obj_end(out);
	free(data);
	return true;
}

static bool cmd_clipboard_set(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	int kind = VIDOW_CLIP_CLIPBOARD;
	if (!clip_kind(server, req, &kind, err, errlen)) {
		return false;
	}
	char mime_buf[256];
	const char *mime = jtext(req, "mime", mime_buf, sizeof(mime_buf));
	if (mime == NULL) {
		mime = jtext(req, "type", mime_buf, sizeof(mime_buf));
	}
	if (mime == NULL) {
		mime = "text/plain;charset=utf-8";
	}

	const char *text = jget_str(req, "text");
	const char *b64 = jget_str(req, "base64");
	unsigned char *data = NULL;
	size_t len = 0;
	bool owned = false;
	if (b64 != NULL) {
		data = b64_decode(b64, &len);
		if (data == NULL) {
			snprintf(err, errlen, "base64 解不开");
			return false;
		}
		owned = true;
	} else if (text != NULL) {
		data = (unsigned char *)text;
		len = strlen(text);
	} else {
		snprintf(err, errlen, "要 text 或者 base64");
		return false;
	}

	bool ok = vidow_clipboard_set(server, kind, mime, data, len);
	if (owned) {
		free(data);
	}
	if (!ok) {
		snprintf(err, errlen, "塞不进去（剪贴板关了？类型太多？）");
		return false;
	}
	wlr_log(WLR_INFO, "ipc: %s = %s（%zu 字节）",
		kind == VIDOW_CLIP_PRIMARY ? "primary" : "剪贴板", mime, len);
	jb_obj(out);
	jb_kv_str(out, "selection",
		kind == VIDOW_CLIP_PRIMARY ? "primary" : "clipboard");
	jb_kv_str(out, "mime", mime);
	jb_kv_uint(out, "size", len);
	jb_obj_end(out);
	return true;
}

static bool cmd_clipboard_clear(struct vidow_server *server, struct jval *req,
		struct jbuf *out, char *err, size_t errlen) {
	int kind = VIDOW_CLIP_CLIPBOARD;
	if (!clip_kind(server, req, &kind, err, errlen)) {
		return false;
	}
	vidow_clipboard_clear(server, kind);
	wlr_log(WLR_INFO, "ipc: 清空 %s",
		kind == VIDOW_CLIP_PRIMARY ? "primary" : "剪贴板");
	jb_obj(out);
	jb_kv_str(out, "selection",
		kind == VIDOW_CLIP_PRIMARY ? "primary" : "clipboard");
	jb_kv_bool(out, "cleared", true);
	jb_obj_end(out);
	return true;
}

/* --------------------------------------------------------- 命令总表 */

static bool ipc_run(struct vidow_server *server, struct ipc_client *c,
		const char *cmd, struct jval *req, struct jbuf *out, char *err,
		size_t errlen) {
	/* 查询 */
	if (strcmp(cmd, "state") == 0) {
		vidow_ipc_json_state(out, server);
		return true;
	}
	if (strcmp(cmd, "version") == 0) {
		json_version(out, server);
		return true;
	}
	if (strcmp(cmd, "ping") == 0) {
		jb_obj(out);
		jb_kv_bool(out, "pong", true);
		jb_obj_end(out);
		return true;
	}
	if (strcmp(cmd, "focused") == 0) {
		vidow_ipc_json_focused(out, server);
		return true;
	}
	if (strcmp(cmd, "windows") == 0) {
		vidow_ipc_json_windows(out, server);
		return true;
	}
	if (strcmp(cmd, "columns") == 0) {
		vidow_ipc_json_columns(out, server);
		return true;
	}
	if (strcmp(cmd, "workspaces") == 0) {
		vidow_ipc_json_workspaces(out, server);
		return true;
	}
	if (strcmp(cmd, "outputs") == 0) {
		vidow_ipc_json_outputs(out, server);
		return true;
	}
	if (strcmp(cmd, "layers") == 0) {
		vidow_ipc_json_layers(out, server);
		return true;
	}
	if (strcmp(cmd, "keybinds") == 0) {
		vidow_ipc_json_keybinds(out, server);
		return true;
	}
	if (strcmp(cmd, "rules") == 0) {
		vidow_ipc_json_rules(out, server);
		return true;
	}
	if (strcmp(cmd, "config") == 0 || strcmp(cmd, "get_config") == 0) {
		return cmd_get_config(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "seats") == 0) {
		vidow_ipc_json_seats(out, server);
		return true;
	}
	if (strcmp(cmd, "input") == 0) {
		vidow_ipc_json_input(out, server);
		return true;
	}
	if (strcmp(cmd, "cursor") == 0) {
		jb_obj(out);
		jb_kv_double(out, "x", server->cursor->x);
		jb_kv_double(out, "y", server->cursor->y);
		/* 摇晃放大的状态：magnified / scale / image / hardware / hw_max */
		vidow_cursor_fx_json(out, server);
		jb_obj_end(out);
		return true;
	}
	if (strcmp(cmd, "tokens") == 0) {
		vidow_ipc_json_tokens(out, server);
		return true;
	}
	if (strcmp(cmd, "memories") == 0) {
		vidow_ipc_json_memories(out, server);
		return true;
	}
	if (strcmp(cmd, "anims") == 0) {
		vidow_ipc_json_anims(out, server);
		return true;
	}
	if (strcmp(cmd, "im") == 0) {
		vidow_ipc_json_im(out, server);
		return true;
	}
	if (strcmp(cmd, "overview") == 0) {
		vidow_ipc_json_overview(out, server);
		return true;
	}
	if (strcmp(cmd, "mru") == 0 || strcmp(cmd, "recent") == 0) {
		vidow_ipc_json_mru(out, server);
		return true;
	}
	if (strcmp(cmd, "xwayland") == 0) {
		vidow_ipc_json_xwayland(out, server);
		return true;
	}
	if (strcmp(cmd, "clipboard") == 0) {
		return cmd_clipboard(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "clipboard_get") == 0) {
		return cmd_clipboard_get(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "clipboard_set") == 0) {
		return cmd_clipboard_set(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "clipboard_clear") == 0) {
		return cmd_clipboard_clear(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "pick_window") == 0) {
		return cmd_pick_window(server, req, out, err, errlen);
	}

	/* 窗口 */
	if (strcmp(cmd, "focus_window") == 0) {
		return cmd_focus_window(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "activate_window") == 0) {
		return cmd_activate_window(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "close_window") == 0) {
		return cmd_close_window(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "minimize_window") == 0) {
		return cmd_minimize_window(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "unminimize_window") == 0) {
		return cmd_unminimize_window(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "toggle_minimize") == 0) {
		return cmd_toggle_minimize(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "toggle_floating") == 0) {
		return cmd_toggle_floating(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "set_floating") == 0) {
		return cmd_set_floating(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "toggle_fullscreen") == 0) {
		return cmd_toggle_fullscreen(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "set_fullscreen") == 0) {
		return cmd_set_fullscreen(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "move_window_to_workspace") == 0) {
		return cmd_move_window_to_workspace(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "move_window_to_special") == 0) {
		return cmd_move_window_to_special(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "move_window_to_output") == 0) {
		return cmd_move_window_to_output(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "set_window_size") == 0) {
		return cmd_set_window_size(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "set_window_position") == 0) {
		return cmd_set_window_position(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "center_window") == 0) {
		return cmd_center_window(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "set_window_force_render") == 0) {
		return cmd_set_window_force_render(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "focus_window_direction") == 0) {
		return cmd_focus_window_direction(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "move_window_direction") == 0) {
		return cmd_move_window_direction(server, req, out, err, errlen);
	}

	/* 列 */
	if (strcmp(cmd, "focus_column") == 0) {
		return cmd_focus_column(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "move_column") == 0) {
		return cmd_move_column(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "cycle_column_width") == 0) {
		return cmd_cycle_column_width(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "set_column_width") == 0) {
		return cmd_set_column_width(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "expand_column_to_available_width") == 0) {
		return cmd_expand_column_to_available_width(server, req, out, err,
			errlen);
	}
	if (strcmp(cmd, "center_column") == 0) {
		return cmd_center_column(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "toggle_maximize_column") == 0) {
		return cmd_toggle_maximize_column(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "set_maximize_column") == 0) {
		return cmd_set_maximize_column(server, req, out, err, errlen);
	}

	/* 工作区 */
	if (strcmp(cmd, "focus_workspace_previous") == 0 ||
			strcmp(cmd, "focus_workspace_last") == 0) {
		return cmd_focus_workspace_previous(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "move_workspace_up") == 0) {
		return cmd_move_workspace_step(server, req, out, err, errlen, -1);
	}
	if (strcmp(cmd, "move_workspace_down") == 0) {
		return cmd_move_workspace_step(server, req, out, err, errlen, 1);
	}
	if (strcmp(cmd, "set_workspace_name") == 0 ||
			strcmp(cmd, "unset_workspace_name") == 0) {
		return cmd_set_workspace_name(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "move_window_to_workspace_up") == 0) {
		return cmd_move_window_to_workspace_step(server, req, out, err,
			errlen, -1);
	}
	if (strcmp(cmd, "move_window_to_workspace_down") == 0) {
		return cmd_move_window_to_workspace_step(server, req, out, err,
			errlen, 1);
	}
	if (strcmp(cmd, "move_column_to_workspace") == 0) {
		/* niri 的名字。vidow 的列是单窗口的，所以和 move_window_* 一回事。 */
		return cmd_move_window_to_workspace(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "focus_workspace") == 0) {
		return cmd_focus_workspace(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "toggle_special") == 0 ||
			strcmp(cmd, "focus_special") == 0) {
		return cmd_toggle_special(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "move_workspace_to_output") == 0) {
		return cmd_move_workspace_to_output(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "create_workspace") == 0) {
		return cmd_create_workspace(server, req, out, err, errlen);
	}

	/* 输出 */
	if (strcmp(cmd, "output_power") == 0 ||
			strcmp(cmd, "output_enable") == 0 ||
			strcmp(cmd, "output_disable") == 0 ||
			strcmp(cmd, "output_toggle") == 0) {
		return cmd_output_power(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "output_scale") == 0) {
		return cmd_output_scale(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "output_transform") == 0) {
		return cmd_output_transform(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "output_mode") == 0) {
		return cmd_output_mode(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "output_position") == 0) {
		return cmd_output_position(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "output_focus") == 0) {
		return cmd_output_focus(server, req, out, err, errlen);
	}

	/* 合成器 */
	if (strcmp(cmd, "action") == 0) {
		return cmd_action(server, req, out, err, errlen);
	}
	/* 事件（见 hooks.c）：谁在听、有哪些事件、自己发一个 */
	if (strcmp(cmd, "hooks") == 0) {
		return cmd_hooks(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "events") == 0) {
		return cmd_events(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "emit") == 0) {
		return cmd_emit(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "set_config") == 0) {
		return cmd_set_config(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "reload_config") == 0) {
		return cmd_reload_config(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "quit") == 0) {
		return cmd_quit(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "move_cursor") == 0) {
		return cmd_move_cursor(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "cursor_shake") == 0) {
		return cmd_cursor_shake(server, req, out, err, errlen);
	}
	/* 输入注入（合成鼠标 / 键盘事件，见上面那一节） */
	if (strcmp(cmd, "pointer_button") == 0) {
		return cmd_pointer_button(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "pointer_scroll") == 0) {
		return cmd_pointer_scroll(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "send_key") == 0) {
		return cmd_send_key(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "type_text") == 0) {
		return cmd_type_text(server, req, out, err, errlen);
	}
	if (strcmp(cmd, "toggle_overview") == 0) {
		return cmd_toggle_overview(server, req, out, err, errlen);
	}

	/* 订阅：之后的这条连接上来的全是事件行。 */
	if (strcmp(cmd, "subscribe") == 0) {
		if (!c->subscribed) {
			c->subscribed = true;
			server->ipc_subscribers++;
		}
		struct jval *events = jget(req, "events");
		if (events != NULL && events->t == J_ARR) {
			for (size_t i = 0; i < events->n; i++) {
				if (events->items[i]->t != J_STR) {
					continue;
				}
				char **p = realloc(c->filter,
					(c->filter_n + 1) * sizeof(*p));
				if (p == NULL) {
					break;
				}
				c->filter = p;
				c->filter[c->filter_n] = strdup(events->items[i]->str);
				if (c->filter[c->filter_n] == NULL) {
					break;
				}
				c->filter_n++;
			}
		}
		jb_obj(out);
		jb_kv_bool(out, "subscribed", true);
		jb_key(out, "events");
		if (c->filter_n == 0) {
			jb_str(out, "*");
		} else {
			jb_arr(out);
			for (size_t i = 0; i < c->filter_n; i++) {
				jb_str(out, c->filter[i]);
			}
			jb_arr_end(out);
		}
		jb_obj_end(out);
		return true;
	}
	if (strcmp(cmd, "unsubscribe") == 0) {
		if (c->subscribed) {
			c->subscribed = false;
			server->ipc_subscribers--;
		}
		jb_obj(out);
		jb_kv_bool(out, "subscribed", false);
		jb_obj_end(out);
		return true;
	}

	snprintf(err, errlen,
		"没有 '%s' 这个命令（vidowmsg help 里有清单）", cmd);
	return false;
}

/* 一条请求 -> 一条应答。 */
static void ipc_handle_line(struct ipc_client *c, const char *line) {
	struct vidow_server *server = c->server;
	char err[256] = "";
	struct jbuf result;
	jb_init(&result, 4096);
	bool ok = false;

	struct jval *req = jparse(line, err, sizeof(err));
	if (req == NULL) {
		/* 解析失败：err 里已经有位置了 */
	} else if (req->t != J_OBJ) {
		snprintf(err, sizeof(err), "请求得是一个 JSON 对象");
	} else {
		const char *cmd = jget_str(req, "cmd");
		if (cmd == NULL) {
			cmd = jget_str(req, "command");
		}
		if (cmd == NULL) {
			snprintf(err, sizeof(err), "请求里要有个 \"cmd\"");
		} else {
			/* 命令名里的 '-' 一律当 '_' 看：toggle-floating 和
			 * toggle_floating 是同一条命令。 */
			char name[64];
			size_t i = 0;
			for (; cmd[i] != '\0' && i < sizeof(name) - 1; i++) {
				name[i] = cmd[i] == '-' ? '_' : cmd[i];
			}
			name[i] = '\0';
			ok = ipc_run(server, c, name, req, &result, err, sizeof(err));
		}
	}
	jfree(req);

	struct jbuf resp;
	jb_init(&resp, result.len + 512);
	jb_obj(&resp);
	jb_kv_bool(&resp, "ok", ok);
	if (ok) {
		jb_key(&resp, "result");
		jb_raw(&resp, jb_ok(&result) ? result.buf : "null");
	} else {
		jb_kv_str(&resp, "error", err[0] != '\0' ? err : "命令没做成");
	}
	jb_obj_end(&resp);
	jb_raw(&resp, "\n");
	if (jb_ok(&resp)) {
		ipc_client_out(c, resp.buf, resp.len);
	} else {
		wlr_log(WLR_ERROR, "ipc: 应答拼不出来（内存不够？）");
		c->dead = true;
	}
	jb_free(&resp);
	jb_free(&result);
}

/* --------------------------------------------------------------- 收发 */

static bool ipc_client_read(struct ipc_client *c) {
	char tmp[4096];
	for (;;) {
		ssize_t n = read(c->fd, tmp, sizeof(tmp));
		if (n > 0) {
			size_t need = c->in_len + (size_t)n;
			if (need > c->in_cap) {
				size_t cap = c->in_cap ? c->in_cap : 4096;
				while (cap < need) {
					cap *= 2;
				}
				char *p = realloc(c->in, cap);
				if (p == NULL) {
					c->dead = true;
					return false;
				}
				c->in = p;
				c->in_cap = cap;
			}
			memcpy(c->in + c->in_len, tmp, (size_t)n);
			c->in_len += (size_t)n;
			continue;
		}
		if (n == 0) {
			c->dead = true; /* 对面关了 */
			return false;
		}
		if (errno == EINTR) {
			continue;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			break;
		}
		c->dead = true;
		return false;
	}

	/* 一行一条请求。 */
	for (;;) {
		char *nl = memchr(c->in, '\n', c->in_len);
		if (nl == NULL) {
			break;
		}
		size_t len = (size_t)(nl - c->in);
		char *line = malloc(len + 1);
		if (line == NULL) {
			c->dead = true;
			return false;
		}
		memcpy(line, c->in, len);
		line[len] = '\0';
		memmove(c->in, nl + 1, c->in_len - len - 1);
		c->in_len -= len + 1;
		ipc_handle_line(c, line);
		free(line);
		if (c->dead || c->want_close) {
			return !c->dead;
		}
	}
	if (c->in_len > IPC_MAX_LINE) {
		/* 一行太长：报错然后断开（剩下的字节不看了）。 */
		static const char too_long[] =
			"{\"ok\":false,\"error\":\"这一行太长了（上限 1 MiB）\"}\n";
		ipc_client_out(c, too_long, sizeof(too_long) - 1);
		c->in_len = 0;
		c->want_close = true;
	}
	return true;
}

static int ipc_client_event(int fd, uint32_t mask, void *data) {
	(void)fd;
	struct ipc_client *c = data;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
		ipc_client_destroy(c);
		return 0;
	}
	if (mask & WL_EVENT_READABLE) {
		if (!ipc_client_read(c)) {
			if (c->dead) {
				ipc_client_destroy(c);
				return 0;
			}
		}
	}
	if (mask & WL_EVENT_WRITABLE) {
		ipc_client_flush(c);
	}
	if (c->dead || (c->want_close && c->out_sent == c->out_len)) {
		ipc_client_destroy(c);
	}
	return 0;
}

static void ipc_new_client(struct vidow_server *server, int fd) {
	size_t count = (size_t)wl_list_length(&server->ipc_clients);
	if (count >= IPC_MAX_CLIENTS) {
		wlr_log(WLR_ERROR, "ipc: 连接太多了（%zu），拒掉一个", count);
		close(fd);
		return;
	}
	struct ipc_client *c = calloc(1, sizeof(*c));
	if (c == NULL) {
		close(fd);
		return;
	}
	c->server = server;
	c->fd = fd;
	c->source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), fd, WL_EVENT_READABLE,
		ipc_client_event, c);
	if (c->source == NULL) {
		close(fd);
		free(c);
		return;
	}
	wl_list_insert(server->ipc_clients.prev, &c->link);
}

static int ipc_accept(int fd, uint32_t mask, void *data) {
	struct vidow_server *server = data;
	if (!(mask & WL_EVENT_READABLE)) {
		return 0;
	}
	for (;;) {
		int client = accept(fd, NULL, NULL);
		if (client < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
				break;
			}
			wlr_log(WLR_ERROR, "ipc: accept 失败：%s", strerror(errno));
			break;
		}
		fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);
		fcntl(client, F_SETFD, fcntl(client, F_GETFD, 0) | FD_CLOEXEC);
		ipc_new_client(server, client);
	}
	return 0;
}

/* --------------------------------------------------------------- 广播 */

bool vidow_ipc_wanted(const struct vidow_server *server) {
	return server != NULL && server->ipc_subscribers > 0;
}

bool vidow_event_wanted(const struct vidow_server *server, const char *event) {
	if (vidow_ipc_wanted(server)) {
		return true;
	}
	/* 没有订阅者，但配置里可能挂着钩子（vidow.on）：事件照样得发出来 */
	return vidow_hooks_wanted(server, event);
}

void vidow_ipc_broadcast(struct vidow_server *server, const char *event,
		const char *line, size_t len) {
	if (!vidow_ipc_wanted(server)) {
		return;
	}
	struct ipc_client *c, *tmp;
	wl_list_for_each_safe(c, tmp, &server->ipc_clients, link) {
		if (!c->subscribed || c->dead) {
			continue;
		}
		if (c->filter_n > 0) {
			bool want = false;
			for (size_t i = 0; i < c->filter_n; i++) {
				if (strcmp(c->filter[i], event) == 0) {
					want = true;
					break;
				}
			}
			if (!want) {
				continue;
			}
		}
		ipc_client_out(c, line, len);
		if (c->dead) {
			ipc_client_destroy(c);
		}
	}
}

/* --------------------------------------------------------------- 起停 */

static void ipc_path_build(struct vidow_server *server, char *buf, size_t len) {
	if (server->config.ipc_socket[0] != '\0') {
		snprintf(buf, len, "%s", server->config.ipc_socket);
		return;
	}
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const char *display = getenv("WAYLAND_DISPLAY");
	if (runtime != NULL && runtime[0] != '\0' && display != NULL &&
			display[0] != '\0') {
		snprintf(buf, len, "%s/vidow-%s.sock", runtime, display);
		return;
	}
	snprintf(buf, len, "/tmp/vidow-%u.sock", (unsigned)getuid());
}

const char *vidow_ipc_path(const struct vidow_server *server) {
	return server->ipc_path;
}

bool vidow_ipc_init(struct vidow_server *server) {
	wl_list_init(&server->ipc_clients);
	server->ipc_path[0] = '\0';
	server->ipc_subscribers = 0;
	return vidow_ipc_apply_config(server);
}

/* 建 / 拆 / 换 socket，看配置怎么说。配置是 Lua，跑得比这里晚（要等合成器起
 * 得来），所以配置落地后还要再调一次：ipc = false 就拆掉，改了 ipc_socket 就
 * 换一个。已经在听、路径也没变就什么都不做。 */
bool vidow_ipc_apply_config(struct vidow_server *server) {
	char path[PATH_MAX];
	ipc_path_build(server, path, sizeof(path));

	if (server->ipc_listen != NULL && server->config.ipc &&
			strcmp(path, server->ipc_path) == 0) {
		return true; /* 没变 */
	}
	if (server->ipc_listen != NULL) {
		vidow_ipc_finish(server);
	}

	if (!server->config.ipc) {
		wlr_log(WLR_INFO, "ipc: 关着（配置里 ipc = false）");
		return true;
	}

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "ipc: 建不了 socket：%s", strerror(errno));
		return false;
	}
	struct sockaddr_un addr = {0};
	addr.sun_family = AF_UNIX;
	size_t path_len = strlen(path);
	if (path_len >= sizeof(addr.sun_path)) {
		wlr_log(WLR_ERROR, "ipc: socket 路径太长（上限 %zu 字节）：%s",
			sizeof(addr.sun_path) - 1, path);
		close(fd);
		return false;
	}
	memcpy(addr.sun_path, path, path_len + 1);

	/* 已经有一个在听 = 另一个实例在跑：别去抢它的 socket。 */
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
		wlr_log(WLR_ERROR, "ipc: %s 上已经有一个 vidow 在听了", path);
		close(fd);
		return false;
	}
	unlink(path);
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		wlr_log(WLR_ERROR, "ipc: bind %s 失败：%s", path, strerror(errno));
		close(fd);
		return false;
	}
	/* 只有自己这个用户能连：IPC 能操作合成器，权限就是它的安全边界。 */
	if (chmod(path, 0600) != 0) {
		wlr_log(WLR_ERROR, "ipc: chmod %s 失败：%s", path, strerror(errno));
	}
	if (listen(fd, 32) != 0) {
		wlr_log(WLR_ERROR, "ipc: listen 失败：%s", strerror(errno));
		close(fd);
		unlink(path);
		return false;
	}
	server->ipc_listen = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display), fd, WL_EVENT_READABLE,
		ipc_accept, server);
	if (server->ipc_listen == NULL) {
		close(fd);
		unlink(path);
		return false;
	}
	snprintf(server->ipc_path, sizeof(server->ipc_path), "%s", path);
	wlr_log(WLR_INFO, "ipc: 听 %s（一行一个 JSON 请求，客户端是 vidowmsg）",
		server->ipc_path);
	return true;
}

void vidow_ipc_finish(struct vidow_server *server) {
	struct ipc_client *c, *tmp;
	wl_list_for_each_safe(c, tmp, &server->ipc_clients, link) {
		ipc_client_destroy(c);
	}
	if (server->ipc_listen != NULL) {
		wl_event_source_remove(server->ipc_listen);
		server->ipc_listen = NULL;
	}
	if (server->ipc_path[0] != '\0') {
		unlink(server->ipc_path);
		server->ipc_path[0] = '\0';
	}
}
