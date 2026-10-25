#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/xwayland.h>
#include <wlr/util/log.h>

#include "server.h"
#include "xwm.h"

/* X11 表面的「该不该在屏幕上」判定，见 xwm.h 顶部的说明。
 *
 * 这一层存在的唯一目的：把「窗口该不该显示」这个问题收敛到一个判据上。
 * 以前它散在 x11_map_request / x11_associate / x11_replay_map_if_xmapped
 * 三处，各看各的前置条件，于是会出现「所有标志位都说正常、屏幕上却什么都
 * 没有」的死角（Steam 重开主窗口就是这个）。 */

bool xwm_surface_ready(const struct vidow_view *view) {
	return view != NULL && view->x11 != NULL && view->x11->surface != NULL;
}

bool xwm_surface_should_be_mapped(const struct vidow_view *view) {
	if (!xwm_surface_ready(view)) {
		/* 表面还没建出来：没法判，保持现状。 */
		return view != NULL && view->mapped;
	}
	/* 唯一的判据：客户端交过 buffer（并且我们有 texture 可以画）。
	 *
	 * 为什么不看 X 的 IsViewable：X 侧说 viewable 的时候我们可能一个像素
	 * 都还没收到，这时候把它「放上屏」只是给场景树一个空壳。反过来 X 侧
	 * 还没来得及报 viewable（map_request 早于 associate 到，Steam 关掉再
	 * 打开时实测就是这样）时，只要 buffer 到了就能画。
	 *
	 * 为什么不看我们自己的 x11_map_replayed / scene_parked：那些是**结果**，
	 * 拿结果当判据就会自锁 —— 有一次记错了，之后每次都按错的往下走。 */
	return wlr_surface_has_buffer(view->x11->surface);
}

enum xwm_sync_result xwm_surface_sync(struct vidow_view *view) {
	if (!xwm_surface_ready(view)) {
		return XWM_SYNC_NOOP;
	}
	/* 弹出物走自己那条路（x11_refresh_popup + 绝对定位），不参与这套
	 * 状态机：它们是屏幕级对象，不该被当成普通窗口收放。 */
	if (view->popup || view->focus_helper) {
		return XWM_SYNC_NOOP;
	}

	bool want = xwm_surface_should_be_mapped(view);
	if (want == view->mapped) {
		return XWM_SYNC_NOOP;
	}

	if (want) {
		wlr_log(WLR_DEBUG, "xwm: %s 有 buffer → 上屏（mapped=%d）",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			view->mapped);
		/* **必须同时告诉 wlr_surface 它上屏了**。
		 *
		 * wlr_surface->mapped 不是「有没有 buffer」，而是「合成器认不认
		 * 这个表面在屏幕上」。XWayland 会读它来决定要不要把 X pixmap
		 * 导出成 dmabuf 交给合成器：mapped 为假时它认为这个窗口没人看，
		 * 就不做那份拷贝 —— 于是我们拿到的纹理是**全零**的。
		 *
		 * 这就是 Steam 第二次开主窗口全黑的根因：那个 X 窗口是复用的
		 * （close-window 只发 WM_DELETE_WINDOW，Steam 保留窗口），重开
		 * 时走的是 MapNotify 而不是 MapRequest，wlroots 的
		 * xwm_handle_map_notify() 只清 withdrawn、不碰 mapped，所以
		 * surface->mapped 一直停在 0。我们这边 view->mapped 倒是 1、
		 * 场景节点也全开、纹理对象也有 3808x2050 —— 但里面一个像素都没有
		 * （实测首开 0xff666644，重开 0xff000000）。
		 *
		 * Hyprland 是同一条路：XSurface.cpp 的 map() 里调 m_surface->map()。 */
		struct wlr_surface *surf = vidow_view_surface(view);
		if (surf != NULL) {
			wlr_surface_map(surf);
		}
		/* 以前上过屏、现在又上来：XWayland 导出那份 dmabuf 可能还是
		 * 空的（实测重开 Steam 主窗口：纹理 3808x2050 但像素全零）。
		 * 抖一下尺寸逼它重建 pixmap。首次映射不用抖 —— 那时候
		 * x11_rebuild_w 还是 0。 */
		if (view->x11_rebuild_w > 0) {
			xwm_surface_force_rebuild(view);
		}
		view_map(&view->map, NULL);
		return XWM_SYNC_MAPPED;
	}

	wlr_log(WLR_DEBUG, "xwm: %s 没有 buffer → 收起来（mapped=%d）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		view->mapped);
	struct wlr_surface *surf2 = vidow_view_surface(view);
	if (surf2 != NULL) {
		wlr_surface_unmap(surf2);
	}
	view_unmap(&view->unmap, NULL);
	return XWM_SYNC_UNMAPPED;
}

/* ---- 重映射后逼 XWayland 重建 dmabuf ---------------------------------- */

/* 抖动分两拍：第一拍 +1 像素，第二拍（这个回调）还原。
 *
 * 为什么必须分两拍、不能一次 configure 两个尺寸：X 的 ConfigureRequest 是
 * 异步的，连着发两个不同尺寸，客户端可能只看到最后一个（中间那次被合并
 * 掉），于是「尺寸真的变了」这个条件一次都没成立 —— pixmap 还是不会重建。 */
static int x11_rebuild_restore(void *data) {
	struct vidow_view *view = data;
	if (view->x11 == NULL || view->x11->surface == NULL) {
		view->x11_rebuild_timer = NULL;
		return 0;
	}
	if (view->x11_rebuild_pending) {
		view->x11_rebuild_pending = false;
		wlr_xwayland_surface_configure(view->x11,
			view->x11->x, view->x11->y,
			(int16_t)view->x11_rebuild_w,
			(int16_t)view->x11_rebuild_h);
		wlr_log(WLR_DEBUG, "x11-rebuild: %s 还原 %dx%d",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			view->x11_rebuild_w, view->x11_rebuild_h);
	}
	view->x11_rebuild_timer = NULL;
	return 0;
}

/* 重新上屏时调一次：把窗口尺寸抖 +1 像素，下一拍还原。
 *
 * 只在「之前上过屏、现在又上来」时调 —— 首次映射不用（那时 XWayland 会
 * 正常分配 pixmap，抖一下纯属多余）。判据是 x11_rebuild_w 有没有被填过。 */
void xwm_surface_force_rebuild(struct vidow_view *view) {
	if (view == NULL || view->x11 == NULL || view->x11->surface == NULL) {
		return;
	}
	if (view->x11_rebuild_pending || view->x11_rebuild_timer != NULL) {
		return; /* 正在抖，别叠加 */
	}
	struct wlr_surface *surface = view->x11->surface;
	int w = surface->current.width;
	int h = surface->current.height;
	if (w <= 2 || h <= 2) {
		return;
	}
	if (view->server == NULL || view->server->display == NULL) {
		return;
	}
	view->x11_rebuild_w = w;
	view->x11_rebuild_h = h;
	view->x11_rebuild_pending = true;
	wlr_xwayland_surface_configure(view->x11, view->x11->x, view->x11->y,
		(int16_t)(w + 1), (int16_t)h);
	wlr_log(WLR_INFO, "x11-rebuild: %s 抖一下 %dx%d → %dx%d（逼 XWayland "
		"重建 pixmap）", vidow_view_title(view) != NULL ?
			vidow_view_title(view) : "(无题)", w, h, w + 1, h);
	if (view->x11_rebuild_timer == NULL) {
		view->x11_rebuild_timer = wl_event_loop_add_timer(
			wl_display_get_event_loop(view->server->display),
			x11_rebuild_restore, view);
	}
	if (view->x11_rebuild_timer != NULL) {
		wl_event_source_timer_update(view->x11_rebuild_timer, 50);
	}
}
