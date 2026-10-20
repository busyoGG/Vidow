/* hooks.c — 事件钩子：谁在听、怎么派发、命令钩子的环境变量。见 hooks.h。
 *
 * 三条路一起喂：IPC 订阅者拿一条 JSON（ipc.c 的广播）、命令钩子起一条子进程、
 * Lua 钩子在事件循环里跑一个函数。事件本身只有一个来源（ipcstate.c 的 emit()），
 * 所以谁都不会漏事件、也不会因为「没人订阅」而白建 JSON。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>

#include "hooks.h"
#include "json.h"
#include "server.h"

/* 全部事件名。IPC 的 {"cmd":"events"}、README 的事件表、测试的断言都从这儿来。
 *
 * 命名跟 IPC 事件一致（ipc.h 的事件一节），小写 + 连字符：
 *   window-*    窗口：开 / 关 / 标题 / 位置 / 浮动 / 全屏 / 最小化 / 换工作区 / 焦点
 *   column-*    列：最大化（列宽跟着变）
 *   workspace-* 工作区：建 / 拆 / 改 / 激活 / 特殊工作区
 *   output-*    显示器：插 / 拔 / 改（模式、缩放、位置、开关都算「改」）
 *   layer-*     layer surface（状态栏那种）
 *   clipboard-* / primary-*   剪贴板 / 主选区
 *   mru-*       Alt+Tab 切换器
 *   其余：overview-toggled（全景）/ window-activated（应用喊「看我」）/
 *         drag-started + drag-ended（拖窗口）/ keybind（按键触发的绑定）/
 *         ipc-action（从 IPC 来的动作）/ input-device-added + input-device-removed
 *         （插拔输入设备）/ xwayland-ready / screencast-started +
 *         screencast-stopped（录屏起止）/ output-power-changed（屏开关）/
 *         config-reloaded + config-failed
 *
 * 自定义事件（IPC 的 emit 命令）不在名单里：名字由调用方定，钩子照样能听。 */
static const char *event_names[] = {
	"window-opened",
	"window-closed",
	"window-title-changed",
	"window-moved",
	"window-floating-changed",
	"window-fullscreen-changed",
	"window-minimized",
	"window-unminimized",
	"window-workspace-changed",
	"window-focus-changed",
	/* 小窗（mini window，S1）：形态变了（none / free / mini / icon）。载荷同
	 * window-floating-changed，多带 mini / mini_output / mini_anchor。 */
	"window-mini-changed",
	"column-maximize-changed",
	"workspace-created",
	"workspace-removed",
	"workspace-changed",
	"workspace-activated",
	"workspace-special-changed",
	"output-added",
	"output-removed",
	"output-changed",
	"layer-mapped",
	"layer-unmapped",
	"layer-closed",
	"background-changed",
	"clipboard-changed",
	"primary-changed",
	"clipboard-history-changed",
	"mru-shown",
	"mru-opened",
	"mru-committed",
	"mru-cancelled",
	"overview-toggled",
	"window-activated",
	"drag-started",
	"drag-ended",
	"keybind",
	"ipc-action",
	"input-device-added",
	"input-device-removed",
	"xwayland-ready",
	"screencast-started",
	"screencast-stopped",
	"output-power-changed",
	"config-reloaded",
	"config-failed",
};

size_t vidow_event_names(const char *const **out) {
	if (out != NULL) {
		*out = event_names;
	}
	return sizeof(event_names) / sizeof(event_names[0]);
}

/* ------------------------------------------------------------- 注册表 */

static bool event_matches(const struct vidow_hook *h, const char *event) {
	return strcmp(h->event, "*") == 0 || strcmp(h->event, event) == 0;
}

/* 有多少条钩子会收到这个事件（IPC 的 emit 命令回给调用方看）。 */
int vidow_hooks_matching(const struct vidow_server *server, const char *event) {
	if (server == NULL || server->hook_count == 0 || event == NULL) {
		return 0;
	}
	int n = 0;
	struct vidow_hook *h;
	wl_list_for_each(h, &server->hooks, link) {
		if (!h->dead && event_matches(h, event)) {
			n++;
		}
	}
	return n;
}

bool vidow_hooks_wanted(const struct vidow_server *server, const char *event) {
	return vidow_hooks_matching(server, event) > 0;
}

static void hook_destroy(struct vidow_server *server, struct vidow_hook *h) {
	(void)server;
	/* Lua 那个引用要还回去 —— 但只在 Lua 状态还活着的时候（配置重载时钩子先
	 * 清、Lua 状态后关，见 script.c 的 vidow_script_finish）。 */
	if (h->lua && h->lua_state != NULL && h->lua_ref != 0 &&
			vidow_script_alive(server, h->lua_state)) {
		vidow_script_hook_unref(h->lua_state, h->lua_ref);
	}
	wl_list_remove(&h->link);
	free(h);
}

/* 派发途中被摘掉的钩子：当场不能 free（正在遍历），统一在这里回收。 */
static void hooks_sweep(struct vidow_server *server) {
	if (server->hook_dispatch_depth > 0) {
		return;
	}
	struct vidow_hook *h, *tmp;
	wl_list_for_each_safe(h, tmp, &server->hooks, link) {
		if (h->dead) {
			hook_destroy(server, h);
		}
	}
}

static int hook_add(struct vidow_server *server, const char *event,
		const char *cmd, void *lua_state, int lua_ref) {
	if (server == NULL || event == NULL || event[0] == '\0') {
		return 0;
	}
	if (strlen(event) >= sizeof(((struct vidow_hook *)0)->event)) {
		wlr_log(WLR_ERROR, "钩子：事件名 '%s' 太长", event);
		return 0;
	}
	if (!lua_state && (cmd == NULL || cmd[0] == '\0')) {
		return 0;
	}
	struct vidow_hook *h = calloc(1, sizeof(*h));
	if (h == NULL) {
		return 0;
	}
	h->id = server->hook_next_id++;
	if (server->hook_next_id <= 0) {
		server->hook_next_id = 1;
	}
	snprintf(h->event, sizeof(h->event), "%s", event);
	h->lua = lua_state != NULL;
	h->lua_state = lua_state;
	h->lua_ref = lua_ref;
	if (cmd != NULL) {
		snprintf(h->cmd, sizeof(h->cmd), "%s", cmd);
		/* 末尾的换行会让 sh 少跑半条命令，去掉 */
		size_t n = strlen(h->cmd);
		while (n > 0 && (h->cmd[n - 1] == '\n' || h->cmd[n - 1] == '\r')) {
			h->cmd[--n] = '\0';
		}
		if (n == 0) {
			free(h);
			return 0;
		}
	}
	wl_list_insert(server->hooks.prev, &h->link);
	server->hook_count++;
	return h->id;
}

int vidow_hook_add_cmd(struct vidow_server *server, const char *event,
		const char *cmd) {
	return hook_add(server, event, cmd, NULL, 0);
}

int vidow_hook_add_lua(struct vidow_server *server, const char *event,
		void *lua_state, int lua_ref) {
	return hook_add(server, event, NULL, lua_state, lua_ref);
}

bool vidow_hook_remove(struct vidow_server *server, int id) {
	if (server == NULL || id <= 0) {
		return false;
	}
	struct vidow_hook *h;
	wl_list_for_each(h, &server->hooks, link) {
		if (h->id != id || h->dead) {
			continue;
		}
		h->dead = true;
		server->hook_count--;
		if (server->hook_dispatch_depth == 0) {
			hook_destroy(server, h);
		}
		return true;
	}
	return false;
}

void vidow_hooks_clear(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	struct vidow_hook *h, *tmp;
	wl_list_for_each_safe(h, tmp, &server->hooks, link) {
		h->dead = true;
		server->hook_count--;
		if (server->hook_dispatch_depth == 0) {
			hook_destroy(server, h);
		}
	}
	server->hook_count = 0;
	/* 排着队还没派发的事件不算数了：钩子都没了 */
	struct vidow_hook_event *q, *qtmp;
	wl_list_for_each_safe(q, qtmp, &server->hook_queue, link) {
		wl_list_remove(&q->link);
		free(q->event);
		free(q->json);
		free(q);
	}
	server->hook_queue_len = 0;
}

/* ------------------------------------------------- 命令钩子的环境变量 */

/* 事件字段变成环境变量，命令里直接用：
 *
 *   vidow.on("window-opened", "notify-send \"$VIDOW_APP_ID\" \"$VIDOW_TITLE\"")
 *
 * 给的变量（data 里每个标量字段一条）：
 *   VIDOW_EVENT          事件名
 *   VIDOW_EVENT_TIME_MS  事件时间（vidow 起算的毫秒）
 *   VIDOW_EVENT_JSON     整个信封的 JSON（要什么有什么，别的不够就用它）
 *   VIDOW_<字段名大写>    data.<字段>，连字符换成下划线
 *
 * 只认标量（字符串 / 数字 / 真假）：数组和对象跳过（它们在 VIDOW_EVENT_JSON 里）。
 * 值截到 512 字节，名字只留 [A-Z0-9_]，免得奇怪的键把子进程环境搞脏。 */
#define HOOK_ENV_MAX 64
#define HOOK_ENV_VALUE_MAX 512

struct saved_env {
	char *name;
	char *old;   /* NULL = 原来没有这个变量 */
};

static void env_put(struct saved_env *sv, int *n, const char *name,
		const char *value) {
	if (*n >= HOOK_ENV_MAX) {
		return;
	}
	const char *old = getenv(name);
	sv[*n].name = strdup(name);
	sv[*n].old = old != NULL ? strdup(old) : NULL;
	(*n)++;
	setenv(name, value, 1);
}

static void env_restore(struct saved_env *sv, int n) {
	for (int i = 0; i < n; i++) {
		if (sv[i].old != NULL) {
			setenv(sv[i].name, sv[i].old, 1);
		} else {
			unsetenv(sv[i].name);
		}
		free(sv[i].name);
		free(sv[i].old);
	}
}

static bool env_name_for(const char *key, char *out, size_t outlen) {
	size_t n = 0;
	const char *prefix = "VIDOW_";
	for (size_t i = 0; prefix[i] != '\0'; i++) {
		if (n + 1 >= outlen) {
			return false;
		}
		out[n++] = prefix[i];
	}
	for (size_t i = 0; key[i] != '\0'; i++) {
		char c = key[i];
		if (c == '-') {
			c = '_';
		}
		c = (char)toupper((unsigned char)c);
		if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) {
			return false;
		}
		if (n + 1 >= outlen) {
			return false;
		}
		out[n++] = c;
	}
	out[n] = '\0';
	return n > 6; /* 光 "VIDOW_" 不算 */
}

static bool scalar_text(const struct jval *v, char *out, size_t outlen) {
	switch (v->t) {
	case J_STR:
		snprintf(out, outlen, "%s", v->str != NULL ? v->str : "");
		return true;
	case J_BOOL:
		snprintf(out, outlen, "%s", v->b ? "true" : "false");
		return true;
	case J_NUM: {
		double d = v->num;
		if (d == (double)(long long)d && d > -1e15 && d < 1e15) {
			snprintf(out, outlen, "%lld", (long long)d);
		} else {
			snprintf(out, outlen, "%.6g", d);
		}
		return true;
	}
	case J_NULL:
		out[0] = '\0';
		return true;
	default:
		return false; /* 数组 / 对象：见 VIDOW_EVENT_JSON */
	}
}

static void hook_run_cmd(struct vidow_server *server, struct vidow_hook *h,
		const char *event, const char *json) {
	(void)server;
	struct saved_env sv[HOOK_ENV_MAX];
	int n = 0;
	char buf[64];

	env_put(sv, &n, "VIDOW_EVENT", event);
	env_put(sv, &n, "VIDOW_EVENT_JSON", json);

	struct jval *env = jparse(json, NULL, 0);
	if (env != NULL) {
		long long ms = 0;
		if (jget_int(env, "time_ms", &ms)) {
			snprintf(buf, sizeof(buf), "%lld", ms);
			env_put(sv, &n, "VIDOW_EVENT_TIME_MS", buf);
		}
		const struct jval *data = jget(env, "data");
		if (data != NULL && data->t == J_OBJ) {
			char name[128];
			char text[HOOK_ENV_VALUE_MAX];
			for (size_t i = 0; i < data->n; i++) {
				if (!env_name_for(data->keys[i], name, sizeof(name))) {
					continue;
				}
				if (!scalar_text(data->items[i], text, sizeof(text))) {
					continue;
				}
				env_put(sv, &n, name, text);
			}
		}
		jfree(env);
	}

	wlr_log(WLR_DEBUG, "钩子 #%d：%s -> %s", h->id, event, h->cmd);
	vidow_exec_detached(h->cmd);
	env_restore(sv, n);
}

/* --------------------------------------------------------------- 派发 */

static void hook_idle_cb(void *data);

static void run_event(struct vidow_server *server, const char *event,
		const char *json) {
	struct vidow_hook *h, *tmp;
	wl_list_for_each_safe(h, tmp, &server->hooks, link) {
		if (h->dead || !event_matches(h, event)) {
			continue;
		}
		h->fired++;
		h->last_ms = vidow_now_ns() / 1000000;
		if (h->lua) {
			if (!vidow_script_hook_run(server, h, json)) {
				wlr_log(WLR_ERROR, "钩子 #%d（%s，Lua）：这次没跑起来",
					h->id, h->event);
			}
		} else {
			hook_run_cmd(server, h, event, json);
		}
	}
}

static void hooks_arm_idle(struct vidow_server *server) {
	if (server->hook_idle != NULL || server->display == NULL) {
		return;
	}
	server->hook_idle = wl_event_loop_add_idle(
		wl_display_get_event_loop(server->display), hook_idle_cb, server);
}

static void hook_idle_cb(void *data) {
	struct vidow_server *server = data;
	server->hook_idle = NULL;
	server->hook_dispatch_depth++;
	struct vidow_hook_event *q, *tmp;
	wl_list_for_each_safe(q, tmp, &server->hook_queue, link) {
		wl_list_remove(&q->link);
		server->hook_queue_len--;
		run_event(server, q->event, q->json);
		free(q->event);
		free(q->json);
		free(q);
	}
	server->hook_dispatch_depth--;
	hooks_sweep(server);
	/* 钩子里又发了事件（或者 Lua 里挂了新钩子）：下一轮继续，别让队列留着。 */
	if (!wl_list_empty(&server->hook_queue)) {
		hooks_arm_idle(server);
	}
}

void vidow_hooks_dispatch(struct vidow_server *server, const char *event,
		const char *json) {
	if (server == NULL || event == NULL || json == NULL) {
		return;
	}
	if (!vidow_hooks_wanted(server, event)) {
		return;
	}
	if (server->hook_queue_len >= VIDOW_HOOK_QUEUE_MAX) {
		wlr_log(WLR_ERROR, "钩子：待派发的事件积到 %d 条（%s 起不进队了）——"
			"钩子里干的活太重了", VIDOW_HOOK_QUEUE_MAX, event);
		return;
	}
	struct vidow_hook_event *q = calloc(1, sizeof(*q));
	if (q == NULL) {
		return;
	}
	q->event = strdup(event);
	q->json = strdup(json);
	if (q->event == NULL || q->json == NULL) {
		free(q->event);
		free(q->json);
		free(q);
		return;
	}
	wl_list_insert(server->hook_queue.prev, &q->link);
	server->hook_queue_len++;
	hooks_arm_idle(server);
}

/* --------------------------------------------------------------- 生命周期 */

bool vidow_hooks_init(struct vidow_server *server) {
	wl_list_init(&server->hooks);
	wl_list_init(&server->hook_queue);
	server->hook_count = 0;
	server->hook_queue_len = 0;
	server->hook_next_id = 1;
	server->hook_dispatch_depth = 0;
	server->hook_idle = NULL;
	return true;
}

void vidow_hooks_finish(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	vidow_hooks_clear(server);
	if (server->hook_idle != NULL) {
		wl_event_source_remove(server->hook_idle);
		server->hook_idle = NULL;
	}
	/* dead 的那些钩子：clear 之后没人再碰它们了 */
	hooks_sweep(server);
}

/* ---------------------------------------------------------------- IPC */

void vidow_hooks_json(struct jbuf *b, struct vidow_server *server) {
	jb_obj(b);
	jb_kv_int(b, "count", server != NULL ? server->hook_count : 0);
	jb_key(b, "hooks");
	jb_arr(b);
	if (server != NULL) {
		struct vidow_hook *h;
		wl_list_for_each(h, &server->hooks, link) {
			if (h->dead) {
				continue;
			}
			jb_obj(b);
			jb_kv_int(b, "id", h->id);
			jb_kv_str(b, "event", h->event);
			jb_kv_str(b, "kind", h->lua ? "lua" : "cmd");
			if (!h->lua) {
				jb_kv_str(b, "cmd", h->cmd);
			}
			jb_kv_uint(b, "fired", h->fired);
			if (h->fired > 0) {
				jb_kv_int(b, "last_ms", (long long)h->last_ms);
			}
			jb_obj_end(b);
		}
	}
	jb_arr_end(b);

	/* 顺带把事件名单也带上：{"cmd":"hooks"} 一次就把「谁在听什么」和「能听
	 * 哪些」都说清楚，脚本不用再发第二条。 */
	const char *const *names = NULL;
	size_t n = vidow_event_names(&names);
	jb_key(b, "event_names");
	jb_arr(b);
	for (size_t i = 0; i < n; i++) {
		jb_str(b, names[i]);
	}
	jb_arr_end(b);
	jb_obj_end(b);
}
