#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "server.h"
#include "x11surface.h"
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
		 * 这个表面在屏幕上」。Xwayland 会读它来决定要不要把 X pixmap
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
