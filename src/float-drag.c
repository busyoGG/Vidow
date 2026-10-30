#include "server.h"
#include "float-internal.h"
#include "ipc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

/* 浮动窗口的几何与拖拽：摆放 / 吸附 / 跨屏 / 自由拖动中的锚点（S8）。
 *
 * 和 float.c 的分界：float.c 管「这个窗口是不是浮动的、是哪种浮动」（规则、模式、
 * 运行时切换）；这里管「浮动窗口摆哪儿、拖它的时候发生了什么」。mini 小窗的甩出
 * 与形态过渡在 float-mini.c。
 *
 * 这一块对外只有 vidow_float_* 那几个入口（server.h），内部 static 一个都不外露，
 * 所以拆出来不需要任何内部头文件。 */
/* ------------------------------------------------------------ geometry */

void vidow_float_on_commit(struct vidow_view *view) {
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		return;
	}
	/* 弹出物的尺寸由客户端定，我们既不夹到工作区也不居中（夹一次、居一次都会
	 * 把它挪到错的地方），摆位统一走 vidow_popup_place()。 */
	if (view->popup) {
		vidow_popup_place(view);
		return;
	}
	/* 全屏期间不采纳客户端尺寸：整屏大小是**我们** configure 给它的（见 arrange
	 * 的全屏分支），不是「它想要的浮窗尺寸」。记进 float_geom 的话，退出全屏之后
	 * 浮窗就停在整屏那么大 —— Mod+F 开一下、再关一下，浮窗就回不去了。 */
	if (view->fullscreen) {
		return;
	}

	/* 小窗态：尺寸由形态决定，不是「客户端想要的浮窗尺寸」。
	 *   MINI / ICON  永远跟配置，客户端自己 resize 不作数；
	 *   FREE         记比例（mini_wf/mini_hf），客户端可以改 —— 改完回写比例。
	 * 目标尺寸跟客户端现在的不一样就再 configure 一次（尺寸钉子：float_want_w/h
	 * 一直钉着，一个迟到的提交吃不掉它）。 */
	if (view->mini != VIDOW_MINI_NONE) {
		/* S3：保留客户端原生分辨率（默认）时，小窗态**不采纳客户端尺寸、也不
		 * configure** —— 客户端 buffer 保持原生，由合成器把场景节点缩到小窗盒
		 * 显示（view.c 的 view_mini_scale）。客户端完全不知道自己在小窗里：
		 * 不重配、不重建 swapchain、不触发它的 resize 路径（有的游戏一 resize
		 * 就重置成窗口模式）。盒子由形态 / 锚点决定，客户端说什么都不作数。
		 *
		 * 只有关掉保留（规则 / 全局键 mini_preserve_buffer = false）才走下面
		 * 那条老路：把客户端 configure 成小窗尺寸。
		 *
		 * **只管迷你 / 图标**：自由小窗是正常窗户语义，盒子多大就 configure 客户端
		 * 多大多，它的提交照常采纳（见 view.c 的 vidow_view_set_size 同一处收窄）。 */
		if ((view->mini == VIDOW_MINI_MINI ||
				view->mini == VIDOW_MINI_ICON) &&
				vidow_mini_preserve_buffer(view)) {
			return;
		}
		int cw = 0, ch = 0;
		vidow_view_client_size(view, &cw, &ch);
		/* 「我让它多大」还没被客户端认下来（拖拽改大小刚 configure、它还在交
		 * 旧尺寸）：这次提交**不采纳** —— 采纳就等于让一个迟到的旧尺寸把刚拖出
		 * 来的盒子改回去（拖了等于没拖）。下面那段「盒子跟客户端不一样就再催一次
		 * configure」负责把 want 再钉一遍。 */
		bool stale = view->float_want_w > 0 &&
			(cw != view->float_want_w || ch != view->float_want_h);
		if (!stale && view->mini == VIDOW_MINI_FREE && cw > 1 && ch > 1 &&
				view->server->config.mini_remember_size) {
			struct wlr_box work =
				vidow_work_area(view->server, view->output);
			if (work.width > 0 && work.height > 0) {
				view->mini_wf = (double)cw / (double)work.width;
				view->mini_hf = (double)ch / (double)work.height;
				view->mini_size_known = true;
			}
		}
		vidow_mini_resolve_geom(view);
		if (view->float_geom.width > 0 && view->float_geom.height > 0 &&
				(cw != view->float_geom.width ||
					ch != view->float_geom.height)) {
			view->float_want_w = view->float_geom.width;
			view->float_want_h = view->float_geom.height;
			vidow_view_set_size(view, view->float_geom.width,
				view->float_geom.height);
			vidow_arrange(view->server);
		}
		return;
	}

	/* 客户端自己要的尺寸，逻辑单位。X11 的 x11_want_w/h 是**设备像素**
	 * （X 屏幕按 xwayland_scale 放大了），所以下面要除回去 —— 见 view.c 的
	 * vidow_view_x11_scale_int 与 xwayland.c 的 KDE 模型注释。 */
	int w = 0, h = 0;
	vidow_view_client_size(view, &w, &h);
	/* 1x1 不是「客户端想要 1x1」，是「还没定尺寸」：X11 的窗口生出来就是 1x1，
	 * 客户端 map 之后（甚至更晚）才 resize。把这个尺寸写进 float_geom，arrange
	 * 就会回一个 configure(x, y, 1, 1) —— 等于命令客户端保持 1x1，GTK 的对话框
	 * 就这么永远长不出来。等它自己报出真实尺寸。 */
	if (w <= 1 || h <= 1) {
		/* 还没定尺寸，但它可能已经把「我想要多大」写在尺寸提示（min == max）
		 * 或者刚才那个 configure 请求里了 —— 两条都试试。 */
		int hw = 0, hh = 0;
		if (vidow_view_fixed_size_hint(view, &hw, &hh)) {
			w = hw;
			h = hh;
		} else if (view->kind == VIDOW_VIEW_X11 && view->x11_want_w > 1 &&
				view->x11_want_h > 1) {
			double lw = view->x11_want_w, lh = view->x11_want_h;
			vidow_xwm_from_native_size(view->server, view->output,
				lw, lh, &lw, &lh);
			w = (int)lround(lw);
			h = (int)lround(lh);
		} else {
			wlr_log(WLR_DEBUG, "float %s: 尺寸还没定（%dx%d），先不摆",
				vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
				w, h);
			return;
		}
	}

	/* 「我让它多大」还没被客户端认下来：保持那个盒子，并再催一次 configure。
	 * 客户端为别的原因提交的旧尺寸不能把盒子改回去。 */
	if (view->float_want_w > 0) {
		if (w == view->float_want_w && h == view->float_want_h) {
			view->float_want_w = 0; /* 到位了，以后客户端说了算 */
			view->float_want_h = 0;
		} else {
			if (view->float_geom.width != view->float_want_w ||
					view->float_geom.height != view->float_want_h) {
				view->float_geom.width = view->float_want_w;
				view->float_geom.height = view->float_want_h;
				vidow_view_set_size(view, view->float_want_w,
					view->float_want_h);
				vidow_arrange(view->server);
			}
			return;
		}
	}

	struct wlr_box work = vidow_work_area(view->server, view->output);
	if (work.width > 0 && work.height > 0) {
		if (w > work.width) {
			w = work.width;
		}
		if (h > work.height) {
			h = work.height;
		}
	}

	bool first = view->float_geom.width <= 0 || view->float_geom.height <= 0;
	if (!first && view->float_geom.width == w && view->float_geom.height == h) {
		return;
	}

	view->float_geom.width = w;
	view->float_geom.height = h;

	if (first) {
		if (view->float_mode == VIDOW_FLOAT_FOLLOW && view->parent != NULL) {
			/* centre the dialog over the parent tile */
			int dx = (view->parent->geom.width - w) / 2;
			int dy = (view->parent->geom.height - h) / 2;
			view->float_geom.x = dx > 0 ? dx : 0;
			view->float_geom.y = dy > 0 ? dy : 0;
		} else {
			view->float_geom.x = work.x + (work.width - w) / 2;
			view->float_geom.y = work.y + (work.height - h) / 2;
		}
		wlr_log(WLR_INFO, "float %s: %dx%d at %d,%d (mode %d)",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			w, h, view->float_geom.x, view->float_geom.y, view->float_mode);
	}

	vidow_arrange(view->server);
}

static void float_place_recursive(struct vidow_view *view, int depth,
		int slide_y) {
	if (depth > FLOAT_MAX_DEPTH) {
		return;
	}

	if (view->float_mode == VIDOW_FLOAT_FOLLOW) {
		if (view->parent != NULL) {
			if (view->parent->float_mode != VIDOW_FLOAT_NONE) {
				/* make sure the parent has its current box first */
				float_place_recursive(view->parent, depth + 1, slide_y);
			}
			if (view->float_geom.width > 0 && view->float_geom.height > 0) {
				vidow_place_view(view,
					view->parent->geom.x + view->float_geom.x,
					view->parent->geom.y + view->float_geom.y,
					view->float_geom.width, view->float_geom.height);
			}
			return;
		}
		/* parent vanished: keep the spot we are at, as an independent float */
		view->float_mode = VIDOW_FLOAT_FIXED;
		view->float_geom.x = view->geom.x;
		view->float_geom.y = view->geom.y;
	}

	if (view->float_mode == VIDOW_FLOAT_FIXED &&
			view->float_geom.width > 0 && view->float_geom.height > 0) {
		/* 小窗态：位置尺寸**每次**都按比例锚点重算（锚点 -> 工作区），不走
		 * 下面那套 home_dx/home_dy 的绝对偏移 —— 小窗是屏级对象，跟的是
		 * 「工作区的相对位置」，换工作区尺寸 / 改缩放 / 拔插屏之后天然对得上。
		 * 这是唯一的换算点（vidow_mini_set_state 里那次是为了让 IPC 立刻能
		 * 查到正确的 float_geometry，不重复也行，重算幂等）。 */
		if (view->mini != VIDOW_MINI_NONE) {
			vidow_mini_resolve_geom(view);
			vidow_place_view(view, view->float_geom.x, view->float_geom.y,
				view->float_geom.width, view->float_geom.height);
			return;
		}
		/* 浮动窗口remember 的是「相对它现在那块屏原点」的偏移（home_dx/home_dy）。
		 *
		 * 为什么不用绝对坐标：拔掉 / 插上一块屏之后 wlroots 会重排剩下的输出
		 * （撞见过：右边那块从 x=1280 挪到 x=0），而且拔线那一刻问到的目的屏位置
		 * 还是旧的 —— 绝对坐标会留在原地，相对坐标才能跟着屏走。
		 *
		 * 这里是唯一的换算点：偏移 -> 绝对 -> 摆好 -> 把偏移刷新回来（拖过窗口、
		 * 客户端改过尺寸之后偏移也跟着更新）。第一次（还没摆过）按客户端给的绝对
		 * 位置摆。 */
		struct wlr_box box = {0};
		if (view->output != NULL) {
			wlr_output_layout_get_box(view->server->output_layout,
				view->output, &box);
		}
		if (view->placed && box.width > 0) {
			view->float_geom.x = box.x + view->home_dx;
			view->float_geom.y = box.y + view->home_dy;
		}
		vidow_place_view(view, view->float_geom.x,
			view->float_geom.y + slide_y,
			view->float_geom.width, view->float_geom.height);
		if (box.width > 0) {
			view->home_dx = view->float_geom.x - box.x;
			view->home_dy = view->float_geom.y - box.y;
		}
	}
}

void vidow_float_place_all(struct vidow_server *server,
		struct vidow_output *output) {
	struct vidow_workspace *shown =
		vidow_workspace_shown_for(server, output->wlr_output);
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->float_mode == VIDOW_FLOAT_NONE) {
			continue;
		}
		if (view->popup) {
			/* 弹出物走 vidow_popup_place_all()：它的位置是屏幕绝对坐标，跟
			 * 哪块输出在摆无关。 */
			continue;
		}
		if (view->output != output->wlr_output) {
			continue;
		}
		/* 正被推开的那块工作区里的浮窗也要摆：它和列里的窗口一样，过渡期间
		 * 还画在屏上，得跟着 slide_y 一起滑走。只问可见性会把它跳过，于是它
		 * 停在原地，直到过渡结束、工作区藏起来才消失 —— 用户报的「浮动窗口
		 * 不跟随工作区移动」。 */
		bool sliding_out = output->slide_from != NULL &&
			view->ws == output->slide_from;
		/* 藏起来的工作区（或者正常屏上的特殊工作区）里的浮窗不摆：它的场景
		 * 节点是关着的，摆了也看不见，还会把「这块屏摆过什么」的账算乱。 */
		if (!sliding_out && !vidow_workspace_view_visible(server, view)) {
			continue;
		}
		int slide_y = 0;
		if (sliding_out) {
			slide_y = (int)lround(output->slide_from->slide_y);
		} else if (shown != NULL && view->ws == shown) {
			slide_y = (int)lround(shown->slide_y);
		}
		float_place_recursive(view, 0, slide_y);
	}
}

/* 把单个弹出物摆到客户端说的位置上。幂等，可以随时重调（提交、map、重排）。
 * 顺带说清为什么它不在这里做别的：不居中、不夹工作区、不做移动补间 —— 这三点
 * 正是「菜单被窗口布局的规矩搬走」的来源。 */
void vidow_popup_place(struct vidow_view *view) {
	/* scene_tree 为 NULL = 表面还没来（XWayland 自己那个 -100,-100 的 1x1
	 * WM 窗口就是先建视图后建表面的），这时候没东西可摆。 */
	if (!view->popup || view->scene_tree == NULL) {
		return;
	}
	/* 弹出物跟父窗口走：父窗口所在的工作区藏着的时候，这个菜单也不该冒出来。 */
	if (view->parent != NULL && !vidow_workspace_view_visible(view->server,
			view->parent)) {
		return;
	}
	struct wlr_box box = {0};
	if (vidow_view_x11_popup_box(view, &box)) {
		vidow_place_view(view, box.x, box.y, box.width, box.height);
	}
}

void vidow_popup_place_all(struct vidow_server *server) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		vidow_popup_place(view);
	}
}

void vidow_float_detach_children(struct vidow_server *server,
		struct vidow_view *dying) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->parent != dying) {
			continue;
		}
		/* Leave the dialog where it is, but stop tracking the dead parent. */
		view->parent = NULL;
		view->float_mode = VIDOW_FLOAT_FIXED;
		view->float_geom.x = view->geom.x;
		view->float_geom.y = view->geom.y;
	}
}

/* --------------------------------------------------------------- grabs */

void vidow_float_begin_grab(struct vidow_server *server,
		struct vidow_view *view, bool resize, bool from_client) {
	server->grab_mode = resize ? VIDOW_GRAB_RESIZE : VIDOW_GRAB_MOVE;
	server->grab_view = view;
	/* **拖动也要置 view->dragging**（改大小不置：尺寸一直在变，裁不裁无所谓，
	 * 而它本来也不会把窗口拖出原屏）。
	 *
	 * view_clip_box() 里那道「正被拖拽的窗口不裁」的放行只认 view->dragging，
	 * 而这个字段以前只有平铺拖拽（drag.c 的 vidow_tile_grab_begin）会置位 ——
	 * 浮窗拖拽走的是这里，于是浮窗一拖出原屏就被整块裁掉，屏幕上看着像
	 * 「窗口拖不走」（用户报的「X11 应用无法跨屏拖拽，松手却过去了」）。
	 * 松手时 drag.c 那边统一清掉这个标志。 */
	if (!resize) {
		view->dragging = true;
	}
	/* 客户端自己发起的那次（见 server.h 里 grab_client_press 的说明）：按下
	 * 已经转给它了，松手也要转给它，状态才对称。 */
	server->grab_client_press = from_client;
	/* 拖动有果冻，改大小没有：改大小每帧换源缓冲尺寸，网格对不上（见 jelly.c）。
	 *
	 * 小窗（mini）**不做果冻**：jelly 的落点换算（jelly.c 的
	 * `dst = buf / fx->scale`）假设 fx->scale 就是「设备像素 / 逻辑像素」，
	 * 而小窗那条路上烘图倍率里**已经乘过 contain 系数**（view.c 的
	 * view_anim_bake 传 `view_scale(view) * fx_ov`，fx_ov 含小窗系数），
	 * 于是那一份被除第二遍：内容被放大 1/ms 倍（实测 FREE 盒 504x315、
	 * contain 0.43 → 拖起来内容的屏幕脚印从 272x304 变成 641x507 ≈ 原生大小，
	 * 正是用户报的「一拖就变回原始大小」）。要修得动 jelly.c 的换算（不在
	 * 本次范围），先按「缩放态不做果冻」处理：拖拽的手感和尺寸都不会错。 */
	if (!resize && vidow_view_mini_scale_export(view) == 1.0) {
		vidow_jelly_begin(view);
	}
	server->grab_x = server->cursor->x;
	server->grab_y = server->cursor->y;
	server->grab_geom = view->float_geom;

	wlr_log(WLR_INFO, "grab %s: %s",
		resize ? "resize" : "move",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
		resize ? "se-resize" : "grabbing");
}

/* ---------------------------------------- FREE 小窗拖动中的跨屏 / 锚点（S8）
 *
 * FREE 是「自由摆放」那一档，拖拽语义和普通浮窗一样：跟着光标、能跨屏、落在
 * 光标处。它不能直接改 float_geom —— 小窗的位置是**比例锚点**，唯一的换算点是
 * vidow_mini_resolve_geom()（见 float_place_recursive 的小窗分支）。所以拖动里
 * 每次都得把「光标想要的绝对位置」换算成锚点回写，换算用的工作区必须是**盒子
 * 现在归属的那块屏**（跨屏的瞬间就换过去，见 mini_free_drag_to_output）。
 *
 * 锚点 0 = 贴工作区左 / 上边、1 = 贴右 / 下边。
 *
 * allow_over 决定「能探出工作区多少」，**按形态给**：
 *   FREE  → true ：出屏**半个盒子**（用户要的「允许一半在屏外」）
 *   MINI  → false：不出工作区（拖到屏边就停）—— 这一档是这次修的
 *   ICON  → true ：照旧允许越界一点（图标出屏那点，见 mini_icon_overflow）
 * 以前 MINI / ICON 那两条路**完全不夹**，只靠 resolve 的兜底 -0.5..1.5 —— 那是
 * 「半个**工作区**」（1908 屏上 ≈954px），而迷你盒才 264px 宽：能把盒子整个拖出
 * 屏幕（用户视频 /tmp/mini.mp4：拖出去两秒完全看不见，回来时贴右边缘只剩 200px）。 */
void vidow_mini_anchor_from(struct vidow_view *view,
		struct wlr_box work, int w, int h, double bx, double by,
		bool allow_over) {
	double span_x = (double)(work.width - w);
	double span_y = (double)(work.height - h);
	double ax = span_x > 1.0 ? (bx - (double)work.x) / span_x : 0.0;
	double ay = span_y > 1.0 ? (by - (double)work.y) / span_y : 0.0;
	/* 允许**出屏半个盒子**（用户：「能拖出屏幕边缘，允许一半在屏外」）：锚点
	 * 0 = 贴工作区左/上边、1 = 贴右/下边，出屏半个盒子对应的锚点偏移是
	 * w / (2 * span)。以前夹在 0..1 ⇒ 盒子永远进不了屏幕左右那两片区域
	 * （左边缘最多停在 work.x + span，离屏边还有一个盒子宽 —— 用户报的
	 * 「左右屏幕区域把小窗卡住不让过去」就是这条）。
	 *
	 * 上限还要跟 vidow_mini_resolve_geom 那边对齐（它夹 -0.5..1.5）：盒子接近
	 * 满屏时 span 很小，w/(2*span) 会远超 0.5 —— 存一个 resolve 摆不出来的
	 * 锚点，拖动就会「盒子先停、锚点还在涨」，往回拖要先空转掉那一段。 */
	double over_x = allow_over && span_x > 1.0 ?
		(double)w / (2.0 * span_x) : 0.0;
	double over_y = allow_over && span_y > 1.0 ?
		(double)h / (2.0 * span_y) : 0.0;
	if (over_x > 0.5) {
		over_x = 0.5;
	}
	if (over_y > 0.5) {
		over_y = 0.5;
	}
	if (ax < -over_x) {
		ax = -over_x;
	} else if (ax > 1.0 + over_x) {
		ax = 1.0 + over_x;
	}
	if (ay < -over_y) {
		ay = -over_y;
	} else if (ay > 1.0 + over_y) {
		ay = 1.0 + over_y;
	}
	view->mini_ax = ax;
	view->mini_ay = ay;
}

/* 同一块屏上的拖动：把「盒子该在的绝对位置」换算成锚点回写，再按锚点把盒子算
 * 出来（这样盒子就真的跟着光标走了 —— 以前只写 float_geom.x/y，下一次 arrange
 * 被 resolve_geom 按旧锚点抹回去，整段拖拽盒子一动不动）。 */
static void mini_free_drag_set_anchor(struct vidow_server *server,
		struct vidow_view *view, int x, int y) {
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width < 1 || work.height < 1) {
		return;
	}
	vidow_mini_anchor_from(view, work, view->float_geom.width,
		view->float_geom.height, (double)x, (double)y, true);
	vidow_mini_resolve_geom(view);
}

/* 拖动中光标跨过屏边界：**当场**把归属换到光标底下那块屏，并按「抓取偏移不变」
 * 把锚点算在新屏的工作区里（从右边进来 → 贴新屏左边缘，不是远边）。松手那条路
 * （vidow_mini_drag_finish）也走 vidow_mini_move_to_output，但那要等到松手 ——
 * 拖动中不换的话盒子会被旧屏工作区夹住、跟不上光标。 */
void vidow_mini_drag_to_output(struct vidow_server *server,
		struct vidow_view *view, struct wlr_output *to, double lx, double ly) {
	struct wlr_box work = vidow_work_area(server, to);
	if (work.width < 1 || work.height < 1) {
		return;
	}
	/* 盒子在新屏上的尺寸：和 vidow_mini_resolve_geom 同一套换算（比例锚点 ×
	 * 新屏工作区，夹到工作区里），这里先算出来只是为了拿到 span。 */
	int w = (int)lround(view->mini_wf * (double)work.width);
	int h = (int)lround(view->mini_hf * (double)work.height);
	if (w < FLOAT_MIN_SIZE) {
		w = FLOAT_MIN_SIZE;
	}
	if (h < FLOAT_MIN_SIZE) {
		h = FLOAT_MIN_SIZE;
	}
	if (w > work.width) {
		w = work.width;
	}
	if (h > work.height) {
		h = work.height;
	}
	/* 抓取偏移 = 按下那一刻光标在盒子里的位置：整个拖拽里保持不变，手感才是
	 * 「拎着它走」而不是「盒子跳到光标底下」。 */
	double bx = lx - (server->grab_x - (double)server->grab_geom.x);
	double by = ly - (server->grab_y - (double)server->grab_geom.y);
	/* allow_over = false：换屏那一刻**贴新屏边缘**（「从右边进新屏就贴新屏的
	 * 左边缘」）—— 这正是「能拖到另一块屏幕」的手感，别放开成半挂屏外。 */
	vidow_mini_anchor_from(view, work, w, h, bx, by, false);
	vidow_mini_move_to_output(server, view, to);
}

void vidow_float_grab_update(struct vidow_server *server, double lx,
		double ly) {
	struct vidow_view *view = server->grab_view;
	if (view == NULL || server->grab_mode == VIDOW_GRAB_NONE) {
		return;
	}

	int dx = (int)(lx - server->grab_x);
	int dy = (int)(ly - server->grab_y);

	if (server->grab_mode == VIDOW_GRAB_MOVE) {
		int x = server->grab_geom.x + dx;
		int y = server->grab_geom.y + dy;

		/* 夹在「所有输出的包围盒」里，而不是当前这块屏的工作区里 —— 夹工作区
		 * 的话窗口永远拖不到另一块屏上（跨屏拖拽就是要能拖出去）。完全拖出屏
		 * 之外还是拦住：看不见的窗口找不回来。 */
		struct wlr_box all = {0};
		wlr_output_layout_get_box(server->output_layout, NULL, &all);
		if (all.width > 0 && all.height > 0) {
			/* 夹取用**盒子当前的尺寸**，不是按下那一刻那份（grab_geom）：拖动中
			 * 跨屏会当场换归属，盒子尺寸跟着新屏重算（FREE 是比例锚点，见下面
			 * mini 那支），拿旧尺寸夹就少走「旧 - 新」那一段 —— 实测从 1260x700
			 * 的屏（盒 504x315）拖到 620x340 的屏（盒 248x153）之后，盒子右边缘
			 * 停在 1416 = 1920 - 504，明明还差 248 才到布局右边界。位置本身照旧
			 * 按 grab_geom 推（抓取偏移不变才是「拎着走」）。 */
			int cw = view->float_geom.width > 0 ?
				view->float_geom.width : server->grab_geom.width;
			int ch = view->float_geom.height > 0 ?
				view->float_geom.height : server->grab_geom.height;
			/* 小窗（mini）再放开**半个盒子**：用户要的是「能拖出屏幕边缘、
			 * 一半在屏外」。普通浮窗照旧整个夹在布局里（看不见的窗口找不回
			 * 来）。两条路的界限一样：盒子至少还有一半在屏上。 */
			int out_x = view->mini == VIDOW_MINI_FREE ? cw / 2 : 0;
			int out_y = view->mini == VIDOW_MINI_FREE ? ch / 2 : 0;
			if (x + cw > all.x + all.width + out_x) {
				x = all.x + all.width + out_x - cw;
			}
			if (y + ch > all.y + all.height + out_y) {
				y = all.y + all.height + out_y - ch;
			}
			if (x < all.x - out_x) {
				x = all.x - out_x;
			}
			if (y < all.y - out_y) {
				y = all.y - out_y;
			}
		}

		view->float_geom.x = x;
		view->float_geom.y = y;
		/* 小窗（mini）：位置语义是**比例锚点**（唯一的换算点是
		 * vidow_mini_resolve_geom），只写 float_geom.x/y 没有用 —— 下一次
		 * arrange 走到 float_place_recursive() 的小窗分支会按锚点把位置算回来。
		 * 实测就是用户报的那条：「按住 Mod+左键拖自由小窗，拖不动」（整段拖拽
		 * 里盒子的 x/y 逐字段不变）。和 input.c 的 mini_grab_update()（拖迷你 /
		 * 图标）以及下面 resize 分支回写 mini_wf/hf 是同一规矩：**拖出来的东西
		 * 必须回写锚点**。放在夹取之后：夹出来的位置才是真正要落地的那个。 */
		if (view->mini == VIDOW_MINI_FREE) {
			/* FREE 态与普通浮窗**同权**（自由摆放）：拖动中跟着光标走、能跨屏。
			 *
			 * 以前这一支用**旧屏**的工作区算 span 把锚点写回去（还不夹 0..1），
			 * 紧接着 resolve_geom 又按旧屏工作区把盒子夹回来 —— 盒子一出旧屏
			 * 边界就卡住（实测光标在邻屏 1500 时盒子还卡在旧屏 x = 758）；
			 * 改归属又只发生在**松手**那一刻（那时锚点已被旧屏算成 1.638），
			 * 新屏上按这个锚点摆就被夹到**远边**，正是用户报的「跨屏弹到同向边」。
			 *
			 * 现在的规矩：光标在哪块屏上，归属**当场**换过去（不等松手）；锚点
			 * 按「光标在新屏工作区里的相对位置」算，而且抓取偏移（光标按下去
			 * 那一点在盒子里的位置）保持不变 —— 于是盒子始终跟着光标，从右边
			 * 进新屏就贴新屏的**左**边缘。 */
			struct wlr_output *under = wlr_output_layout_output_at(
				server->output_layout, lx, ly);
			if (under != NULL && under != view->output) {
				vidow_mini_drag_to_output(server, view, under, lx, ly);
			} else {
				mini_free_drag_set_anchor(server, view, x, y);
			}
			x = view->float_geom.x;
			y = view->float_geom.y;
		} else if (view->mini != VIDOW_MINI_NONE) {
			/* 迷你 / 图标：和自由态一样 —— 允许越界（allow_over = true），而且
			 * **光标在哪块屏上，归属当场换过去**（用户：「没有死区了，但无法拖拽
			 * 到另一个屏幕」）。这里以前只按**当前**屏的工作区写锚点，`view->output`
			 * 一直不变，所以迷你 / 图标态永远换不了屏 —— 换屏那支以前只在 FREE
			 * 分支里。 */
			struct wlr_output *under = wlr_output_layout_output_at(
				server->output_layout, lx, ly);
			if (under != NULL && under != view->output) {
				vidow_mini_drag_to_output(server, view, under, lx, ly);
			} else {
				struct wlr_box work = vidow_work_area(server,
					view->output);
				vidow_mini_anchor_from(view, work,
					view->float_geom.width,
					view->float_geom.height, (double)x,
					(double)y, true);
			}
			x = view->float_geom.x;
			y = view->float_geom.y;
			vidow_mini_resolve_geom(view);
			x = view->float_geom.x;
			y = view->float_geom.y;
		}
		/* 位置是「相对自己那块屏」记的（见本文件顶上那段注释）：只改绝对盒、
		 * 不刷新 home_dx/home_dy，下一次 arrange 的 float_place_recursive()
		 * 会按旧偏移把它摆回去 —— 拖了等于没拖。 */
		struct wlr_box box = {0};
		if (view->output != NULL) {
			wlr_output_layout_get_box(server->output_layout, view->output,
				&box);
		}
		if (box.width > 0) {
			view->home_dx = x - box.x;
			view->home_dy = y - box.y;
		}
	} else {
		int w = server->grab_geom.width + dx;
		int h = server->grab_geom.height + dy;
		if (w < FLOAT_MIN_SIZE) {
			w = FLOAT_MIN_SIZE;
		}
		if (h < FLOAT_MIN_SIZE) {
			h = FLOAT_MIN_SIZE;
		}
		/* 小窗（S3b）：锁比例时**宽度驱动**（h = w / ratio）。理由：vidow 的
		 * 平铺改宽本来就是横向驱动，手感一致；宽度驱动还保证「拖右边」和
		 * 「拖右下角」得到同一个结果，不会因为光标 y 抖一下就跳。 */
		double ratio = 0.0;
		if (vidow_mini_aspect_lock(view, &ratio) && ratio > 0.0) {
			h = (int)lround((double)w / ratio);
			if (h < FLOAT_MIN_SIZE) {
				h = FLOAT_MIN_SIZE;
			}
		}
		view->float_geom.width = w;
		view->float_geom.height = h;
		/* 小窗（S3b）：FREE 的盒子是**比例锚点**算出来的 —— 每次 arrange 都会
		 * 走 vidow_mini_resolve_geom()，按 mini_wf/mini_hf 重算 float_geom。
		 * 拖出来的尺寸不回写锚点，下一次 arrange 就把它抹掉（拖了等于没拖，
		 * 实测就是这样）。和 input.c 里拖小窗位置时回写 mini_ax/ay 同一条规矩。 */
		if (view->mini == VIDOW_MINI_FREE) {
			struct wlr_box work = vidow_work_area(server, view->output);
			if (work.width > 0 && work.height > 0) {
				view->mini_wf = (double)w / (double)work.width;
				view->mini_hf = (double)h / (double)work.height;
				view->mini_size_known = true;
				/* 锚点跟着**新尺寸**重算一次，钉住盒子的左上角：锚点是「盒子
				 * 在工作区里的比例位置」，尺寸一变同一个锚点就落到别处 ——
				 * 拖右边缘时左边缘会跟着往回跑（实测 1648 → 1601）。普通浮窗
				 * 改大小的语义就是左上角不动，FREE 照它。 */
				vidow_mini_anchor_from(view, work, w, h,
					(double)view->float_geom.x,
					(double)view->float_geom.y, true);
			}
			/* FREE 走普通浮窗那条路：盒子多大就**命令客户端**多大多（它跟着
			 * 重新排版），不是把原生 buffer 缩进一个更大的盒子里留白 —— 后者
			 * 拖出来的只是留白，内容比例一动不动（用户报的「改大小被限制了
			 * 比例」）。float_want_w/h 一起钉住：客户端为别的原因提交的旧尺寸
			 * 不能把盒子改回去（见 vidow_float_on_commit 的小窗分支）。 */
			view->float_want_w = w;
			view->float_want_h = h;
			vidow_view_set_size(view, w, h);
		}
	}

	/* 重排推迟到这一帧要出帧之前（见 output.c 里那段）：指针比刷新快的时候
	 * 一帧会来好几个移动事件，中间那些位置屏幕上从来没出现过 —— 以前每个事件
	 * 都整排一次（所有窗口重新 place，每个都要重烘装饰图），实测每个事件 70µs
	 * （1000Hz 鼠标 = 每帧 0.44ms，8000Hz = 每帧 3.5ms，全是白算的）。 */
	server->arrange_dirty = true;
	server->grab_ev_ns = vidow_now_ns(); /* 探针：量「事件 -> 出帧」延迟 */
	/* **两块屏都得催**：窗口跟着光标越过屏边界之后画在对面那块屏上，它的归属
	 * （view->output）要到松手才换 —— 只给旧屏排帧，对面那块屏自己不动，屏幕上
	 * 就是「拖过去看不见窗口」（用户 2026-10-23 报的）。见 drag.c 的
	 * vidow_drag_schedule_outputs。 */
	vidow_drag_schedule_outputs(server, view, "拖浮动窗口");
	/* 同上：浮动拖拽的落位是推迟到帧里做的，帧一停它一点都不动，所以这里
	 * 必须保证「就算后端不给帧事件，帧也照出」（见 output.c 的 anim_watchdog）。 */
	vidow_output_kick_watchdog(server);
	/* 小窗（S4）落点区：拖动中随时算（只在光标落进去的时候才亮）。 */
	vidow_mini_drop_zone_update(server);
}

void vidow_float_end_grab(struct vidow_server *server) {
	if (server->grab_mode == VIDOW_GRAB_NONE) {
		return;
	}
	/* 松手前把还欠着的那一次重排补上：最后一个位置得先摆出来，后面的收尾
	 * （跨屏搬家之类）才是从它接着算的。 */
	if (server->arrange_dirty) {
		server->arrange_dirty = false;
		vidow_arrange(server);
	}
	struct vidow_view *view = server->grab_view;
	server->grab_mode = VIDOW_GRAB_NONE;
	server->grab_view = NULL;
	server->grab_client_press = false;
	/* 松手了：拉着的不再是「拖拽中」—— clip 豁免（view_clip_box 里那道
	 * `if (view->dragging) return false;`）要在这个窗口落到新归属屏之前就
	 * 撤销，否则松手瞬间它还按旧归属屏裁一刀。放在下面那一串提前 return
	 * 之前，各条路径都能走到。 */
	if (view != NULL) {
		view->dragging = false;
	}
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
	if (view != NULL) {
		vidow_jelly_release(view);
	}
	/* 小窗（S4）落点区：松手时光标在不在里面，先记下来（下面那些分支都会提前
	 * return，收尾只能在这儿做一次）。 */
	bool drop_zone = vidow_mini_drop_zone_hit(server, server->cursor->x,
		server->cursor->y);
	vidow_mini_drop_zone_hide(server);

	if (view == NULL || view->popup ||
			view->float_mode == VIDOW_FLOAT_NONE) {
		return;
	}

	/* 小窗（S4）主入口：把**自由小窗 / 浮窗**拖进落点区松手 → 迷你小窗（用户
	 * 要的「拖窗口时在每块屏右上角亮出落点区，松手 → 迷你小窗」在浮窗这条路
	 * 也一样）。 */
	if (drop_zone) {
		wlr_log(WLR_INFO, "mini: 拖拽落进落点区 → %s 变迷你小窗",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "?");
		vidow_mini_set_state(view, VIDOW_MINI_MINI);
		return;
	}

	/* 小窗（S5）甩出：自由小窗（也是小窗三态里唯一走浮窗拖拽那条路的）被
	 * **甩**出去 → 按速度方向收成图标 / 换屏。放在落点判定之前：甩出去的那
	 * 一下速度就是意图，不该再拿「光标现在停在哪」去判。 */
	if (view->mini != VIDOW_MINI_NONE) {
		double vx = 0.0, vy = 0.0;
		if (vidow_mini_fling_velocity(server, &vx, &vy) &&
				vidow_mini_fling(server, view, vx, vy)) {
			return;
		}
	}

	/* 小窗（S4）：迷你 / 图标的形态拖拽（input.c 的 mini_grab_*）和浮窗拖拽
	 * 共用同一套落点判据 —— 内边缘收图标、跨屏边换屏保持形态。普通浮窗
	 * （mini == NONE）走这儿什么都没发生。 */
	if (view->mini != VIDOW_MINI_NONE &&
			vidow_mini_drag_finish(server, view)) {
		return;
	}

	/* 松手时光标落在**另一块屏**上：这块浮窗改归那块屏显示着的工作区（跨屏
	 * 拖拽）。位置保持绝对坐标不变，改完按新屏原点刷新相对偏移 —— 不刷新的话
	 * 下一次 arrange 会把它按旧偏移摆到新屏的别处去。 */
	struct wlr_output *under = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (under == NULL || under == view->output) {
		return;
	}
	/* **搬去目标屏上「同一个序号」的工作区，不是它此刻亮着的那块**。
	 *
	 * 以前取的是 vidow_workspace_shown_for()（目标屏当前显示的工作区）：人在第 3
	 * 块工作区上把窗口拖到隔壁屏，窗口就被塞进隔壁屏的第 1 块 —— 用户报的
	 * 「跨屏之后原来屏幕的窗口跳转到第一个，应该啥都不要变才对」。工作区本身
	 * 是有稳定编号的实体（见 workspace.h 的 ws->id），跨屏只是换挂在哪块屏上，
	 * 序号不该跟着变。
	 *
	 * 目标屏上没有那么靠后的工作区时退回「它亮着的那块」：宁可落在能看见的
	 * 地方，也不要凭空替人建一块新工作区。 */
	int idx = vidow_workspace_index(view->ws);
	struct vidow_workspace *target =
		idx > 0 ? vidow_workspace_at(server, under, idx) : NULL;
	if (target == NULL) {
		target = vidow_workspace_shown_for(server, under);
	}
	if (target == NULL) {
		return;
	}
	/* 先按新屏刷新相对偏移，**再**搬 —— 搬那一步会 arrange，而摆放是按
	 * `box + home_dx` 算的：用旧偏移配新屏的 box，窗口会整块平移一块屏的宽度
	 * （实测拖过去落在 x=2490，本来该在 1210）。 */
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, under, &box);
	if (box.width > 0) {
		view->home_dx = view->float_geom.x - box.x;
		view->home_dy = view->float_geom.y - box.y;
	}
	if (!vidow_workspace_move_view(server, view, target)) {
		return;
	}
	wlr_log(WLR_INFO, "grab: 浮窗拖到 %s 上 → 归到那块屏第 %d 块工作区",
		under->name, vidow_workspace_index(target));
	vidow_arrange(server);
}

/* ================================= 小窗（mini window，S4）：边缘判定 / 工具条动作
 *
 * 三件事，全部只在 mini 态生效：
 *   1) 「这条边的外侧有没有相邻输出」—— 拖迷你小窗到边上的判据（有 = 跨屏边 =
 *      换屏保持形态；没有 = 内边缘 = 收成图标）。用户写死的：判据只有这一条。
 *   2) 跨屏边松手：换屏、保持形态。
 *   3) 工具条按钮的动作（放大 / 收成图标 / 还原成列 / 关闭）。
 */

/* 这条边的外侧有没有相邻输出。探针沿边的法线取 1px（越过边界一点点），并要求
 * 与目标屏有**非零区间重叠** —— 只看「角上相接」的两块屏不算相邻边（对角摆的
 * 显示器：右上角那一点探得到，但那条边外侧其实没有屏）。纯几何，不看后端。 */
struct wlr_output *vidow_mini_output_across(struct vidow_server *server,
		struct wlr_output *self, enum vidow_mini_edge edge) {
	if (server == NULL || self == NULL || edge == VIDOW_MINI_EDGE_NONE) {
		return NULL;
	}
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, self, &box);
	if (box.width < 1 || box.height < 1) {
		return NULL;
	}
	int px = 0, py = 0;
	switch (edge) {
	case VIDOW_MINI_EDGE_LEFT:
		px = box.x - 1;
		py = box.y + box.height / 2;
		break;
	case VIDOW_MINI_EDGE_RIGHT:
		px = box.x + box.width;
		py = box.y + box.height / 2;
		break;
	case VIDOW_MINI_EDGE_TOP:
		px = box.x + box.width / 2;
		py = box.y - 1;
		break;
	case VIDOW_MINI_EDGE_BOTTOM:
		px = box.x + box.width / 2;
		py = box.y + box.height;
		break;
	case VIDOW_MINI_EDGE_NONE:
		return NULL;
	}
	struct wlr_output *other =
		wlr_output_layout_output_at(server->output_layout, px, py);
	if (other == NULL || other == self) {
		return NULL;
	}
	struct wlr_box obox = {0};
	wlr_output_layout_get_box(server->output_layout, other, &obox);
	if (obox.width < 1 || obox.height < 1) {
		return NULL;
	}
	if (edge == VIDOW_MINI_EDGE_LEFT || edge == VIDOW_MINI_EDGE_RIGHT) {
		/* 左右相邻：两条边在**纵向**上得有非零重叠。 */
		if (obox.y >= box.y + box.height ||
				obox.y + obox.height <= box.y) {
			return NULL;
		}
	} else {
		/* 上下相邻：两条边在**横向**上得有非零重叠。 */
		if (obox.x >= box.x + box.width ||
				obox.x + obox.width <= box.x) {
			return NULL;
		}
	}
	return other;
}

/* 光标贴着自己那块屏工作区的哪条边（mini_edge_snap_px 以内），没贴边 = NONE。
 * 只认自己那块屏的四条边 —— 跨屏边也在四条里，收不收图标由调用方按
 * vidow_mini_output_across() 判。 */
/* 离这个位置最近的那条工作区边（**always** 有答案，除非工作区退化）。
 * 图标态拖拽用它吸附：图标永远贴最近那条边（用户：「icon态需要吸附在屏幕边缘」）
 * —— vidow_mini_edge_under_cursor() 那个是「光标在 mini_edge_snap_px 以内才算贴
 * 边」，给收图标用；这里要的是「无论拖到哪儿都挑一条边」。按**盒子中心**量，不按
 * 光标：光标在盒子里的抓点位置是任意的，按光标会让吸附跟着抓点跳。 */
enum vidow_mini_edge vidow_mini_nearest_edge(struct vidow_server *server,
		struct vidow_view *view, int w, int h, double bx, double by) {
	if (server == NULL || view == NULL) {
		return VIDOW_MINI_EDGE_NONE;
	}
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width < 1 || work.height < 1) {
		return VIDOW_MINI_EDGE_NONE;
	}
	double cx = bx + (double)w / 2.0;
	double cy = by + (double)h / 2.0;
	double d[4] = {
		fabs(cx - (double)work.x),
		fabs((double)(work.x + work.width) - cx),
		fabs(cy - (double)work.y),
		fabs((double)(work.y + work.height) - cy),
	};
	enum vidow_mini_edge edges[4] = {
		VIDOW_MINI_EDGE_LEFT, VIDOW_MINI_EDGE_RIGHT,
		VIDOW_MINI_EDGE_TOP, VIDOW_MINI_EDGE_BOTTOM,
	};
	int best = 0;
	for (int i = 1; i < 4; i++) {
		if (d[i] < d[best]) {
			best = i;
		}
	}
	return edges[best];
}

enum vidow_mini_edge vidow_mini_edge_under_cursor(struct vidow_server *server,
		struct vidow_view *view) {
	if (server == NULL || view == NULL) {
		return VIDOW_MINI_EDGE_NONE;
	}
	int snap = server->config.mini_edge_snap_px;
	if (snap < 0) {
		snap = 0;
	}
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width < 1 || work.height < 1) {
		return VIDOW_MINI_EDGE_NONE;
	}
	double cx = server->cursor->x, cy = server->cursor->y;
	double d[4] = {
		fabs(cx - (double)work.x),
		fabs((double)(work.x + work.width) - cx),
		fabs(cy - (double)work.y),
		fabs((double)(work.y + work.height) - cy),
	};
	enum vidow_mini_edge edges[4] = {
		VIDOW_MINI_EDGE_LEFT, VIDOW_MINI_EDGE_RIGHT,
		VIDOW_MINI_EDGE_TOP, VIDOW_MINI_EDGE_BOTTOM,
	};
	/* 光标得真的在这块工作区的范围里（贴着某条边）：横向的边看 y，纵向的边
	 * 看 x —— 不然「光标在隔壁屏、离这条边很近」也会命中。 */
	double margin = (double)snap + 1.0;
	int best = -1;
	for (int i = 0; i < 4; i++) {
		bool horizontal = (i >= 2); /* TOP / BOTTOM：边是横的 */
		if (horizontal) {
			if (cx < (double)work.x - margin ||
					cx > (double)(work.x + work.width) + margin) {
				continue;
			}
		} else {
			if (cy < (double)work.y - margin ||
					cy > (double)(work.y + work.height) + margin) {
				continue;
			}
		}
		if (d[i] > (double)snap) {
			continue;
		}
		if (best < 0 || d[i] < d[best]) {
			best = i;
		}
	}
	return best < 0 ? VIDOW_MINI_EDGE_NONE : edges[best];
}

/* 小窗（S4）：一次形态拖拽（迷你 / 自由小窗被拖动）松手时的落点判定。
 * 三条判据，顺序写死：
 *   1) 光标已经在**另一块屏**上 → 跨屏边松手 = 换屏、保持形态（不收图标）；
 *   2) 光标贴着自己那块屏的一条边，且**那条边外侧没有相邻输出**（内边缘）→
 *      收成图标，贴那条边；
 *   3) 其余（跨屏边但光标还在自己屏上 / 没贴边）→ 什么都不做。
 * 返回 true = 形态或归属变了（调用方别再走自己的收尾）。
 *
 * 「这条边的外侧有没有相邻输出」是唯一的判据（用户写死的），见
 * vidow_mini_output_across()：探针沿法线取 1px，还要求与目标屏有非零区间重叠。 */
bool vidow_mini_drag_finish(struct vidow_server *server,
		struct vidow_view *view) {
	if (server == NULL || view == NULL || view->mini == VIDOW_MINI_NONE) {
		return false;
	}
	struct wlr_output *under = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (under != NULL && under != view->output) {
		vidow_mini_move_to_output(server, view, under);
		return true;
	}
	/* FREE 是**自由摆放**那一档：拖到工作区边缘松手就是「摆在那儿」，不收成
	 * 图标。判据三条：
	 *   ① 语义不同 —— MINI / ICON 是贴边的缩略图（尺寸钉死、不吃输入），
	 *      「拖到内边缘 = 收成图标」是它们的形态语言；FREE 是正常窗户（客户端
	 *      拿得到焦点 / 键盘 / 滚轮，尺寸由客户端自己说了算），摆到哪就是哪。
	 *   ② 会打架 —— FREE 已经能**改大小**（resize 会真的重配客户端），而收成
	 *      图标会把这个窗口的客户端尺寸 / 比例一起丢掉，用户改大的窗口拖到边上
	 *      就白改了。
	 *   ③ 有别的路 —— 收成图标有专门的入口：拖进落点区（正上方那条路）、
	 *      工具条按钮、滚轮降档、甩出（甩出那条不改，见 vidow_mini_fling）。
	 * MINI / ICON 那条一字不动。 */
	if (view->mini == VIDOW_MINI_FREE) {
		return false;
	}
	enum vidow_mini_edge edge = vidow_mini_edge_under_cursor(server, view);
	if (edge == VIDOW_MINI_EDGE_NONE) {
		return false;
	}
	if (vidow_mini_output_across(server, view->output, edge) != NULL) {
		/* 跨屏边：不收图标（换屏那条已经由上面 1) 管了 —— 光标还在自己
		 * 屏上就是「拖到边上但没过去」，什么都不做）。 */
		return false;
	}
	/* 内边缘：沿这条边的那一轴按光标位置记锚点（另一轴钉死在边上），图标就贴在
	 * 「拖到哪儿」的那一段上。 */
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width > 0 && work.height > 0) {
		int iw = server->config.mini_icon_size;
		double span_x = (double)(work.width - iw);
		double span_y = (double)(work.height - iw);
		if (edge == VIDOW_MINI_EDGE_LEFT || edge == VIDOW_MINI_EDGE_RIGHT) {
			view->mini_ax = edge == VIDOW_MINI_EDGE_LEFT ? 0.0 : 1.0;
			view->mini_ay = span_y > 1.0 ?
				(server->cursor->y - (double)work.y) / span_y : 0.0;
		} else {
			view->mini_ay = edge == VIDOW_MINI_EDGE_TOP ? 0.0 : 1.0;
			view->mini_ax = span_x > 1.0 ?
				(server->cursor->x - (double)work.x) / span_x : 0.0;
		}
	}
	view->mini_edge = edge;
	wlr_log(WLR_INFO, "mini: %s 拖到内边缘（%s，外侧没有相邻输出）→ 收成图标",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		edge == VIDOW_MINI_EDGE_LEFT ? "左" : edge == VIDOW_MINI_EDGE_RIGHT ?
			"右" : edge == VIDOW_MINI_EDGE_TOP ? "上" : "下");
	vidow_mini_set_state(view, VIDOW_MINI_ICON);
	return true;
}
