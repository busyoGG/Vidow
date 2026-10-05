#include "server.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>
#include <wlr/xwayland/shell.h>
#include <xcb/xcb.h>

/* X11 只有一套坐标、一套缩放：所有输出共用。取「最密那块屏的缩放」向上取整，
 * 因为 GTK 的 GDK_SCALE 只能整数。取最大值的道理：窗口按 S 倍像素画出来，落到
 * 缩放 s 的输出上就是 s/S 倍 —— 只有 s/S <= 1（也就是 S >= s）时才是缩小，
 * 缩小不丢清晰度；S < s 就是放大，放大必糊。
 *
 * 为什么这样能同时满足「一样大」和「一样清晰」：X11 窗口在布局里占 L/S 个逻辑
 * 单位（L 是客户端画的像素数），真正的物理尺寸 = (L/S) * s / ppi —— 而 Wayland
 * 应用同样占 L/S 个逻辑单位。两块屏的 scale ∝ ppi 时，物理尺寸就一样；清晰度
 * 上最密那块屏是 1:1，其余是缩小。xwayland_scale=off 则反过来：X 按 1:1 画
 * （不认 GDK_SCALE 的老程序会和 Wayland 应用一样大），但 HiDPI 屏上是放大糊的。 */
void vidow_xwayland_scale_recompute(struct vidow_server *server) {
	double scale = 1.0;
	struct vidow_scale_spec spec;

	bool parsed = vidow_scale_parse(server->config.xwayland_scale, &spec);
	if (parsed && spec.kind == VIDOW_SCALE_FIXED) {
		scale = spec.value;
	} else if (parsed && spec.kind == VIDOW_SCALE_OFF) {
		/* off = 不当缩放桥：X 一律按 1:1 画（老程序保持和 Wayland 一样大，
		 * 代价是 HiDPI 屏上被放大、糊）。 */
		scale = 1.0;
	} else {
		struct vidow_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			double s = output->wlr_output->scale;
			if (s > scale) {
				scale = s;
			}
		}
	}

	scale = ceil(scale);
	if (scale < 1.0) {
		scale = 1.0;
	}
	if (scale > 8.0) {
		scale = 8.0;
	}
	server->xwayland_scale = scale;
}

/* 我们自己设过哪些环境变量、设成了什么。用户自己显式设过的（比如会话里
 * 已经有 GDK_SCALE）不抢，不然会跟人的意图打架。 */
static struct {
	const char *name;
	char ours[32];
} managed_env[] = {
	{ "GDK_SCALE", "" },
	{ "QT_SCALE_FACTOR", "" },
	{ "QT_AUTO_SCREEN_SCALE_FACTOR", "" },
	{ "QT_ENABLE_HIGHDPI_SCALING", "" },
	{ "XCURSOR_SIZE", "" },
};

/* 只接管「没人设的」和「我们上次设的」：用户自己显式设过的值不抢。 */
static void set_managed(const char *name, char *slot, size_t slot_len,
		const char *value) {
	const char *current = getenv(name);
	bool ours = slot[0] != '\0' && current != NULL && strcmp(current, slot) == 0;
	if (current != NULL && strcmp(current, value) != 0 && !ours) {
		wlr_log(WLR_DEBUG, "xwayland: %s=%s 不是我们设的，不动它", name,
			current);
		return;
	}
	setenv(name, value, 1);
	snprintf(slot, slot_len, "%s", value);
}

static const char *int_str(char *buf, size_t len, int value) {
	snprintf(buf, len, "%d", value);
	return buf;
}

void vidow_xwayland_env(struct vidow_server *server) {
	if (!server->config.xwayland_env) {
		return;
	}

	int scale = (int)lround(server->xwayland_scale);
	if (scale < 1) {
		scale = 1;
	}
	char value[32];
	set_managed("GDK_SCALE", managed_env[0].ours, sizeof(managed_env[0].ours),
		int_str(value, sizeof(value), scale));
	/* Qt 会另外从 Xft.dpi 猜一个缩放，和我们设的叠起来就是 4 倍，关掉。 */
	set_managed("QT_SCALE_FACTOR", managed_env[1].ours,
		sizeof(managed_env[1].ours), int_str(value, sizeof(value), scale));
	set_managed("QT_AUTO_SCREEN_SCALE_FACTOR", managed_env[2].ours,
		sizeof(managed_env[2].ours), "0");
	set_managed("QT_ENABLE_HIGHDPI_SCALING", managed_env[3].ours,
		sizeof(managed_env[3].ours), "1");
	set_managed("XCURSOR_SIZE", managed_env[4].ours, sizeof(managed_env[4].ours),
		int_str(value, sizeof(value), 24 * scale));

	wlr_log(WLR_INFO, "xwayland: GDK_SCALE=QT_SCALE_FACTOR=%d（X 客户端按这个画）",
		scale);
}

/* X 的 root window（写属性用）。 */
static xcb_window_t x11_root(xcb_connection_t *conn) {
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
	return it.data != NULL ? it.data->root : XCB_NONE;
}

/* 告诉打过 HiDPI 补丁的 Xwayland（xorg-xwayland-lily / xorg 的 MR!733）「X 这边
 * 全局缩放是 N」。
 *
 * 它收到之后做两件事：X 屏幕和 RandR 模式都乘 N（于是 X 客户端看到的是**设备
 * 像素**的屏幕 —— 正是 vidow 内部「X 像素 = 逻辑 × xwayland_scale」那个假设），
 * 并且在每个窗口表面上声明同样的 buffer scale（vidow 那边按同一个倍数缩小，
 * 两边正好抵消，窗口尺寸不变）。
 *
 * 不写的后果（实测）：X 屏幕只有**逻辑**尺寸，GTK 那类按屏幕大小算布局的客户端
 * 会把整屏当成一半大小 —— 最大化的窗口只占 1/4 面积，微信登录窗那种固定尺寸的
 * 窗口也会和面板对不上。没打补丁的 Xwayland 里这个原子不存在，那就什么都不写
 * （它本来就会按逻辑尺寸报屏幕，vidow 的换算照旧自洽）。 */
static void x11_set_global_scale(struct vidow_server *server,
		xcb_connection_t *conn, int scale) {
	xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 1,
		sizeof("_XWAYLAND_GLOBAL_OUTPUT_SCALE") - 1,
		"_XWAYLAND_GLOBAL_OUTPUT_SCALE");
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
	if (reply == NULL || reply->atom == XCB_ATOM_NONE) {
		free(reply);
		wlr_log(WLR_INFO, "xwayland: X 这边没有 _XWAYLAND_GLOBAL_OUTPUT_SCALE"
			"（没打 HiDPI 补丁的 Xwayland）→ X 屏幕按逻辑尺寸报，vidow 照旧除");
		return;
	}
	xcb_atom_t atom = reply->atom;
	free(reply);

	uint32_t value = (uint32_t)(scale > 0 ? scale : 1);
	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, x11_root(conn), atom,
		XCB_ATOM_CARDINAL, 32, 1, &value);
	xcb_flush(conn);
	wlr_log(WLR_INFO, "xwayland: _XWAYLAND_GLOBAL_OUTPUT_SCALE = %u"
		"（X 屏幕和 RandR 模式按它放大成设备像素）", value);
	(void)server;
}

/* RESOURCE_MANAGER 里的 Xft.dpi = 96 × 缩放。
 *
 * GDK_SCALE / QT_SCALE_FACTOR 只覆盖 GTK / Qt；Chromium 系（微信的登录窗就是
 * CEF）、Electron、Java、Wine 这些只认 Xft.dpi。不写它，它们按 1 倍画，而 vidow
 * 按 xwayland_scale 除 —— 窗口就只剩该有的 1/4（「只有面板的 1/4 大」）。
 *
 * 写法上要保住别人的资源项：把现有的 RESOURCE_MANAGER 读出来，删掉旧的 Xft.dpi
 * 行再补一行新的（就是 xrdb -merge 干的事，只不过不依赖 xrdb 在不在）。 */
static void x11_set_xft_dpi(xcb_connection_t *conn, int dpi) {
	xcb_window_t root = x11_root(conn);
	if (root == XCB_NONE) {
		return;
	}
	xcb_intern_atom_cookie_t cookie = xcb_intern_atom(conn, 0,
		sizeof("RESOURCE_MANAGER") - 1, "RESOURCE_MANAGER");
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(conn, cookie, NULL);
	if (reply == NULL) {
		return;
	}
	xcb_atom_t atom = reply->atom;
	free(reply);

	char *old = NULL;
	size_t old_len = 0;
	xcb_get_property_cookie_t get = xcb_get_property(conn, 0, root, atom,
		XCB_ATOM_STRING, 0, 1 << 20);
	xcb_get_property_reply_t *got = xcb_get_property_reply(conn, get, NULL);
	if (got != NULL) {
		if (got->type == XCB_ATOM_STRING && got->format == 8 &&
				got->value_len > 0) {
			old = xcb_get_property_value(got);
			old_len = xcb_get_property_value_length(got);
		}
	}

	char buf[4096];
	size_t n = 0;
	n += (size_t)snprintf(buf + n, sizeof(buf) - n, "Xft.dpi:\t%d\n", dpi);
	/* 抄旧的内容，跳过原有的 Xft.dpi 行。 */
	for (size_t i = 0; old != NULL && i < old_len && n + 1 < sizeof(buf); ) {
		size_t line_end = i;
		while (line_end < old_len && old[line_end] != '\n') {
			line_end++;
		}
		size_t line_len = line_end - i;
		if (!(line_len >= 8 && strncmp(old + i, "Xft.dpi:", 8) == 0)) {
			size_t copy = line_len < sizeof(buf) - n - 2 ? line_len :
				sizeof(buf) - n - 2;
			memcpy(buf + n, old + i, copy);
			n += copy;
			if (line_end < old_len) {
				buf[n++] = '\n';
			}
		}
		i = line_end < old_len ? line_end + 1 : old_len;
	}
	buf[n] = '\0';

	xcb_change_property(conn, XCB_PROP_MODE_REPLACE, root, atom,
		XCB_ATOM_STRING, 8, (uint32_t)n, buf);
	xcb_flush(conn);
	if (got != NULL) {
		free(got);
	}
	wlr_log(WLR_INFO, "xwayland: Xft.dpi = %d（Chromium/CEF、Java、Wine 靠它）",
		dpi);
}

/* 把「X 这边的缩放」告诉 X 服务：属性 + Xft.dpi。缩放变了要重来一遍。 */
static void x11_apply_scale_props(struct vidow_server *server) {
	if (server->xwayland == NULL || !server->config.xwayland_env) {
		return;
	}
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
	if (conn == NULL) {
		return; /* X 还没起来（懒启动），ready 的时候再来 */
	}
	int scale = (int)lround(server->xwayland_scale);
	if (scale < 1) {
		scale = 1;
	}
	x11_set_global_scale(server, conn, scale);
	x11_set_xft_dpi(conn, 96 * scale);
}

void vidow_xwayland_scale_apply(struct vidow_server *server) {
	double before = server->xwayland_scale;
	vidow_xwayland_scale_recompute(server);
	if (server->xwayland == NULL) {
		return;
	}
	if (fabs(before - server->xwayland_scale) < 1e-3) {
		return;
	}

	wlr_log(WLR_INFO, "xwayland: 全局缩放 %.2f -> %.2f（逻辑尺寸要除以它才是 X 像素）",
		before, server->xwayland_scale);

	vidow_xwayland_env(server);
	/* X 那边也要知道：屏幕尺寸、RandR 模式、Xft.dpi 都跟着变。 */
	x11_apply_scale_props(server);

	/* 已经存在的 X11 窗口要按新缩放重新声明一遍缓冲区比例。 */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->kind == VIDOW_VIEW_X11) {
			vidow_view_x11_scale(view);
		}
	}
}

/* X 那边真的活了：这时候才能写 root window 的属性。 */
static void xwayland_ready(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server = wl_container_of(listener, server, xwayland_ready);

	/* 先把 X 那边的缩放告诉它（_XWAYLAND_GLOBAL_OUTPUT_SCALE + Xft.dpi）：
	 * 打过补丁的 Xwayland 收到之后会立刻把 X 屏幕和 RandR 模式改成设备像素。
	 * 必须赶在第一个客户端连上之前 —— 客户端一进来就会按当时的屏幕尺寸算布局。 */
	x11_apply_scale_props(server);

	/* 诊断用：X 看到的屏幕多大，和我们以为的（逻辑 × 缩放）对不对得上。
	 * 这条日志是排查「X 窗口位置偏移 / 菜单跑到别的屏上」的第一现场。
	 * 注意不能用 xcb_get_setup() 里的屏幕尺寸 —— 那是连接建立时的那一份，属性
	 * 改了它也不会更新；root window 的 geometry 才是「现在」的。 */
	xcb_connection_t *conn = wlr_xwayland_get_xwm_connection(server->xwayland);
	if (conn != NULL) {
		xcb_window_t root = x11_root(conn);
		xcb_get_geometry_reply_t *geo = root != XCB_NONE ?
			xcb_get_geometry_reply(conn, xcb_get_geometry(conn, root), NULL) :
			NULL;
		unsigned xw = geo != NULL ? (unsigned)geo->width : 0;
		unsigned xh = geo != NULL ? (unsigned)geo->height : 0;
		free(geo);
		struct wlr_box layout = {0};
		wlr_output_layout_get_box(server->output_layout, NULL, &layout);
		int want_w = (int)lround(layout.width * server->xwayland_scale);
		int want_h = (int)lround(layout.height * server->xwayland_scale);
		wlr_log(WLR_INFO,
			"xwayland: X 屏幕 %ux%u（布局 %dx%d 逻辑 ×%.2f = %dx%d 设备像素）%s",
			xw, xh, layout.width, layout.height, server->xwayland_scale,
			want_w, want_h,
			(layout.width > 0 && xw == (unsigned)want_w &&
				xh == (unsigned)want_h) ? "" :
			" —— 对不上：X 那边没按设备像素报屏幕（没打 HiDPI 补丁的 Xwayland "
			"就是这样），按屏幕尺寸算布局的客户端会把整屏当成一半大小");
		if (layout.width < 1 || layout.height < 1) {
			/* X 屏幕的大小 = 输出的并集：一块屏都没有时它就是 0x0，X11 客户端
			 * 会看不见、点不动（用户看到的就是「XWayland 点不动」，见
			 * README「X 屏幕 0x0」那条）。这里必须明说，不然人只会去查
			 * 输入，查半天是会话里一块屏都没有。 */
			wlr_log(WLR_ERROR,
				"xwayland: 现在一块屏都没有（布局 0x0）→ X 屏幕 0x0，X11 客户端"
				"看不见也点不动。先给 vidow 一块屏：嵌套跑的话别把它的窗口关了；"
				"真机上就是显示器全拔了。");
		}
	}

	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box box = {0};
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
			&box);
		wlr_log(WLR_INFO, "xwayland:   输出 %s 逻辑 %dx%d@%d,%d ×%.2f",
			output->wlr_output->name, box.width, box.height, box.x, box.y,
			output->wlr_output->scale);
	}

	/* X 的剪贴板桥要等 xwm 的连接可用才能接（见 clipboard.c）。 */
	vidow_clipboard_x11_ready(server);
}

static void xwayland_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server = wl_container_of(listener, server, xwayland_destroy);
	wl_list_remove(&server->xwayland_ready.link);
	wl_list_remove(&server->xwayland_destroy.link);
	wl_list_remove(&server->new_xwayland_surface.link);
	server->xwayland = NULL;
}

static void handle_new_xwayland_surface(struct wl_listener *listener,
		void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_xwayland_surface);
	struct wlr_xwayland_surface *xsurface = data;

	/* xwm 自己那条连接上的窗口不是客户端窗口：我们剪贴板桥的监听窗口，还有
	 * xwm 的 selection_window 之类的内部窗口（都建在同一条连接上）。它们永远
	 * 不映射，但 xwm 照样会给它们建 surface —— 收进视图模型就是一个 1x1 的
	 * 幽灵窗口，白占一格布局（识别见 clipboard.c 的 vidow_clipboard_owns_xwindow）。 */
	if (vidow_clipboard_owns_xwindow(server, xsurface->window_id)) {
		return;
	}
	vidow_view_new_x11(server, xsurface);
}

/* 只让 Xwayland 自己拿到 xwayland_shell_v1：否则任何 Wayland 客户端都能伪造
 * 「我是一个 X11 窗口」，去蹭只有 X 窗口才有的东西。 */
static bool global_filter(const struct wl_client *client,
		const struct wl_global *global, void *data) {
	struct vidow_server *server = data;
	if (server->xwayland == NULL || server->xwayland->shell_v1 == NULL ||
			global != server->xwayland->shell_v1->global) {
		return true;
	}
	struct wlr_xwayland_server *xserver = server->xwayland->server;
	return xserver != NULL && client == xserver->client;
}

bool vidow_xwayland_init(struct vidow_server *server) {
	return vidow_xwayland_apply_config(server);
}

/* 按配置把 X 服务建起来或拆掉。
 *
 * 配置是 Lua，跑得比这里晚（要等输出、seat 都在），所以启动时先按默认值建起来
 * —— 配置里的子进程得能拿到 DISPLAY —— 等配置落地后由 vidow_config_sync()
 * 再调一次这里对账：配置说关就拆（X 客户端会被断开，和启动时就关掉等效），
 * 之后又把开关打开（或者删掉那行再 SIGHUP）就重新建起来。 */
bool vidow_xwayland_apply_config(struct vidow_server *server) {
	if (!server->config.xwayland) {
		if (server->xwayland == NULL) {
			return true;
		}
		wlr_log(WLR_INFO, "xwayland: 配置里关了，拆掉 X 服务");
		/* destroy 的信号处理里会把监听摘掉、把 server->xwayland 置空；
		 * global_filter 那时就成了直通车，不用再动。 */
		wlr_xwayland_destroy(server->xwayland);
		unsetenv("DISPLAY");
		return true;
	}
	if (server->xwayland != NULL) {
		return true;
	}
	if (server->compositor == NULL) {
		wlr_log(WLR_ERROR, "xwayland: 没有 wlr_compositor，起不了");
		return false;
	}

	/* lazy = true：真有客户端连 X 的时候才 fork Xwayland，没人用就不花这份钱。 */
	server->xwayland = wlr_xwayland_create(server->display,
		server->compositor, true);
	if (server->xwayland == NULL) {
		wlr_log(WLR_ERROR, "xwayland: wlr_xwayland_create 失败");
		return false;
	}

	wlr_xwayland_set_seat(server->xwayland, server->seat);

	server->xwayland_ready.notify = xwayland_ready;
	wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);
	server->xwayland_destroy.notify = xwayland_destroy;
	wl_signal_add(&server->xwayland->events.destroy, &server->xwayland_destroy);
	server->new_xwayland_surface.notify = handle_new_xwayland_surface;
	wl_signal_add(&server->xwayland->events.new_surface,
		&server->new_xwayland_surface);
	wl_display_set_global_filter(server->display, global_filter, server);

	/* 配置里启动的客户端要能连上 X：lazy 模式下 socket 现在就有了。 */
	setenv("DISPLAY", server->xwayland->display_name, 1);

	vidow_xwayland_scale_recompute(server);
	vidow_xwayland_env(server);

	wlr_log(WLR_INFO, "xwayland: DISPLAY=%s，懒启动，X 全局缩放 %.2f",
		server->xwayland->display_name, server->xwayland_scale);
	return true;
}
