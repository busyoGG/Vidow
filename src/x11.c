/*
 * 独立于 wlroots 的 X11 观察层。
 *
 * 为什么要有它
 * ------------
 * wlroots 的 wlr_xwm 只在自己收到 MapRequest 时才把窗口标记成 mapped。
 * 一个**已经映射过**的窗口被客户端重新 map（Steam 关掉主窗口、再点开）
 * 时，X 发的是 MapNotify 而不是 MapRequest —— wlroots 的
 * xwm_handle_map_notify() 只清 withdrawn、不碰 mapped，于是
 * wlr_xwayland_surface.mapped 永远停在 0。
 *
 * 我们不信那个字段（判定只看有没有 buffer），但 Vidow 仍然缺一个
 * 「X 服务器眼里这个窗口现在到底是什么状态」的权威来源。这个文件就是
 * 那个来源：自己开一条 xcb 连接，只看、不抢。
 *
 * 为什么只订阅 SubstructureNotify
 * -------------------------------
 * SubstructureRedirect 一个时刻只能有一个客户端持有，wlroots 已经拿了。
 * 硬抢会让 wlroots 收不到 MapRequest，整个 XWayland 直接瘫。所以我们只
 * 加只读的 SubstructureNotify：X 照样把 map/unmap/configure 通知给我们
 * 一份，我们据此记账，不去改任何窗口行为。
 *
 * 生命周期
 * --------
 * x11_observe_start() 在 XWayland ready 之后调，x11_observe_stop() 在
 * 退出时调。连接断了就整条停掉，不影响合成器本体。
 */

#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/util/log.h>
#include <xcb/xcb.h>

#include "server.h"
#include "x11.h"

/* 一条独立连接 + 自己的事件源。 */
struct x11_observe {
	struct vidow_server *server;
	xcb_connection_t *conn;
	struct wl_event_source *source;
	bool running;
	/* 诊断计数：确认事件真的在流动，而不是「订阅了但一条没来」。 */
	unsigned map_notify;
	unsigned unmap_notify;
	unsigned configure_notify;
	unsigned destroy_notify;
};

static struct x11_observe observe;

/* 只读查询：这个 X 窗口在服务器眼里是不是可见的。
 *
 * 这是本文件存在的唯一理由 —— wlroots 的 mapped 字段回答不了这个问题
 * （见文件头）。走自己的连接，不和 wlroots 的请求队列抢顺序。 */
bool x11_observe_is_viewable(xcb_window_t window) {
	if (!observe.running || observe.conn == NULL ||
			window == XCB_WINDOW_NONE) {
		return false;
	}
	xcb_get_window_attributes_cookie_t cookie =
		xcb_get_window_attributes(observe.conn, window);
	xcb_get_window_attributes_reply_t *reply =
		xcb_get_window_attributes_reply(observe.conn, cookie, NULL);
	if (reply == NULL) {
		return false;
	}
	bool viewable = reply->map_state == XCB_MAP_STATE_VIEWABLE;
	free(reply);
	return viewable;
}

/* 事件循环回调：把 xcb 队列取干净，顺手分类计数。
 *
 * 这里**什么都不改**：只记账。一旦在这里改窗口属性，就等于和 wlroots
 * 抢同一个客户端，两边状态会打架。 */
static int x11_observe_dispatch(int fd, uint32_t mask, void *data) {
	(void)fd;
	(void)mask;
	(void)data;
	if (!observe.running || observe.conn == NULL) {
		return 0;
	}
	xcb_generic_event_t *ev;
	while ((ev = xcb_poll_for_event(observe.conn)) != NULL) {
		switch (ev->response_type & 0x7f) {
		case XCB_MAP_NOTIFY: {
			xcb_map_notify_event_t *e =
				(xcb_map_notify_event_t *)ev;
			observe.map_notify++;
			wlr_log(WLR_INFO, "x11-observe: MapNotify 0x%x",
				(unsigned)e->window);
			break;
		}
		case XCB_UNMAP_NOTIFY: {
			xcb_unmap_notify_event_t *e =
				(xcb_unmap_notify_event_t *)ev;
			observe.unmap_notify++;
			wlr_log(WLR_INFO, "x11-observe: UnmapNotify 0x%x",
				(unsigned)e->window);
			break;
		}
		case XCB_CONFIGURE_NOTIFY: {
			xcb_configure_notify_event_t *e =
				(xcb_configure_notify_event_t *)ev;
			observe.configure_notify++;
			wlr_log(WLR_INFO, "x11-observe: ConfigureNotify 0x%x "
				"%dx%d",
				(unsigned)e->window, e->width, e->height);
			break;
		}
		case XCB_DESTROY_NOTIFY: {
			xcb_destroy_notify_event_t *e =
				(xcb_destroy_notify_event_t *)ev;
			observe.destroy_notify++;
			wlr_log(WLR_INFO, "x11-observe: DestroyNotify 0x%x",
				(unsigned)e->window);
			break;
		}
		default:
			break;
		}
		free(ev);
	}
	if (xcb_connection_has_error(observe.conn)) {
		wlr_log(WLR_ERROR, "x11-observe: 连接断了，停止观察");
		observe.running = false;
	}
	return 0;
}

void x11_observe_start(struct vidow_server *server) {
	if (observe.running) {
		return;
	}
	memset(&observe, 0, sizeof(observe));
	observe.server = server;

	int screen_num = 0;
	observe.conn = xcb_connect(NULL, &screen_num);
	if (observe.conn == NULL || xcb_connection_has_error(observe.conn)) {
		wlr_log(WLR_ERROR, "x11-observe: 连不上 X，跳过观察");
		if (observe.conn != NULL) {
			xcb_disconnect(observe.conn);
		}
		observe.conn = NULL;
		return;
	}

	/* 只订阅根窗口的 SubstructureNotify：所有顶层窗口的 map / unmap /
	 * configure / destroy 都会抄送一份给我们，且**不抢** wlroots 的
	 * SubstructureRedirect。 */
	const xcb_setup_t *setup = xcb_get_setup(observe.conn);
	xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
	xcb_screen_t *screen = it.data;
	if (screen == NULL) {
		wlr_log(WLR_ERROR, "x11-observe: 拿不到屏幕，跳过观察");
		xcb_disconnect(observe.conn);
		observe.conn = NULL;
		return;
	}
	uint32_t mask = XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
	xcb_change_window_attributes(observe.conn, screen->root,
		XCB_CW_EVENT_MASK, &mask);
	xcb_flush(observe.conn);

	observe.source = wl_event_loop_add_fd(
		wl_display_get_event_loop(server->display),
		xcb_get_file_descriptor(observe.conn),
		WL_EVENT_READABLE, x11_observe_dispatch, NULL);
	if (observe.source == NULL) {
		wlr_log(WLR_ERROR, "x11-observe: 装不上事件源，跳过观察");
		xcb_disconnect(observe.conn);
		observe.conn = NULL;
		return;
	}

	observe.running = true;
	wlr_log(WLR_INFO, "x11-observe: 已连上 X（root=0x%x），只读观察",
		(unsigned)screen->root);
}

void x11_observe_stop(void) {
	if (observe.source != NULL) {
		wl_event_source_remove(observe.source);
		observe.source = NULL;
	}
	if (observe.conn != NULL) {
		xcb_disconnect(observe.conn);
		observe.conn = NULL;
	}
	if (observe.running) {
		wlr_log(WLR_INFO, "x11-observe: 停止。收到 map=%u unmap=%u "
			"configure=%u destroy=%u",
			observe.map_notify, observe.unmap_notify,
			observe.configure_notify, observe.destroy_notify);
	}
	observe.running = false;
}

void x11_observe_report(void) {
	if (!observe.running) {
		wlr_log(WLR_INFO, "x11-observe: 未运行");
		return;
	}
	wlr_log(WLR_INFO, "x11-observe: map=%u unmap=%u configure=%u destroy=%u",
		observe.map_notify, observe.unmap_notify,
		observe.configure_notify, observe.destroy_notify);
}
