#include "env.h"
#include "ipc.h"
#include "server.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>
#include <wlr/xcursor.h>
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
/* Rendering density only: Xft.dpi = 96*S and cursor theme size = 24*S.
 * Geometry uses independent per-output affine maps from xwmscale.c. */
static double xwayland_scale_want(struct vidow_server *server) {
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
	return scale;
}

/* DPI policy is independent of the virtual X output geometry. */
void vidow_xwayland_scale_recompute(struct vidow_server *server) {
	server->xwayland_scale = xwayland_scale_want(server);
}

/* 我们自己设过哪些环境变量、设成了什么。用户自己显式设过的（比如会话里
 * 已经有 GDK_SCALE）不抢，不然会跟人的意图打架。 */
static struct {
	const char *name;
	char ours[32];
} managed_env[] = {
	/* GDK_SCALE 曾经在这里（=S）。现在**不再设** —— KDE 也不设：
	 * 见 vidow_xwayland_env 的注释。留着这一格是为了 XCURSOR_SIZE 的下标
	 * 不变（下面的宏按位置取），只是永远写空。 */
	{ "GDK_SCALE", "" },
	{ "QT_AUTO_SCREEN_SCALE_FACTOR", "" },
	{ "QT_ENABLE_HIGHDPI_SCALING", "" },
	{ "XCURSOR_SIZE", "" },
	{ "XCURSOR_THEME", "" },
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
	/* **不设 GDK_SCALE**（和 KDE 一致）。
	 *
	 * 缩放这件事只留**一个**来源：X 的 RESOURCE_MANAGER 里那份 Xft.dpi
	 * （= S × 96，见 x11_set_xft_dpi）。GTK / Chromium / Electron / Java 都读它。
	 *
	 * 以前这里写 GDK_SCALE=S 而把 Xft.dpi 钉死在 96，两个来源打架：GTK 优先
	 * 信 GDK_SCALE（界面按 S 倍画），但它拿到的 X 屏幕已经是**设备像素**
	 * （补丁 Xwayland 放大过），于是 CEF 系（Steam 的登录窗）算出来的窗口
	 * 尺寸和 vidow 除回去的对不上 —— Steam 主窗口干脆建不出来（实测
	 * xwayland_scale=2 时主窗口 map 0 次，=1 时 2 次）。
	 *
	 * KDE 的做法（plasma-workspace/startkde/plasma-setup-xwayland）：
	 *   xwaylandScale = kwinrc[Xwayland][Scale]
	 *   Xft.dpi = xwaylandScale * 96      ← 唯一来源
	 *   GDK_SCALE 不设、QT_SCALE_FACTOR 不设
	 * 照抄它。 */
	if (managed_env[0].ours[0] != '\0') {
		/* 老版本 vidow 设过 GDK_SCALE：把它撤掉，别留给下一个 X 客户端。 */
		const char *current = getenv("GDK_SCALE");
		if (current != NULL &&
				strcmp(current, managed_env[0].ours) == 0) {
			unsetenv("GDK_SCALE");
		}
		managed_env[0].ours[0] = '\0';
	}
	set_managed("QT_AUTO_SCREEN_SCALE_FACTOR", managed_env[1].ours,
		sizeof(managed_env[1].ours), "1");
	set_managed("QT_ENABLE_HIGHDPI_SCALING", managed_env[2].ours,
		sizeof(managed_env[2].ours), "1");
	set_managed("XCURSOR_SIZE", managed_env[3].ours, sizeof(managed_env[3].ours),
		int_str(value, sizeof(value), 24 * scale));
	/* 光标主题也要给 X11 客户端：X 那边自己按 XCURSOR_THEME 找 ~/.icons
	 * 下的目录，不说就用 Adwaita（用户报的「主题不见了，一直是默认」）。
	 * 值来自 main.c 起光标时读的那一份（环境变量优先、其次 gsettings），
	 * 这里只是把它转给 X。 */
	const char *theme = getenv("XCURSOR_THEME");
	if (theme != NULL && theme[0] != '\0') {
		set_managed("XCURSOR_THEME", managed_env[4].ours,
			sizeof(managed_env[4].ours), theme);
	}

	wlr_log(WLR_INFO, "xwayland: 不设 GDK_SCALE、Xft.dpi=%d（= %d × 96，唯一"
		"缩放来源；和 KDE 的 plasma-setup-xwayland 一致）、XCURSOR_SIZE=%d"
		"、XCURSOR_THEME=%s（不设 QT_SCALE_FACTOR —— Wayland 的 Qt 也认它）",
		scale * 96, scale, 24 * scale,
		theme != NULL && theme[0] != '\0' ? theme : "(未设)");
}

/* X 的 root window（写属性用）。 */
static xcb_window_t x11_root(xcb_connection_t *conn) {
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn));
	return it.data != NULL ? it.data->root : XCB_NONE;
}

/* 【已废弃】以前这里写 _XWAYLAND_GLOBAL_OUTPUT_SCALE（给打过 HiDPI 补丁的
 * Xwayland 的 xorg-xwayland-lily / xorg MR!733）。
 *
 * 那个属性会让 Xwayland 做两件事：X 屏幕 / RandR 乘 N，**并且在每个窗口表面上
 * 声明同样的 buffer scale**。第二件事是致命的 —— wlroots 在
 * types/wlr_compositor.c:210 校验 `buffer_width % scale == 0`，1x1 的辅助窗口
 * 必然不整除，于是 surface 被 WL_SURFACE_ERROR_INVALID_SIZE 直接杀掉：
 *   XWAYLAND: wl_surface#31: error 2: Buffer size (1x1) is not divisible by scale (2)
 * 米哈游启动器 / Steam 都建一批 1x1 DIALOG（托盘宿主），全部踩雷。
 *
 * 现在改由 src/xdgoutput.c 做同一件事：只对 Xwayland 客户端把 xdg_output 的
 * logical_size 报成统一虚拟尺寸。X 屏幕使用虚拟并集，但**不碰 buffer scale** —— 这正是
 * KWin（src/wayland/xdgoutput_v1.cpp:191）和 Hyprland
 * （src/protocols/XDGOutput.cpp:122）的做法。
 *
 * 这条属性也不写回去了：留着它会让上面那条校验重新生效。 */

/* RESOURCE_MANAGER 里的 Xft.dpi = **scale × 96**（和 KDE 一致）。
 *
 * 这是 X 客户端缩放**唯一**的来源：vidow 不设 GDK_SCALE、不设 QT_SCALE_FACTOR
 * （见 vidow_xwayland_env）。GDK_SCALE 和 Xft.dpi 同时存在时 GTK / Chromium
 * 会把两者**相乘**（DPR = GDK_SCALE × Xft.dpi/96），那才是重复缩放。
 *
 * 为什么必须是 ×S 而不是恒 96：补丁 Xwayland 把 X 屏幕放大成了设备像素，
 * X 客户端在设备像素坐标里画。只告诉它 DPI=96 的话，GTK/CEF 按 1 倍画界面、
 * 却拿到一个 2 倍大的屏幕 —— 算出来的窗口尺寸和 vidow 除回去的对不上，
 * Steam 的 x11 主窗口在 xwayland_scale=2 时建不出来（实测 map 0 次）。
 * 给 192 之后界面按 2 倍像素画，逻辑尺寸不变，两边对齐。
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
	wlr_log(WLR_INFO, "xwayland: Xft.dpi = %d（= scale × 96，X 客户端唯一的"
		"缩放来源；不设 GDK_SCALE，见 vidow_xwayland_env）", dpi);
}

/* Apply rendering density, never _XWAYLAND_GLOBAL_OUTPUT_SCALE. */
static void x11_apply_scale_props(struct vidow_server *server) {
	if (server->xwayland == NULL) {
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
	/* X-only xdg-output already supplies the final virtual rectangles. The
	 * global output-scale property would multiply them again and change
	 * surface buffer_scale (breaking 1x1 helper surfaces), so never set it. */
	x11_set_xft_dpi(conn, (int)(scale * 96));
}

/* Xwayland handles xdg-output asynchronously. Bound the root-size probe
 * to four attempts; a mismatch is diagnostic, never a DPI fallback. */
static const int probe_delays_ms[] = { 120, 250, 500, 1000 };

static int xwayland_probe_cb(void *data);

static void xwayland_probe_stop(struct vidow_server *server) {
	if (server->xwayland_probe != NULL) {
		wl_event_source_remove(server->xwayland_probe);
		server->xwayland_probe = NULL;
	}
}

static void xwayland_probe_schedule(struct vidow_server *server) {
	if (server->xwayland == NULL || server->xwayland_scale_probed ||
			server->xwayland_probe != NULL) {
		return;
	}
	if (server->xwayland_probe == NULL) {
		server->xwayland_probe = wl_event_loop_add_timer(
			wl_display_get_event_loop(server->display), xwayland_probe_cb,
			server);
		if (server->xwayland_probe == NULL) {
			return;
		}
	}
	server->xwayland_probe_tries = 1;
	wl_event_source_timer_update(server->xwayland_probe, probe_delays_ms[0]);
}

/* Same virtual union as X-only xdg-output, independent of Wayland scale. */
static void x11_want_screen_size(struct vidow_server *server, int *w, int *h) {
	struct wlr_box native = {0};
	vidow_xwm_native_union(server, &native);
	*w = native.width;
	*h = native.height;
}

/* X 屏幕现在多大（root window 的 geometry —— xcb_get_setup() 那份是连接建立时
 * 的快照，属性改了它不更新）。 */
static void x11_screen_size(xcb_connection_t *conn, int *w, int *h) {
	*w = 0;
	*h = 0;
	xcb_window_t root = x11_root(conn);
	if (root == XCB_NONE) {
		return;
	}
	xcb_get_geometry_reply_t *geo = xcb_get_geometry_reply(conn,
		xcb_get_geometry(conn, root), NULL);
	if (geo != NULL) {
		*w = (int)geo->width;
		*h = (int)geo->height;
		free(geo);
	}
}

static int xwayland_probe_cb(void *data) {
	struct vidow_server *server = data;
	xcb_connection_t *conn = server->xwayland != NULL ?
		wlr_xwayland_get_xwm_connection(server->xwayland) : NULL;
	if (conn == NULL) {
		xwayland_probe_stop(server);
		return 0;
	}

	int xw = 0, xh = 0;
	x11_screen_size(conn, &xw, &xh);
	int want_w = 0, want_h = 0;
	x11_want_screen_size(server, &want_w, &want_h);
	bool fit = want_w > 0 && want_h > 0 && xw == want_w && xh == want_h;
	if (!fit && server->xwayland_probe_tries <
			(int)(sizeof(probe_delays_ms) / sizeof(probe_delays_ms[0]))) {
		int delay = probe_delays_ms[server->xwayland_probe_tries++];
		wl_event_source_timer_update(server->xwayland_probe, delay);
		return 0;
	}

	xwayland_probe_stop(server);
	server->xwayland_scale_probed = want_w > 0 && want_h > 0;
	server->xwayland_scale_ok = fit;
	wlr_log(fit ? WLR_INFO : WLR_ERROR,
		"xwayland: X root %dx%d, virtual output union %dx%d: %s; DPI %.0f unchanged",
		xw, xh, want_w, want_h, fit ? "matched" : "mismatch",
		96.0 * server->xwayland_scale);

	return 0;
}

void vidow_xwayland_scale_apply(struct vidow_server *server) {
	double before = server->xwayland_scale;
	vidow_xwayland_scale_recompute(server);
	if (server->xwayland == NULL) {
		return;
	}
	if (fabs(before - server->xwayland_scale) >= 1e-3) {
		wlr_log(WLR_INFO,
			"xwayland: 全局缩放 %.2f -> %.2f（仅渲染 DPI，几何按逐屏仿射）",
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
	/* Layout/mode changes affect virtual geometry even if DPI stays unchanged. */
	server->xwayland_scale_probed = false;
	xwayland_probe_schedule(server);
}

/* X 那边真的活了：这时候才能写 root window 的属性。 */
static void xwayland_ready(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server = wl_container_of(listener, server, xwayland_ready);

	/* 把 seat 再交给 X 服务一次。Xwayland 退出时 wlroots 的 xwm_destroy 会
	 * wlr_xwayland_set_seat(xwayland, NULL)（顺带把 seat 上那份 X 来的选区撤
	 * 掉），而懒启动模式下重启用的是同一个 wlr_xwayland 对象 —— 只在建的时候
	 * 设过一次的话，重启后 xwayland->seat 一直是 NULL：xwm 拒掉所有选区事件
	 * （日志刷 "not handling selection events: no seat assigned to xwayland"），
	 * Wayland 复制的东西 X 侧永远拿不到（README 252）。
	 * wlr_xwayland_set_seat 会先摘旧监听，重入安全。 */
	wlr_xwayland_set_seat(server->xwayland, server->seat);

	/* X clients without their own cursor inherit the root cursor. Seed it on
	 * every Xwayland start, otherwise they inherit X's default cross. The image
	 * buffer is borrowed from cursor_mgr; Xwayland retains it as needed. */
	/* Scale 1 is wrong on a HiDPI output: Xwayland hands this buffer to X
	 * clients as their inherited root cursor, and a 1x 24px image inside a
	 * 2x desktop is drawn small but its hotspot/clear area still covers a
	 * 2x cursor box, so the pointer comes out clipped (reported on the 2x
	 * output while the 1x one looked fine). Use the largest output scale,
	 * which is what the cursor manager preloads for. */
	double cursor_scale = 1.0;
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		double sc = o->wlr_output->scale;
		if (sc > cursor_scale) {
			cursor_scale = sc;
		}
	}
	struct wlr_xcursor *cursor = wlr_xcursor_manager_get_xcursor(
		server->cursor_mgr, "left_ptr", (float)cursor_scale);
	if (cursor != NULL && cursor->image_count > 0) {
		struct wlr_xcursor_image *image = cursor->images[0];
		struct wlr_buffer *buffer = wlr_xcursor_image_get_buffer(image);
		if (buffer != NULL) {
			wlr_xwayland_set_cursor(server->xwayland, buffer,
				image->hotspot_x, image->hotspot_y);
		}
	}

	/* New Xwayland instance: recheck its root against the virtual union. */
	server->xwayland_scale_ok = true;
	server->xwayland_scale_probed = false;
	server->xwayland_probe_tries = 0;

	/* 事件：X 服务真的起来了（懒启动，第一个 X 客户端连上来才有这一下）。
	 * 配置里 exec_once 起的 X11 程序要等它的话，听这个。 */
	const struct vidow_event_field fields[] = {
		VIDOW_EV_STR("display", server->xwayland != NULL ?
			server->xwayland->display_name : NULL),
		VIDOW_EV_NUM("scale", (long long)(server->xwayland_scale + 0.5)),
		VIDOW_EV_END,
	};
	vidow_ipc_emit_simple(server, "xwayland-ready", fields);

	/* **告诉 xdg-output 哪个 wl_client 是 Xwayland**：它会把这个客户端拿到的
	 * logical_size / position 换成统一虚拟矩形（见 src/xdgoutput.c）。这一步替代了以前写
	 * _XWAYLAND_GLOBAL_OUTPUT_SCALE 的做法 —— 那个属性会连带把 X surface 的
	 * buffer scale 也设成 S，1x1 的辅助窗口于是被 wlroots 拒掉
	 * （"Buffer size (1x1) is not divisible by scale (2)"）。
	 *
	 * 放在 x11_apply_scale_props 之前：Xwayland 一起来就可能已经查过
	 * xdg_output 了，先认身份、再补属性。 */
	vidow_xdg_output_set_xwayland_client(
		server->xwayland != NULL && server->xwayland->server != NULL ?
		server->xwayland->server->client : NULL);

	/* 再把渲染密度告诉 X（Xft.dpi = S × 96，几何另走逐屏映射）。
	 * 必须赶在第一个客户端连上之前 —— 客户端一进来就会按当时的 DPI 算布局。 */
	x11_apply_scale_props(server);
	xwayland_probe_schedule(server);

	/* 诊断用：X root 与统一虚拟输出并集是否一致。
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
		/* 期望值同样用逐屏累加（见 x11_want_screen_size）—— 混合 DPI 下
		 * 「布局逻辑 × 全局缩放」是错的，这条日志就是排查的第一现场，
		 * 数字必须是真的。 */
		int want_w = 0, want_h = 0;
		x11_want_screen_size(server, &want_w, &want_h);
		wlr_log(WLR_INFO,
			"xwayland: X root snapshot %ux%u; expected virtual union %dx%d; "
			"Wayland layout %dx%d unchanged, DPI %.0f",
			xw, xh, want_w, want_h, layout.width, layout.height,
			96.0 * server->xwayland_scale);

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
	xwayland_probe_stop(server);
	/* X 服务要拆了：剪贴板桥手上那条连接马上就不算数了（wlroots 紧接着
	 * xwm_destroy → xcb_disconnect），先摘掉，别留野指针。 */
	vidow_clipboard_x11_detach(server);
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
	wlr_log(WLR_INFO,
		"x11-life new xid=0x%x xs=%p surface=%p surface_id=%u",
		xsurface->window_id, xsurface, xsurface->surface,
		xsurface->surface_id);
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
		xwayland_probe_stop(server);
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

	/* 关于 Xwayland 的 ready 管道（wlroots 的 server->pipe_source）：Xwayland
	 * 写完 ready 字节之后这个管道**一直是可读的**，wlroots 挂在主循环上的那个源
	 * 于是每轮都醒 —— 一个常驻自唤醒。这里没能接管：libwayland 不导出 event
	 * source 的底层 fd，而 ready 事件也**不能**提前自己发（wlroots 的
	 * handle_ready 要做 init / 建 XWM 连接）。其余自唤醒 fd 都已经走 fdwatch 了。 */

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
	/* DISPLAY 变了就得再推一轮：systemd 拉起来的 X11 程序也要连得上 vidow 的 X */
	vidow_env_export_later(server);

	vidow_xwayland_scale_recompute(server);
	vidow_xwayland_env(server);

	wlr_log(WLR_INFO, "xwayland: DISPLAY=%s，懒启动，X 全局缩放 %.2f",
		server->xwayland->display_name, server->xwayland_scale);
	return true;
}
