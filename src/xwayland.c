#include "env.h"
#include "ipc.h"
#include "server.h"
#include "x11surface.h"

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
	if (server->xwayland_server == NULL) {
		return;
	}
	xcb_connection_t *conn = vidow_x11_connection();
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

	/* **XSETTINGS 也要跟着写。**
	 *
	 * Xft.dpi 只够 Chromium/CEF（它直接读 X 资源）和字体密度；GTK3 的
	 * X11 后端用 XSETTINGS 的 Gdk/WindowScalingFactor 决定窗口倍率，没有
	 * 属主就当 1 倍 —— 而 vidow 把 X 缓冲除 2，内容只剩一半。详见
	 * xwm_x11.c 的 xsettings 段。 */
	vidow_x11_xsettings_update(server->xwm, scale);
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
	if (server->xwayland_server == NULL || server->xwayland_scale_probed ||
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
	xcb_connection_t *conn = server->xwayland_server != NULL ?
		vidow_x11_connection() : NULL;
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
	if (server->xwayland_server == NULL) {
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
/* 把光标的像素读出来，交给 XWM 设到 root 上。
 *
 * 为什么不用 wlr_xwayland_set_cursor()：那个函数会把 buffer 转给 wlroots 内部
 * 的 xwm。我们的 xwm 是自己的，光标也得自己设 —— 做法照 Hyprland 的
 * CXWM::setCursor()（XWM.cpp:1389），像素来源则是 wlr_xcursor 的 buffer。
 *
 * cursor 会交一张 ARGB8888 的图，stride 是每行字节数；X 那边要的正是这个布局
 * （32 位 depth + alpha_mask 0xff000000），所以直接抄过去就行。 */
static void xwayland_set_root_cursor(struct vidow_server *server,
		struct wlr_xcursor_image *image) {
	if (server->xwm == NULL || image == NULL) {
		return;
	}
	struct wlr_buffer *buffer = wlr_xcursor_image_get_buffer(image);
	if (buffer == NULL) {
		return;
	}
	void *data = NULL;
	uint32_t format = 0;
	size_t stride = 0;
	if (!wlr_buffer_begin_data_ptr_access(buffer,
			WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
		return;
	}
	vidow_x11_set_cursor(data, (uint32_t)stride, image->width,
		image->height, image->hotspot_x, image->hotspot_y);
	wlr_buffer_end_data_ptr_access(buffer);
}

/* Xwayland 进程 fork 出来之后、它连 display 之前。
 *
 * **bind 的白名单要在这里就给好。** Xwayland 一起来第一件事就是连 display
 * 并 bind xwayland_shell_v1，而 wlroots 的检查（反汇编：
 * `cmp client, shell->client` / 不等就报 Permission denied）在 shell->client
 * 为 NULL 时**不放行任何人** —— 连 Xwayland 自己也不行。等 ready 事件再
 * set_client 就太晚了：那时 bind 已经失败、Xwayland 已经退出。
 *
 * start 在 fork 成功之后立刻发，这时 server->client 已经分配好了。 */
static void xwayland_start(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_server *server = wl_container_of(listener, server, xwayland_start);
	if (server->xwayland_server == NULL) {
		return;
	}
	struct wl_client *client = server->xwayland_server->client;
	wlr_log(WLR_INFO, "xwayland: start（pid %d，client %p，shell %p）",
		(int)server->xwayland_server->pid, (void *)client,
		(void *)server->xwayland_shell);
	if (server->xwayland_shell != NULL && client != NULL) {
		wlr_xwayland_shell_v1_set_client(server->xwayland_shell, client);
	}
}

static void xwayland_ready(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, xwayland_ready);
	struct wlr_xwayland_server_ready_event *ev = data;

	/* **必须在这里把 Xwayland 的 client 交给 shell。**
	 *
	 * wlroots 内部对 xwayland_shell_v1 的 bind 做了白名单检查（反汇编确认：
	 * `cmp client, shell->client` / 不等就报 "Permission denied to bind to
	 * xwayland_shell_v1"）。shell 刚 create 出来时那个字段是 NULL，谁都不
	 * 放行 —— 包括 Xwayland 自己。
	 *
	 * 踩过的弯路：自己写 wl_display_set_global_filter 去比
	 * `client == xserver->client`，永远不成立（filter 跑在 ready 之前，
	 * 那时 xserver->client 还没赋值），于是 Xwayland 被拒在门外：
	 *     XWAYLAND: error 3: Permission denied to bind to xwayland_shell_v1
	 *     (EE) could not connect to wayland server
	 *     Xwayland startup failed, not setting up xwm
	 * 整个 X 服务起不来，/tmp/.X11-unix 空着，DISPLAY=:0 连不上。
	 *
	 * 正确做法就是 wlroots 给的这个入口。ready 是**唯一**能拿到
	 * xserver->client 的时刻（start 事件在 fork 之后、连 display 之前，
	 * 那时 client 还没分配）。 */
	if (server->xwayland_shell != NULL && ev->server != NULL &&
			ev->server->client != NULL) {
		wlr_xwayland_shell_v1_set_client(server->xwayland_shell,
			ev->server->client);
	}


	/* **这里是整个 X11 架构的分界线。**
	 *
	 * wlroots 把这个 wm_fd 交出来，等的就是有人在自己的事件循环里接住它 ——
	 * 这就是 Hyprland 的 CXWM 构造函数干的事（XWM.cpp:1009：
	 * xcb_connect_to_fd(m_server->m_xwmFDs[0])）。以前我们把它留给了
	 * wlr_xwayland 内部的 wlr_xwm，所以窗口管理的一切都不归我们管。
	 *
	 * 现在我们自己接：xcb_connect_to_fd(ev->wm_fd) → 抢
	 * SubstructureRedirect → 装进 wl_event_loop。从这一步起，窗口的
	 * create / map / configure / 属性 / 销毁全走 src/xwm_x11.c。 */
	xcb_connection_t *xwm_conn = xcb_connect_to_fd(ev->wm_fd, NULL);
	if (xcb_connection_has_error(xwm_conn)) {
		wlr_log(WLR_ERROR, "xwayland: xcb_connect_to_fd 失败，X 窗口管不了");
		xcb_disconnect(xwm_conn);
		return;
	}
	server->xwm = vidow_x11_server_create(server, xwm_conn);
	if (server->xwm == NULL) {
		wlr_log(WLR_ERROR, "xwayland: 自己的 XWM 起不来");
		return;
	}

	/* 以前这里要 wlr_xwayland_set_seat()：它只把 seat 交给 wlroots 内部的
	 * xwm，用来判选区归属。现在 xwm 是我们自己的，选区事件走
	 * x11->event_hook（见 clipboard.c），不需要这一步。 */

	/* **根光标设成「透明 1x1」，不再设主题的 left_ptr。**
	 *
	 * 这条以前是 Hyprland 的做法：给「自己不设光标的 X 客户端」一个像样
	 * 的默认光标，不然它们继承 X 自带的十字。但那个做法有个更糟的副作用
	 * —— **全屏游戏藏不住系统光标**。
	 *
	 * 游戏藏光标的典型做法是 SetCursor(NULL)（Wine 侧就是把窗口的 cursor
	 * 设成 None，自己在画面里画光标）。窗口 cursor 是 None 时 X 按继承链
	 * 往上找，最后落到 root window 的 cursor 上；root 上摆着一支可见的
	 * left_ptr，于是 Xwayland 报给合成器的不是「不画」，而是「这支主题
	 * 箭头」，vidow 老实画上去 —— 用户看到的就是「游戏里顶着一支普通白色
	 * 箭头」。
	 *
	 * 现场对得上（2026-11-01）：游戏里报上来的 cursor 图是 **64x64、热点
	 * 13,4**，和 Bibata-Modern-Ice 的 left_ptr 64 那张完全一致（热点 13,4
	 * 是那张图自己的热点），而 64 正是本函数按 2 倍屏预加载的尺寸。把 root
	 * 换成透明之后，同一条继承链报上来的立刻变成 NULL（vidow 日志
	 * 「set_cursor surface=(nil)」），游戏里那支箭头就没了。
	 *
	 * rootless 下 root window 一个像素都不显示，这里唯一的作用就是「没设
	 * 光标的窗口继承到什么」：继承到透明 = 「没设 = 不画」。游戏要显示什么
	 * 光标就自己 set_cursor，那条路照客户端给的图画（vidow_cursor_set_
	 * client_surface）。真遇到从不设光标的 X11 程序，它本来也是没有光标，
	 * 不会比继承一支不属于它的箭头更糟。
	 *
	 * 逃生口：VIDOW_XROOT_CURSOR=theme 退回老行为（root 上摆主题 left_ptr），
	 * 万一某个 X11 程序非要靠继承才有光标，改这一个环境变量就行。 */
	const char *xroot_cursor = getenv("VIDOW_XROOT_CURSOR");
	if (xroot_cursor != NULL && strcmp(xroot_cursor, "theme") == 0) {
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
			xwayland_set_root_cursor(server, cursor->images[0]);
		}
	} else {
		static const unsigned char blank_px[4] = {0, 0, 0, 0};
		vidow_x11_set_cursor(blank_px, 4, 1, 1, 0, 0);
	}

	/* New Xwayland instance: recheck its root against the virtual union. */
	server->xwayland_scale_ok = true;
	server->xwayland_scale_probed = false;
	server->xwayland_probe_tries = 0;

	/* 事件：X 服务真的起来了（懒启动，第一个 X 客户端连上来才有这一下）。
	 * 配置里 exec_once 起的 X11 程序要等它的话，听这个。 */
	const struct vidow_event_field fields[] = {
		VIDOW_EV_STR("display", server->xwayland_server != NULL ?
			server->xwayland_server->display_name : NULL),
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
		server->xwayland_server != NULL && server->xwayland_server != NULL ?
		server->xwayland_server->client : NULL);

	/* 再把渲染密度告诉 X（Xft.dpi = S × 96，几何另走逐屏映射）。
	 * 必须赶在第一个客户端连上之前 —— 客户端一进来就会按当时的 DPI 算布局。 */
	x11_apply_scale_props(server);
	xwayland_probe_schedule(server);

	/* 诊断用：X root 与统一虚拟输出并集是否一致。
	 * 这条日志是排查「X 窗口位置偏移 / 菜单跑到别的屏上」的第一现场。
	 * 注意不能用 xcb_get_setup() 里的屏幕尺寸 —— 那是连接建立时的那一份，属性
	 * 改了它也不会更新；root window 的 geometry 才是「现在」的。 */
	xcb_connection_t *conn = vidow_x11_connection();
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
	wl_list_remove(&server->xwayland_start.link);
	wl_list_remove(&server->xwayland_ready.link);
	wl_list_remove(&server->xwayland_destroy.link);
	/* 我们自己的 XWM 也要收干净：拆事件源、释放窗口对象。
	 * 必须在 xcb_disconnect 之前 —— 它读的是同一条连接。 */
	/* 退出时 main.c 已经先拆过了（它在 display destroy 之前就得动手，见那里的
	 * 注释），这里可能已经是 NULL。 */
	if (server->xwm != NULL) {
		vidow_x11_server_destroy(server->xwm);
		server->xwm = NULL;
	}
	server->xwayland_server = NULL;
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
		if (server->xwayland_server == NULL) {
			return true;
		}
		wlr_log(WLR_INFO, "xwayland: 配置里关了，拆掉 X 服务");
		xwayland_probe_stop(server);
		/* destroy 的信号处理会把我们的 XWM 也收掉、把 xwayland_server
		 * 置空。 */
		wlr_xwayland_server_destroy(server->xwayland_server);
		if (server->xwayland_shell != NULL) {
			wlr_xwayland_shell_v1_destroy(server->xwayland_shell);
			server->xwayland_shell = NULL;
		}
		unsetenv("DISPLAY");
		return true;
	}
	if (server->xwayland_server != NULL) {
		return true;
	}
	if (server->compositor == NULL) {
		wlr_log(WLR_ERROR, "xwayland: 没有 wlr_compositor，起不了");
		return false;
	}

	/* **只用 wlr_xwayland_server，不用 wlr_xwayland。**
	 *
	 * wlr_xwayland_server 只干三件事：fork/exec Xwayland、建 X socket、把
	 * wm_fd 交出来。窗口管理一概不管 —— 正好是我们要的。
	 *
	 * 这是 Hyprland 的结构：它的 CXWayland 手里就只有一个
	 * wlr_xwayland_server，XWM 完全自己写（src/xwayland/XWM.cpp）。
	 *
	 * enable_wm = true：让 Xwayland 建 wm_fd 并传 -wm 参数。这个 fd 到了
	 * xwayland_ready() 里就交给 xcb_connect_to_fd，成为我们自己的 XWM 连接。
	 * lazy = false：**必须关掉懒启动。**
	 *
	 * wlroots 的懒启动路径（server.c 的 server_start_lazy）是这样交接的：
	 * 先把 x_fd[0]/x_fd[1] 注册到合成器的事件循环，等第一个连接到来，
	 * xwayland_socket_connected() 把这两个注册**移除**，再 server_start()
	 * 把 fd 用 -listenfd 传给 Xwayland —— 从此 accept 的责任归 Xwayland。
	 *
	 * 但 Xwayland 24.1.14 不接管：实测它的 epoll 里只有 Wayland 连接和
	 * -wm 那个 fd，两个 -listenfd 一个都没注册，strace 下连接进了队列它
	 * 也**一次 accept 都不调**。于是第一个连接之后就再也进不来人 ——
	 * /tmp/.X11-unix/X0 的 accept 队列（backlog 只有 1，见 wlroots
	 * sockets.c 的 listen(fd, 1)）被 fcitx5 / xrdb 这类自启动客户端占满，
	 * 后面所有 X 应用都卡在 connect() 里：xclock 停在 unix_wait_for_peer、
	 * xdpyinfo 直接超时。用户看到的就是「X 阻塞了，X 应用打不开，自启动
	 * 流程也卡住」。
	 *
	 * lazy = false 走 server_start() 直接起，Xwayland 从第一刻就持有
	 * listen fd，不经过那段会丢注册的交接。 */
	struct wlr_xwayland_server_options options = {
		.lazy = false,
		.enable_wm = true,
		.no_touch_pointer_emulation = true,
		.force_xrandr_emulation = false,
		.terminate_delay = 0,
	};
	server->xwayland_server =
		wlr_xwayland_server_create(server->display, &options);
	if (server->xwayland_server == NULL) {
		wlr_log(WLR_ERROR, "xwayland: wlr_xwayland_server_create 失败");
		return false;
	}

	/* xwayland_shell_v1 是唯一还要 wlroots 参与的 X11 部件：它把 Xwayland
	 * 交上来的 wl_surface 和 X 窗口的 serial 对上号。窗口对象本身是我们的。
	 * 版本取 1 —— 这个协议只有 v1。 */
	if (server->xwayland_shell == NULL) {
		server->xwayland_shell =
			wlr_xwayland_shell_v1_create(server->display, 1);
		if (server->xwayland_shell == NULL) {
			wlr_log(WLR_ERROR, "xwayland: xwayland_shell_v1 建不起来");
		}
	}

	server->xwayland_start.notify = xwayland_start;
	wl_signal_add(&server->xwayland_server->events.start,
		&server->xwayland_start);
	server->xwayland_ready.notify = xwayland_ready;
	wl_signal_add(&server->xwayland_server->events.ready,
		&server->xwayland_ready);
	server->xwayland_destroy.notify = xwayland_destroy;
	wl_signal_add(&server->xwayland_server->events.destroy,
		&server->xwayland_destroy);
	/* 配置里启动的客户端要能连上 X：lazy 模式下 socket 现在就有了。 */
	setenv("DISPLAY", server->xwayland_server->display_name, 1);
	/* DISPLAY 变了就得再推一轮：systemd 拉起来的 X11 程序也要连得上 vidow 的 X */
	vidow_env_export_later(server);

	vidow_xwayland_scale_recompute(server);
	vidow_xwayland_env(server);

	wlr_log(WLR_INFO, "xwayland: DISPLAY=%s，懒启动，X 全局缩放 %.2f",
		server->xwayland_server->display_name, server->xwayland_scale);
	return true;
}
