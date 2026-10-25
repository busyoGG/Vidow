#include <wayland-server-core.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/log.h>

#include "server.h"
#include "xtrace.h"
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
	if (vidow_xtrace_on()) {
		wlr_log(WLR_INFO, "sync-trace: %s xid=0x%x want=%d mapped=%d "
			"buffer=%d surf=%p view=%p",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			view->x11->window_id, want, view->mapped,
			wlr_surface_has_buffer(view->x11->surface),
			(void *)view->x11->surface, (void *)view);
	}
	/* **判据要连 wlr_surface->mapped 一起看，不能只看 view->mapped。**
	 *
	 * 用户报的「Steam 第二次打开窗口还是黑屏，需要拖拽或者 resize 才好」。
	 *
	 * 黑屏时实测（IPC）：buffer=3808x2050、dst=1904x1025、mapped=true、
	 * 场景节点全开 —— 数据一个不缺，屏幕上全黑。resize 一下（哪怕只把
	 * 列宽从 1 改成 0.333）立刻出画面，颜色数 1 → 166。
	 *
	 * 原因是这两个 mapped 是两个东西：
	 *   view->mapped       我们自己记的「这个窗口在布局里展开着」；
	 *   surface->mapped    wlroots 的「合成器认这块表面在屏上」。
	 * Xwayland 读的是后者 —— 它为假时认为没人看这个窗口，**不把 X
	 * pixmap 导出成 dmabuf**，我们拿到的纹理就是全零（首开 0xff666644，
	 * 重开 0xff000000）。
	 *
	 * Steam 复用同一个 X 窗口重开时走 MapNotify 而不是 MapRequest，
	 * wlroots 的 xwm_handle_map_notify() 只清 withdrawn、不碰 mapped，
	 * 而 view->mapped 因为「上次还开着」仍然是 1。于是
	 * want(1) == view->mapped(1) 直接返回，下面那句 wlr_surface_map()
	 * 一次都没跑过 —— 表面永远不被承认，纹理永远是空的。
	 *
	 * resize 之所以能救：configure 改了尺寸 → 客户端重新提交 →
	 * x11_drop_surface() 把 view->mapped 清成 0 → 下一次 commit 时
	 * want(1) != mapped(0)，wlr_surface_map() 终于执行。
	 *
	 * 所以提前返回的条件里必须带上表面自己的状态：两个都对齐了才算
	 * 真的对齐。 */
	bool surf_mapped = view->x11->surface->mapped;
	if (want == view->mapped && (!want || surf_mapped)) {
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
		/* view_map() 头一句就是 if (view->mapped) return; —— 复用窗口
		 * 重开时 view->mapped 本来就是 1，它会直接返回，布局那边一次都
		 * 不刷新。可场景树在 dissociate 时被拆过（x11_drop_surface 把
		 * scene_tree 清成 NULL），不重挂就还是黑的。
		 *
		 * 所以这里补一次：表面重新挂上、场景树重建、状态按当前布局
		 * 重新对齐。vidow_view_attach_x11_surface 内部也是幂等的
		 * （x11_attach_surface 见 scene_tree != NULL 就跳过）。 */
		/* **两种「重新上屏」都要让客户端重画一次。**
		 *
		 * trace 实测（Steam 关掉再开，VIDOW_X11_TRACE）：
		 *     want=0 mapped=0   ← unmap 时已清
		 *     want=1 mapped=0   ← 重开走的是这一支，view_map()
		 *     want=1 mapped=1
		 * 也就是说复用窗口重开时 view->mapped 已经是 0，下面那条
		 * view->mapped 分支根本不会进 —— 之前把清 cfg_sent 放在那里
		 * 是放错位置了，一次都没执行。
		 *
		 * 几何没变时 view.c 的 configure 路径会命中「和上次一模一样」
		 * 直接 return，客户端等一个永远不来的 ConfigureNotify，画面
		 * 定在重开时那一帧空白上。所以这里无条件清一次，让紧接着的
		 * arrange 把 configure 真发出去。 */
		if (view->x11 != NULL) {
			view->cfg_sent = false;
		}
		if (view->mapped) {
			if (view->scene_tree == NULL) {
				vidow_view_attach_x11_surface(view);
			}
			/* 场景树重建后节点是禁用状态（x11_attach_surface 末尾
			 * set_enabled(false)），得让布局重新算一遍把它打开、
			 * 摆到正确位置。 */
			vidow_arrange(view->server);
			/* **还得让 X 客户端重画一次。**
			 *
			 * 光把表面 map 上不够：Steam 重开时只交了一帧空白就不再动
			 * 了（实测 commit 只有 1 次，首开是两千多次）。它要收到
			 * configure 才肯重新渲染 —— 用户说的「拖拽或者 resize
			 * 一下就好」正是这个原因。
			 *
			 * 复用窗口重开时位置和尺寸和上次一模一样，于是 arrange 走到
			 * view.c 那条 configure 路径时命中「和上次一模一样」直接
			 * return —— configure 压根没发出去，客户端等一个永远不会来
			 * 的 ConfigureNotify，画面就这么定在黑色上。
			 * （清 cfg_sent 已挪到上面无条件执行。） */
		} else {
			view_map(&view->map, NULL);
		}
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
