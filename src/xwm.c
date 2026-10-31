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
	/* **弹出物也要把 wlr_surface 放上屏，只是不走布局那一套。**
	 *
	 * 这里以前是 `if (view->popup || view->focus_helper) return NOOP;` ——
	 * 把弹出物整个排除在状态机之外。后果是**所有 X11 菜单 / 下拉框 /
	 * 工具提示都画不出来**（实测 test-xwayland-popup.sh：18 个 FAIL，
	 * 弹出物矩形里 24000/24000 个像素还是父窗口的颜色）。
	 *
	 * 为什么排除会致命：wlr_surface->mapped 不是「有没有 buffer」，而是
	 * 「合成器认不认这块表面在屏幕上」。Xwayland 读它决定要不要把 X
	 * pixmap 导出成 dmabuf。弹出物被排除后那句 wlr_surface_map() 一次
	 * 都不跑，于是：
	 *   - 日志里永远是 `buffer=1 surf_mapped=0`（表面有内容、没人认）；
	 *   - x11_attach_surface 建完树 set_enabled(false) 之后，只有
	 *     view_map() 会把它打开，而 view_map() 也在这套状态机里 ——
	 *     节点就一直关着。fx 那边的日志直接写出来了：
	 *     「弹出物节点关着（没映射 / 在 overview 里 / 工作区藏起来了）」。
	 *
	 * Hyprland 没有这个例外：XSurface.cpp 的 map() 对所有 X 表面都调
	 * m_surface->map()，弹出物只是**不进布局**，不是**不上屏**。这里照它
	 * 改：surface 该 map 就 map，布局那半边继续由 x11_refresh_popup +
	 * 绝对定位负责。
	 *
	 * focus_helper 仍然整条跳过 —— 那些是辅助窗口，本来就不该显示。 */
	if (view->focus_helper) {
		return XWM_SYNC_NOOP;
	}

	bool want = xwm_surface_should_be_mapped(view);
	if (vidow_xtrace_on()) {
		/* 一次性抓齐渲染管线的全部关键状态，别再一轮一轮猜。 */
		struct wlr_scene_buffer *sb =
			view->scene_surface != NULL ? view->scene_surface->buffer : NULL;
		struct wlr_fbox sbox = {0};
		bool has_sbox = false;
		if (sb != NULL) {
			sbox = sb->src_box; /* 公开字段，见 wlr_scene.h:192 */
			has_sbox = true;
		}
		struct wlr_box geom = vidow_view_geometry(view);
		wlr_log(WLR_INFO, "sync-trace: %s xid=0x%x want=%d mapped=%d "
			"buffer=%d surf=%p view=%p | sb=%p en=%d pos=%d,%d "
			"dest=%dx%d src=%.0f,%.0f %.0fx%.0f(%d) geom=%d,%d %dx%d "
			"clip_on=%d parked=%d",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			view->x11->window_id, want, view->mapped,
			wlr_surface_has_buffer(view->x11->surface),
			(void *)view->x11->surface, (void *)view,
			(void *)sb, sb != NULL ? sb->node.enabled : -1,
			sb != NULL ? sb->node.x : 0, sb != NULL ? sb->node.y : 0,
			sb != NULL ? sb->dst_width : 0,
			sb != NULL ? sb->dst_height : 0,
			has_sbox ? sbox.x : -1.0, has_sbox ? sbox.y : -1.0,
			has_sbox ? sbox.width : -1.0, has_sbox ? sbox.height : -1.0,
			has_sbox ? 1 : 0,
			geom.x, geom.y, geom.width, geom.height,
			view->fx.clip_on, view->scene_parked);
		if (view->fx.clip_on) {
			wlr_log(WLR_INFO, "sync-trace: %s clip框=%d,%d %dx%d",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				view->fx.clip.x, view->fx.clip.y,
				view->fx.clip.width, view->fx.clip.height);
		}
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
	if (vidow_xtrace_on() && view->scene_surface != NULL) {
		/* **场景表面和 X 表面必须是同一块。**
		 *
		 * 用户报「Steam 第二次打开黑屏，拖拽或 resize 一下才好」。
		 * 实测：重开后客户端 commit 了 872 次、surface->mapped=1、
		 * view->mapped=1、configure 也确实发出去了，屏幕上还是全黑；
		 * 而 cycle-column-width 一下立刻出画面 —— 且那一瞬间
		 * **commit 次数是 0**。也就是说纹理一直是好的，只是场景节点
		 * 采样的不是这块表面。 */
		bool same = view->scene_surface->surface == view->x11->surface;
		wlr_log(WLR_INFO, "scene-trace: %s scene_surf=%p x11_surf=%p %s "
			"scene_mapped=%d x11_mapped=%d",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)",
			(void *)view->scene_surface->surface,
			(void *)view->x11->surface,
			same ? "一致" : "**不一致，画的是旧表面**",
			view->scene_surface->surface->mapped,
			view->x11->surface->mapped);
	}
	/* 表面和状态全对、屏幕还是黑的时候，唯一剩下的可能就是这个
	 * buffer 本身是空的。直接读像素。 */
	if (vidow_xtrace_on() && want && !surf_mapped) {
		vidow_view_probe_x11_texture(view);
	}
	/* **弹出物的位置要每次 commit 都重摆，所以它不参与「已经对齐就早退」。**
	 *
	 * 普通窗口早退是对的：want 和 mapped 一致、表面也在屏上了，就没有任何
	 * 事情要做。但弹出物不一样 —— 它的坐标是**客户端随时可以改的**
	 * （xpopup 的 move=、Qt 的下拉框跟着输入框挪、菜单被推到另一块屏）。
	 * 位置不在 X 的 map 状态里，改位置只来一条 ConfigureNotify /
	 * ConfigureRequest，surface 那边完全不变。
	 *
	 * 早退挡在前面，那些移动就一次都不生效：实测 test-xwayland-popup.sh
	 * 「属性晚到」那节，客户端说 500,500，窗口停在 760,200 不动。 */
	if (!view->popup && want == view->mapped && (!want || surf_mapped)) {
		return XWM_SYNC_NOOP;
	}
	if (view->popup && want && view->scene_tree != NULL) {
		struct wlr_box box = {0};
		if (vidow_view_x11_popup_box(view, &box)) {
			wlr_scene_node_set_position(&view->scene_tree->node,
				box.x, box.y);
		}
		wlr_scene_node_set_enabled(&view->scene_tree->node, true);
		if (view->scene_surface != NULL) {
			vidow_view_x11_scale(view);
		}
		if (!surf_mapped) {
			struct wlr_surface *psurf = vidow_view_surface(view);
			if (psurf != NULL) {
				wlr_surface_map(psurf);
			}
		}
		return XWM_SYNC_MAPPED;
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
		/* **弹出物到此为止。**
		 *
		 * 上面的 wlr_surface_map() 是它唯一需要的东西：表面被合成器
		 * 认下来，Xwayland 才会导出 dmabuf，fx / 场景节点才有东西可画。
		 *
		 * 下面那整段（view_map / vidow_arrange / x11_scale / configure
		 * 补发）是给**布局里的窗口**准备的，弹出物一概不能碰：
		 *   - 它不进布局（x11_refresh_popup 已经把它挂到 layer_popups）；
		 *   - 位置尺寸由客户端绝对坐标定（vidow_view_x11_popup_box），
		 *     跑一遍 arrange 会被浮窗/平铺规则摆到别处去；
		 *   - view_map() 里有工作区、焦点、开窗动画一整套，弹出物不该有。
		 *
		 * 但节点必须在这里显式打开：x11_attach_surface 建完树是
		 * set_enabled(false) 的，而弹出物不会走 view_map()，没人会替它
		 * 打开 —— 这就是「弹出物节点关着」那条日志的来源。 */
		/* 弹出物在上面那条专用分支里已经处理完（surface map + 摆位 +
		 * 打开节点），走不到这里。 */

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
			/* attach 重建的树是关着的（x11_attach_surface 末尾
			 * set_enabled(false)），点亮交给这里而不是 set_parked：
			 * 复用窗口重开不过 view_map（view->mapped 已是 1），而
			 * set_parked 的 false→false 早退没人点亮 —— Steam 二次打开
			 * 黑屏。dest 的校正在下面 x11_scale，同一个调用栈里跑完，
			 * 中间不出帧，不会闪出错尺寸的一帧。 */
			if (view->scene_tree != NULL) {
				wlr_scene_node_set_enabled(&view->scene_tree->node, true);
			}
			/* 场景树重建后节点是禁用状态（x11_attach_surface 末尾
			 * set_enabled(false)），得让布局重新算一遍把它打开、
			 * 摆到正确位置。 */
			vidow_arrange(view->server);
			/* **attach 之后要再校一次 dest。**
			 *
			 * 探针实测（2026-10-26）：commit → vidow_view_x11_scale() 写
			 * dst=1904、读回也是 1904，但紧接着
			 * wlr_scene_surface_create() 末尾的 surface_reconfigure() 把 dst
			 * 设回 **surface 的逻辑尺寸** 3808（wlroots scene/surface.c:148
			 * `set_dest_size(scene_buffer, state->width, state->height)`）。
			 * 场景节点于是 2 倍大、右下一半落在屏外。
			 *
			 * 这不是 Steam 重开黑屏的**全部**原因（dest 校回 1904 之后画面
			 * 仍然是黑的，见 view.c 里那段排查记录），但它确实是错的：
			 * 节点尺寸必须和窗口几何一致，否则输入命中和裁剪都按错的框算。
			 * 场景节点就位之后在这里重设一遍，幂等。 */
			if (view->kind == VIDOW_VIEW_X11) {
				vidow_view_x11_scale(view);
				if (vidow_xtrace_on() && view->scene_surface != NULL &&
						view->scene_surface->buffer != NULL) {
					wlr_log(WLR_INFO, "xwm: %s attach 后校 dest=%dx%d",
						vidow_view_title(view) != NULL ?
							vidow_view_title(view) : "(无题)",
						view->scene_surface->buffer->dst_width,
						view->scene_surface->buffer->dst_height);
				}
			}
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
