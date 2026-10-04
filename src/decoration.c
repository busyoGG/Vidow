/* zxdg_decoration_manager_v1 —— 告诉客户端「装饰由合成器画」。
 *
 * 为什么必须有：Wayland 没有别的方式让客户端知道这件事。vidow 不实现这个协议
 * 的时候，kitty / GTK / Qt 这些自己会画装饰的客户端（kitty 的 glfw-wayland 里
 * 明明白白带着 CSD 那套：Created decoration buffers / RedrawCSDWindowTitle）只能
 * 退回「自己画标题栏」，于是屏幕上出现一个跟窗口圆角对不上的方角标题栏 ——
 * 一边是我们画的 12px 圆角 + 边框 + 阴影，一边是客户端画的直角标题栏，接缝
 * 处两套装饰打架。实现了之后客户端把标题栏收回去，窗口只剩我们这一套装饰。
 *
 * 规则很简单，就是跟着 decor 配置走：
 *   decor=on  -> SERVER_SIDE（客户端别画了，我们的装饰说了算）
 *   decor=off -> CLIENT_SIDE（我们什么都不画，让客户端自己画）
 * 所以 decor 配置改了（SIGHUP / IPC set-config）要把已经连上的客户端重新
 * 通知一遍，见 vidow_decoration_apply_config()。
 *
 * 一个坑：wlr_xdg_toplevel_decoration_v1_set_mode() 内部会
 * wlr_xdg_surface_schedule_configure()，而那里面有一句
 * `assert(surface->initialized)` —— 客户端是先建 decoration 对象再第一次
 * commit 的，所以在 new_toplevel_decoration 里直接 set_mode 会当场 SIGABRT
 * （踩过：headless 里起个 kitty 就把合成器打死了）。所以这里的规矩是：
 * 只在 surface 已经 initialized 之后才下发模式，没就绪就等它的第一次 commit。 */

#include "server.h"

#include <stdlib.h>

#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>

/* 每个 decoration 一份监听器，挂在 wlr 对象的 data 上。 */
struct vidow_deco {
	struct vidow_server *server;
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	bool applied; /* 模式已经下发过了 */
	struct wl_listener request_mode;
	struct wl_listener commit;
	struct wl_listener destroy;
};

static enum wlr_xdg_toplevel_decoration_v1_mode
wanted_mode(struct vidow_server *server) {
	return server->config.decor_enabled ?
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE :
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
}

static const char *mode_name(enum wlr_xdg_toplevel_decoration_v1_mode mode) {
	switch (mode) {
	case WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
		return "服务端装饰";
	case WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
		return "客户端装饰";
	default:
		return "无";
	}
}

/* 按配置给客户端定调。surface 还没 initialized 就先不动（见文件头的说明），
 * 等它的第一次 commit 再来。 */
static void deco_apply(struct vidow_deco *d) {
	if (d->deco == NULL || d->deco->toplevel == NULL) {
		return;
	}
	if (!d->deco->toplevel->base->initialized) {
		return;
	}
	enum wlr_xdg_toplevel_decoration_v1_mode mode = wanted_mode(d->server);
	if (d->applied && d->deco->scheduled_mode == mode) {
		return;
	}
	wlr_log(WLR_DEBUG, "decor: 客户端想要 %s，按配置给 %s",
		mode_name(d->deco->requested_mode), mode_name(mode));
	wlr_xdg_toplevel_decoration_v1_set_mode(d->deco, mode);
	d->applied = true;
}

/* 客户端可以在任何时候 request_mode（通常一上来就请求）。它的请求只是「想要
 * 什么」，定调的是我们 —— 协议规定服务端说了算，客户端必须照 configure 里的
 * 模式来。 */
static void deco_request_mode(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_deco *d = wl_container_of(listener, d, request_mode);
	deco_apply(d);
}

/* 第一次 commit 之后 surface 才 initialized，这时候才轮得到我们下发模式。 */
static void deco_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_deco *d = wl_container_of(listener, d, commit);
	deco_apply(d);
}

static void deco_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_deco *d = wl_container_of(listener, d, destroy);

	wl_list_remove(&d->request_mode.link);
	wl_list_remove(&d->commit.link);
	wl_list_remove(&d->destroy.link);
	free(d);
}

static void server_new_decoration(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_decoration);
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;

	struct vidow_deco *d = calloc(1, sizeof(*d));
	if (d == NULL) {
		return;
	}
	d->server = server;
	d->deco = deco;

	d->request_mode.notify = deco_request_mode;
	wl_signal_add(&deco->events.request_mode, &d->request_mode);
	d->commit.notify = deco_commit;
	wl_signal_add(&deco->toplevel->base->surface->events.commit, &d->commit);
	d->destroy.notify = deco_destroy;
	wl_signal_add(&deco->events.destroy, &d->destroy);
	deco->data = d;

	/* 已经 initialized 的（客户端晚建 decoration）当场定调，否则等 commit。 */
	deco_apply(d);
}

bool vidow_decoration_init(struct vidow_server *server) {
	server->decoration_manager =
		wlr_xdg_decoration_manager_v1_create(server->display);
	if (server->decoration_manager == NULL) {
		wlr_log(WLR_ERROR, "decor: zxdg_decoration_manager_v1 建不出来");
		return false;
	}
	server->new_decoration.notify = server_new_decoration;
	wl_signal_add(&server->decoration_manager->events.new_toplevel_decoration,
		&server->new_decoration);
	return true;
}

void vidow_decoration_apply_config(struct vidow_server *server) {
	if (server->decoration_manager == NULL) {
		return;
	}
	struct wlr_xdg_toplevel_decoration_v1 *deco;
	wl_list_for_each(deco, &server->decoration_manager->decorations, link) {
		struct vidow_deco *d = deco->data;
		if (d != NULL) {
			/* 配置变了，强制重新下发 */
			d->applied = false;
			deco_apply(d);
			continue;
		}
		if (deco->toplevel != NULL && deco->toplevel->base->initialized) {
			wlr_xdg_toplevel_decoration_v1_set_mode(deco,
				wanted_mode(server));
		}
	}
}
