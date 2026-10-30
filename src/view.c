#include "server.h"
#include "view-internal.h"
#include "flipprobe.h"
#include "ipc.h"
#include "x11surface.h"
#include "xtrace.h"
#include "xwm.h"

#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <GLES2/gl2.h>

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/util/log.h>
#include <xcb/xcb.h>

/* gap 的算法都在 server.h 里（vidow_gap_side / vidow_gap_inner）：统一值 gaps
 * 管全部，单边设了就用单边的，-1 = 跟随。 */

/* 新窗口该落到哪块屏。
 *
 * `server->focused_output` 在「还没人动过鼠标」时是 NULL —— sddm 直接起 vidow 就是
 * 这种情况（用户报的：自启动的应用全是空框）。那时新窗口拿到 output = NULL，
 * 永远不进 arrange、永远不发 configure、永远不交 buffer，还会作为死窗口留在场景里
 * 拖慢每一帧。回落：用第一块输出。
 */
static struct wlr_output *view_default_output(struct vidow_server *server) {
	/* ① 鼠标在哪块屏，新窗口就落哪块屏（用户要的：跟随鼠标）。
	 * 这是唯一「用户现在正看着哪块屏」的真实信号 —— 焦点窗口可能在另一块屏上，
	 * 但用户的眼睛跟着鼠标。 */
	if (server->cursor != NULL) {
		struct wlr_output *under = wlr_output_layout_output_at(
			server->output_layout, server->cursor->x, server->cursor->y);
		if (under != NULL) {
			return under;
		}
	}
	/* ② 鼠标不在任何输出上（刚启动 / 布局外）：用焦点屏。 */
	if (server->focused_output != NULL) {
		return server->focused_output;
	}
	/* ③ 连焦点都没有（sddm 直接起 vidow，还没人动过鼠标）：主屏 → 第一块屏。 */
	if (server->config.primary_output != NULL) {
		struct vidow_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			if (o->wlr_output->name[0] != '\0' &&
					strcmp(o->wlr_output->name,
						server->config.primary_output) == 0) {
				return o->wlr_output;
			}
		}
	}
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		return o->wlr_output;
	}
	return NULL;
}

void vidow_place_view(struct vidow_view *view, int x, int y, int w, int h);
/* 小窗（S5）：图标态的快照节点（定义在文件后半段的预览那一节里）。 */
static void mini_snapshot_place(struct vidow_view *view);
/* 小窗（S3）：窗口的原生尺寸（场景坐标）。 */
void view_mini_native(const struct vidow_view *view, int *w, int *h);
static void view_ensure_column(struct vidow_server *server,
		struct vidow_view *view);
void vidow_view_leave_column(struct vidow_view *view);
/* X11：按窗口的绝对坐标把它归到某块输出上（弹出物可以开在另一块屏上）。 */
void x11_track_output(struct vidow_view *view);
/* 这块屏的归属记忆（定义在文件后半段的热插拔那一节）。 */
static struct vidow_output_memory *home_of(struct vidow_server *server,
		struct wlr_output *wlr_output);

/* 把一个窗口放回平铺布局（浮动 → 平铺那次重判走这里，见 float.c 的
 * vidow_float_set_mode）。
 *
 * **没上屏的窗口不给列**：列是条带上的一格，占着它就是占着屏上的宽度 ——
 * 一个没 map 的窗口进了列，屏上什么都不画、那格却照占，就是用户报的「凭空
 * 多出一列」。proton / Steam 那批辅助窗（现场 dump 里标题 `Input` 的窗口 50）
 * 就是这么来的：X11 窗口**从来没 map 过**，可它的属性（WM_TRANSIENT_FOR /
 * WM_NORMAL_HINTS / `_NET_WM_WINDOW_TYPE`）在运行中被改掉，vidow 于是把它从
 * 浮动重判成平铺 —— 这一步以前不看它上没上屏，直接建了一条列（几何 2883,6
 * 951x1068，正卡在 lutris 和游戏之间）。
 *
 * 延迟到首次 map：view_map() 里 mapped 先置 true，随后自己会走
 * view_ensure_column() 补上这条列，所以上屏的窗口一个都不少。
 *
 * 注意别把这条判据搬到 view_ensure_column() 里：xdg 的初始 commit 那一趟
 * （view_commit 的 initial_commit 分支）**故意**在 map 之前建列 —— 客户端要拿
 * 着「平铺格的尺寸」这个 configure 才会上屏，拦掉它窗口就会先按自己的尺寸冒
 * 出来再跳一下（而且抢跑的最大化请求也兑现在那儿）。 */
void vidow_view_join_layout(struct vidow_server *server,
		struct vidow_view *view) {
	if (!view->mapped) {
		wlr_log(WLR_DEBUG, "join layout: %s 还没上屏，先不给列（等 map）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
		return;
	}
	view_ensure_column(server, view);
}

void vidow_view_leave_layout(struct vidow_view *view) {
	vidow_view_leave_column(view);
}


/* ------------------------------------------------------------- columns */

static bool column_has_views(struct vidow_column *col) {
	return !wl_list_empty(&col->views);
}

/* 最小化的窗口**留在原来的列里**（位置、列宽、上下顺序、归属工作区全都不用重建，
 * 放回来是瞬时的），但它不占位：布局、焦点遍历、命中都要跳过它。列里就剩它一个的
 * 时候，那条列也跟着从条带里消失（niri 的条带是「有东西才占宽度」）。
 *
 * 注意别把它当成「藏起来的工作区」那种不可见：那边是整列都不在条带上，这边列还在，
 * 只是这条列可能空着。所以下面这些判据是「还占着位置」而不是「场景节点开着」。 */
static bool view_shows(const struct vidow_view *view) {
	/* **不要在这里加 view->mapped。** 试过了（19e4c19），代价是 X11 客户端
	 * 起不来 —— 用户 2026-10-28 01:25 报「lutris 起不来了」。
	 *
	 * 原因是一条死锁（和同文件 x11 新窗口那段注释里写的 Steam 黑屏同一个
	 * 形状）：X11 窗口要拿到一次 configure 才 XMapWindow，map 之前 mapped
	 * 是 false；把它判成「不占位」→ column_has_shown_views() 返回 false →
	 * arrange_columns 的 Pass 1 把这条列宽度清零、不摆它 → vidow_place_view
	 * 不发 configure → 客户端永远不 map → 永远占不了位。窗口就再也出不来。
	 *
	 * 「关窗后留一条空白带」那个 bug 得在别处修（空列没被 leave_column 摘
	 * 掉 / 宽度没被重算），不能靠改这个判据。 */
	return view != NULL && !view->minimized && !view->drag_detached &&
		!view->focus_helper;
}

/* 列里第一个「还占着位置」的窗口（焦点落点用）。 */
static struct vidow_view *column_first_shown_view(struct vidow_column *col) {
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		if (view_shows(view)) {
			return view;
		}
	}
	return NULL;
}

/* 这条列在条带上占位置吗（整列都最小化了就不占）。 */
static bool column_has_shown_views(struct vidow_column *col) {
	return column_first_shown_view(col) != NULL;
}

/* 这条列里有全屏窗口吗。
 *
 * 有的话这条列在条带上占**一整屏**宽（不是它自己的 width_fraction）：全屏的窗口
 * 画出来就是整块屏那么大，滚走它自然也该滚一整屏 —— 否则滚一下只挪几个像素，
 * 它照样糊在整块屏上（用户报「全屏的窗口也要参与滚动」）。判据只认「全屏 + 已上屏
 * + 没最小化」，跟摆放那一趟用的完全一致（见 output_arrange 的 Pass 3）。 */
static bool column_has_fullscreen(struct vidow_column *col) {
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		if (view->fullscreen && view->mapped && view_shows(view)) {
			return true;
		}
	}
	return false;
}

/* 聚焦的这条列「在条带上多宽」：全屏的那条算一整屏 —— 全屏窗口画出来就是整块
 * 屏那么大，它在条带上占的位置自然也该是一整屏（不是给全屏开特例：列就是列，
 * 只是它的宽度就是一屏）。滚动（Pass 2 / 预测）两处都用这一个。 */
static int column_strip_width(struct vidow_column *col, int work_width) {
	return column_has_fullscreen(col) ? work_width : col->width;
}

/* 「把焦点列摆到视口里，视口该在哪」—— niri 的 scroll_amount_to_activate 算的是
 * 同一件事。三种策略（configs/20-layout.lua 的 center_focused_column）：
 *
 *   never（默认）  最小滚动：只滚「刚好让焦点列整个露出来」的那点距离，焦点列
 *                  因此常常贴着视口左 / 右边缘 —— vidow 一直以来的行为。
 *   always         焦点列永远摆到视口正中（条带两头夹住，见下面的 clamp）。
 *   on-overflow    焦点列和**上一个焦点列**能一起完整放进视口就不动；放不下
 *                  （= 溢出）才居中。niri 的同名取值。
 *
 * 只算「该在哪」，不真改视口 —— 调用方自己 clamp 到 0..max_viewport。 */
static double focus_viewport_target(struct vidow_server *server,
		struct vidow_output *output, double viewport, int work_width) {
	if (work_width < 1) {
		return viewport;
	}
	struct vidow_view *focus = server->focused;
	if (focus != NULL && focus->float_mode == VIDOW_FLOAT_FOLLOW &&
			focus->parent != NULL) {
		/* scroll to where the dialog's parent lives */
		focus = focus->parent;
	}
	struct vidow_column *focused_col = focus != NULL ? focus->column : NULL;
	if (focused_col == NULL || focused_col->output != output->wlr_output) {
		return viewport;
	}

	/* 居中策略可以按屏覆盖（niri 的 output { layout { center-focused-column } }）：
	 * 读这块屏最终生效的那份值。 */
	struct vidow_layout lay;
	vidow_layout_for(server, output->wlr_output, &lay);

	/* 这条列在条带上占多宽（全屏那条 = 一整屏）。 */
	int cw = column_strip_width(focused_col, work_width);
	/* 居中要用的宽度：列比视口还宽就没法居中（夹住，跟 niri 一样）。 */
	int center_w = cw > work_width ? work_width : cw;

	if (lay.center_focused_column == VIDOW_CENTER_ALWAYS) {
		return focused_col->offset - (work_width - center_w) / 2.0;
	}

	if (lay.center_focused_column == VIDOW_CENTER_ON_OVERFLOW) {
		/* 和上一个焦点列能不能一起完整放进视口。上一个没了 / 在别的屏上 /
		 * 就在同一条列里 —— 都算「放得下」（不居中）。 */
		struct vidow_column *prev = server->last_focused_col;
		bool fits = true;
		if (prev != NULL && prev != focused_col &&
				prev->output == output->wlr_output &&
				column_has_shown_views(prev)) {
			int pw = column_strip_width(prev, work_width);
			int lo = prev->offset < focused_col->offset ?
				prev->offset : focused_col->offset;
			int hi_a = prev->offset + pw;
			int hi_b = focused_col->offset + cw;
			int hi = hi_a > hi_b ? hi_a : hi_b;
			fits = hi - lo <= work_width;
		}
		if (!fits) {
			return focused_col->offset - (work_width - center_w) / 2.0;
		}
		/* 放得下：退回最小滚动（下面那段）。 */
	}

	/* 最小滚动：焦点列在视口右边外面就往左滚到贴右边缘，在左边外面就往右
	 * 滚到贴左边缘；已经在里面就不动。 */
	double target = viewport;
	int right = focused_col->offset + cw;
	if (right > target + work_width) {
		target = right - work_width;
	}
	if (focused_col->offset < target) {
		target = focused_col->offset;
	}
	return target;
}

/* 这块屏上现在该摆哪些列：有特殊工作区开着就摆它的（普通工作区让位），否则摆
 * 活动的那块普通工作区。空指针（还没建过工作区）返回空链表，arrange 就当没列。 */
static struct wl_list *output_shown_columns(struct vidow_server *server,
		struct wlr_output *output) {
	/* 每块屏摆自己的那块 —— 不能问「现在显示着的工作区」（那是聚焦屏的），
	 * 否则另一块屏上的窗口在 arrange 里被跳过、画不出来。 */
	struct vidow_workspace *ws = vidow_workspace_shown_for(server, output);
	if (ws == NULL || ws->output != output) {
		static struct wl_list empty;
		wl_list_init(&empty);
		return &empty;
	}
	return &ws->columns;
}

static void view_ensure_column(struct vidow_server *server,
		struct vidow_view *view) {
	if (view->column != NULL) {
		return;
	}
	/* **弹出物不进列。** 列是条带上的位置，popup 的位置跟父窗口走
	 * （见 view_popup 的定位），它根本不需要一条列。
	 *
	 * 漏了这条判据的症状（用户 2026-10-28 01:09 报「屏幕上出现一条空白带，
	 * 窗口没铺满」）：popup 的 float_mode 是 NONE（detect_mode 见它没有固定
	 * 尺寸提示，见 float.c），view_map 里 `float_mode == NONE → view_ensure_column`
	 * 就给它建了一条列；而关闭时 view_unmap 的 popup 分支直接 return
	 * （只还键盘 + arrange，见同文件 view_unmap），列没被摘。那条列里挂着一个
	 * popup view，`vidow_ipc_json_column` 的 windows 只报 window_is_toplevel
	 * 的窗口（popup 被排除），于是 IPC 看着是 `windows: []`；可
	 * vidow_workspace_cleanup 用的是 `wl_list_empty(&col->views)`，链表非空 →
	 * 永远不删 → arrange 还按旧宽度摆它 → 屏上一条什么都没有的竖条。
	 *
	 * 不放 view_unmap 那边修：那个分支 return 得早，是为了 popup 的键盘语义；
	 * 让 popup 从一开始就别建列更干净，arrange / 焦点 / 命中都不用再判一次。 */
	if (view->popup) {
		return;
	}

	struct vidow_column *col = calloc(1, sizeof(*col));
	if (col == NULL) {
		return;
	}
	col->server = server;
	col->id = ++server->next_column_id;
	col->output = view->output;
	/* 列宽：窗口规则写了 column_width 就用它（这个窗口新开的列有多宽），
	 * 没写用配置里的 default_column_width（默认 0.5，就是原来写死的那个
	 * DEFAULT_COLUMN_WIDTH —— 见 configs/20-layout.lua）。新列不做过渡
	 * （它是刚生出来的，宽度没有「变化」），width_shown 跟逻辑值一起给上
	 * —— arrange 画的是 width_shown。 */
	struct vidow_layout lay;
	vidow_layout_for(server, view->output, &lay);
	col->width_fraction = view->rule_eff.set[VRA_COLUMN_WIDTH] ?
		view->rule_eff.val[VRA_COLUMN_WIDTH].d :
		lay.default_column_width;
	col->width_shown = col->width_fraction;
	wl_list_init(&col->views);

	/* 这条列落在哪块工作区：窗口自己记着的那块（新窗口在 map 时就定好了，
	 * 见 view_map），没有就落到这块屏的活动工作区上。 */
	struct vidow_workspace *ws = view->ws;
	if (ws == NULL) {
		ws = view->output != NULL ?
			vidow_workspace_active_for(server, view->output) :
			vidow_workspace_active(server);
		if (ws == NULL) {
			/* **一块屏都没有**（嵌套会话的外层窗口关了 / 最后一块屏拔了）：
			 * 这时候没有「当前工作区」这回事，但窗口照样会 map —— XWayland
			 * 还活着，X 客户端一连上来就 map（用户报的「打开 kate 直接崩」
			 * 就是这条：原来这里直接解引用 NULL，SEGV 在 view_map 里）。
			 * 安置到一块现在没屏的工作区上：屏回来的时候那块工作区会被认领
			 * 回去（vidow_workspace_adopt_homeless），窗口跟着一起回来。 */
			ws = vidow_workspace_homeless(server,
				home_of(server, view->output));
			if (ws == NULL) {
				ws = vidow_workspace_create(server, NULL, NULL);
			}
		}
		view->ws = ws;
	}
	if (ws != NULL) {
		col->ws = ws;
		wl_list_init(&col->link);
		/* New windows open in a column to the right of the focused one. */
		struct vidow_column *focused_col =
			server->focused != NULL ? server->focused->column : NULL;
		if (focused_col != NULL && focused_col->ws == ws) {
			wl_list_insert(&focused_col->link, &col->link);
		} else {
			wl_list_insert(ws->columns.prev, &col->link);
		}
	} else {
		wl_list_init(&col->link); /* 没有工作区可归：挂在自己头上，谁都摆不到 */
	}

	view->column = col;
	wl_list_insert(col->views.prev, &view->column_link);
	/* 落在栈底那块空的上了：它就是「新工作区」，底下再补一块（niri 的「窗口开
	 * 在最后一块空工作区上时，下面立刻出现一块新的」）。要在列真的挂进工作区
	 * 之后再判 —— 不然这时候它还是「空的」。 */
	if (ws != NULL && !ws->special && ws->output != NULL) {
		vidow_workspace_ensure_bottom(server, ws->output);
	}
}

void vidow_view_leave_column(struct vidow_view *view) {
	if (view->column == NULL) {
		return;
	}
	struct vidow_column *col = view->column;
	wl_list_remove(&view->column_link);
	view->column = NULL;
	if (!column_has_views(col)) {
		if (col->ws != NULL) {
			wl_list_remove(&col->link);
		}
		/* 列宽过渡的 data 就是这条列：不掐掉的话下一帧它会写一块已经 free
		 * 掉的内存（列没了但补间还在跑）。 */
		vidow_anim_cancel(col->anim_width);
		col->anim_width = NULL;
		/* 「上一个焦点列」指着它的话先松手 —— 这条列马上就不是列了
		 * （center_focused_column = "on-overflow" 要用，见
		 * focus_viewport_target）。 */
		if (view->server->last_focused_col == col) {
			view->server->last_focused_col = NULL;
		}
		free(col);
	}
}

/* ------------------------------------------------------------- layout */

/* wlr_scene_xdg_surface_create() nests the scene surface inside a tree of its
 * own, so dig for it instead of assuming a flat layout. */
static struct wlr_scene_surface *find_scene_surface(struct wlr_scene_node *node,
		struct wlr_surface *want) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
			wlr_scene_buffer_from_node(node));
		return ss != NULL && ss->surface == want ? ss : NULL;
	}
	if (node->type != WLR_SCENE_NODE_TREE) {
		return NULL;
	}
	struct wlr_scene_tree *tree = wl_container_of(node, tree, node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		struct wlr_scene_surface *ss = find_scene_surface(child, want);
		if (ss != NULL) {
			return ss;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------ animation */

/* 窗口**属于**哪块屏：平铺 / 全屏的窗口看**它那条列**挂在哪块屏
 * （view->column->output）—— 跟滚动视口无关，稳定。
 *
 * 浮动 / 弹窗返回 NULL：它们不参与滚动（geom 就是自己的绝对位置），按「现在画在
 * 哪块屏上」算才对 —— 拖着跨屏时 view->output 还停在原来那块屏上（见 view_scale
 * 的注释）。
 *
 * 为什么平铺的不能按「现在画在哪」算：平铺窗口的 geom 里含着**滚动视口的偏移**
 * （output_arrange: x = 工作区 + 列偏移 - 视口）。窗口滚出视口之后，它画的那些
 * 像素落在隔壁屏的布局范围里（场景是全局坐标的，见 vidow_view_in_output_box），
 * 于是「画在哪块屏上」的答案变成隔壁那块 —— 倍率、模糊底衬、窗口录屏的取景框都
 * 跟着错（越界的那一块本来就被裁掉了、压根不画，隔壁那块屏跟这个窗口没关系）。 */
struct wlr_output *vidow_view_owner_output(const struct vidow_view *view) {
	/* 唯一真值：窗口属于工作区，工作区属于屏。浮窗也一样（它只是不占列宽，
	 * 不是「不属于工作区」）。弹窗 / 小窗没有工作区，各自按自己的位置算。 */
	if (view->popup || view->mini != VIDOW_MINI_NONE) {
		return NULL;
	}
	if (view->ws != NULL && view->ws->output != NULL) {
		return view->ws->output;
	}
	if (view->column != NULL) {
		return view->column->output;
	}
	return NULL;
}

bool view_clip_box(struct vidow_view *view, struct wlr_box *out);

/* 「看得见的那一块」在布局坐标里的框（哪块屏上都没画着时是空的）：
 *   停到停车位（滚出视口 / 藏起来的工作区）→ 空；
 *   裁着（平铺窗口有一部分越出自己那块屏）→ 几何框 ∩ 自己那块屏的框；
 *   其余（浮窗 / 正被拖的窗口 / 整块都在自己屏里）→ 几何框本身。
 *
 * 为什么不能直接用几何框：平铺窗口的几何框里含着**滚动视口的偏移**，滚出视口之后
 * 会跨到隔壁屏的布局范围里 —— 照它算，倍率就落到隔壁那块屏上：2 倍屏上的窗口在
 * 录屏 / 预览里一直按 1 倍糊着，滚回视口才清楚（用户报的「窗口滚动出去后，又按照
 * 另一个屏幕的缩放变化了」）。裁剪（view_clip_box）裁掉的正是越界那一块，所以
 * 「看得见的」= 几何框 ∩ 那块屏的框。 */
static void view_visible_box(const struct vidow_view *view, struct wlr_box *out) {
	*out = (struct wlr_box){0};
	if (!view->mapped) {
		return;
	}
	if (vidow_view_should_park(view->server, (struct vidow_view *)view)) {
		return; /* 停到停车位：哪块屏上都没有它，判据回退到「它属于哪块屏」 */
	}
	struct wlr_box box = vidow_view_screen_box(view);
	struct wlr_box clip = {0};
	if (view_clip_box((struct vidow_view *)view, &clip)) {
		struct wlr_box hit;
		if (!wlr_box_intersection(&hit, &box, &clip)) {
			return; /* 裁完不剩什么 */
		}
		box = hit;
	}
	*out = box;
}

/* 这块窗口**算在哪块屏上**（倍率 / 模糊底衬都按它）：**看得见的那一块**画在哪块
 * 屏上 —— 它看得见的那些屏里缩放最大的那块（跨在两块屏上时取大的那个，理由见
 * 下面 view_scale 的注释）；一点都看不见（滚出视口、藏起来的工作区、还没摆过）时
 * 回退到「它属于哪块屏」（vidow_view_owner_output，浮动 / 弹窗没有归属时返回 NULL）。
 *
 * 「看得见的那一块」是关键：平铺窗口的几何框里含着滚动视口的偏移，滚出视口之后
 * 会跨到隔壁屏的布局范围里，但那一块被裁掉了（见函数里的说明）。
 *
 * 装饰的离屏缓冲倍率（view_scale）和私有模糊底衬归哪块屏（fx.c 的
 * vidow_blur_update）都走这一个判据 —— 「这块窗口现在算在哪块屏上」只能有一个
 * 答案，两个地方各算各的就会错位（模糊贴图的采样坐标是按底衬那块屏的设备像素算
 * 的，倍率对不上就采错地方）。 */
static struct wlr_output *view_drag_output(const struct vidow_view *view);

struct wlr_output *vidow_view_display_output(const struct vidow_view *view) {
	if (!view->mapped) {
		return NULL;
	}
	/* 拖拽中：窗口真的已经跟着光标画到对面那块屏上了 —— 模糊底衬的区域、
	 * 采样 UV 都必须按**那块屏的设备像素**算，按旧归属屏算出来就是一片横条
	 * （贴图尺寸 / 原点和实际画的地方对不上，见 view_drag_output 的说明）。
	 * 放在最前面：平铺窗口那条归属捷径会直接短路成 ws->output。 */
	struct wlr_output *dragged = view_drag_output(view);
	if (dragged != NULL) {
		return dragged;
	}
	/* 有工作区的窗口：显示归属直接取链路（窗口 -> 工作区 -> 屏）。不再按
	 * 几何框找「看得见的那块」—— 窗口跨在屏边界上时，几何框会在两块屏之间
	 * 来回翻，倍率跟着 1/2 反复切（wlr_scene_surface 也按可见面积发
	 * preferred_buffer_scale，两边对着跳）。 */
	if (!view->popup && view->mini == VIDOW_MINI_NONE) {
		struct wlr_output *chain = vidow_view_owner_output(view);
		if (chain != NULL) {
			return chain;
		}
	}
	/* 判据是「**看得见**的那一块现在画在哪块屏上」：
	 *   - 平铺窗口滚出视口时，几何框里含着滚动视口的偏移、会跨到隔壁屏的布局范围
	 *     里 —— 但越界的那一块被裁掉了（view_clip_box / fx.clip），根本没人画。
	 *     照几何框算，倍率就落到隔壁那块屏上（2 倍屏上的窗口录 / 预览时一直按
	 *     1 倍糊着，滚回视口才清楚 —— 用户报的「窗口滚动出去后，又按照另一个
	 *     屏幕的缩放变化了」）。
	 *   - 拖着窗口跨屏的那一段**不裁**（浮窗、正被拖的窗口允许跨屏，见
	 *     view_clip_box），可见部分就是几何框 —— 那时候必须按对面那块屏算，
	 *     不然拖到 4K 屏上整块窗口是放大出来的（test-mixdpi 钉着这一条）。
	 *   - 一点都看不见（滚出视口 / 藏起来的工作区 / 还没摆过）：回退到「它属于
	 *     哪块屏」（平铺的看它那条列，浮动 / 弹窗没有归属 → NULL）。
	 * 「看得见的那一块」由 view_visible_box 现算（不看上一帧存下来的 clip 状态：
	 * 摆位那份比裁剪那份先跑，读存下来的会落后一帧 —— 于是「刚滚出视口」那一帧
	 * 按几何框算成隔壁屏、把客户端的倍率钉错，见 view_pin_scale）。
	 */
	struct wlr_box box = {0};
	view_visible_box(view, &box);
	struct wlr_output *best = NULL;
	double best_scale = 0.0;
	if (box.width > 0 && box.height > 0) {
		struct vidow_output *output;
		wl_list_for_each(output, &view->server->outputs, link) {
			struct wlr_output *wlr_output = output->wlr_output;
			if (wlr_output == NULL || !wlr_output->enabled) {
				continue;
			}
			struct wlr_box obox;
			wlr_output_layout_get_box(view->server->output_layout,
				wlr_output, &obox);
			struct wlr_box hit;
			if (!wlr_box_intersection(&hit, &box, &obox)) {
				continue;
			}
			double scale = wlr_output->scale > 0.0 ?
				wlr_output->scale : 1.0;
			if (scale > best_scale) {
				best_scale = scale;
				best = wlr_output;
			}
		}
	}
	if (best == NULL) {
		best = vidow_view_owner_output(view);
	}
	return best;
}

/* 窗口**属于**哪块屏就按那块屏的缩放烘（平铺的看它那条列，浮动 / 弹窗看它现在
 * 画在哪块屏上，见 vidow_view_display_output）。装饰着色器的离屏缓冲按它分配 ——
 * 「同一块窗口在不同 DPI 的屏上都清晰」的另一半：客户端那边靠 buffer_scale 声明
 * 自己画了多大，这边靠渲染倍率保证装饰和着色器也是按设备像素画的。
 *
 * 浮动窗口拖着跨屏的那一段，判据是**画在哪**而不是归属（view->output）：归属要到
 * 松手才换，可它已经在对面那块屏上画着了。按 view->output 算，离屏缓冲还是旧倍率
 * （实测：拖到 4K 屏上还是 2076x1236 的 1 倍缓冲），整块窗口被放大到新屏上 ——
 * 4K 屏上一直糊着，松手（归属换了）才清楚。跨在两块屏上时取大的那个倍率：客户端
 * （GTK 那类）一碰到 2 倍屏就按 2 倍重画了，我们按 1 倍烘的话它交上来的 2 倍缓冲
 * 还得先缩下去、屏幕上再放大 —— 还是糊。宁可多烘（画到 1 倍屏上时是缩下去，仍然
 * 清晰），不能少烘（画到 4K 屏上是放大，糊）。
 *
 * 平铺窗口不这么算：它的 geom 里含着滚动视口的偏移，滚出视口之后「画在哪块屏上」
 * 会翻到隔壁那块去（见 vidow_view_owner_output）。 */
/* **窗口属于哪块屏 —— 唯一出口**。
 *
 * 按「窗口挂载的工作区」算，不是按它此刻画在哪儿。几何框靠不住：窗口贴着屏
 * 边界（比如 x=1917，隔壁屏从 1920 开始）、或者摆放动画里扫过隔壁屏时，
 * view->output 会短暂失效 / 指到隔壁屏上 —— 倍率被钉成隔壁屏的值（Chromium
 * 收到 2 倍后不会自己切回 1，内部页面一直按 2 倍排版），裁切也会因为拿不到
 * 屏框而整块放行（表面画到隔壁屏上，wlroots 再发一次 enter + 2 倍，两边对着
 * 跳，实测 15ms 一轮）。
 *
 * 工作区的归属只有真的换屏（换工作区 / 拖过去）才变，动画和贴边都不影响。
 * 倍率、裁切都从这里取，别再各写一套。
 *
 * **拖拽中是唯一的例外**：窗口正被往另一块屏上搬，得按落点那块屏算，否则
 * 落到 2 倍屏上时还是 1 倍缓冲、整块是放大出来的（test-mixdpi 钉这条）。 */
static struct wlr_output *view_home_output(const struct vidow_view *view) {
	/* 窗口 -> 工作区 -> 屏。拖动中也不破例：窗口还没换工作区，就还在原来
	 * 那块屏上；松手换了工作区，倍率才跟着换。 */
	if (view->popup || view->mini != VIDOW_MINI_NONE) {
		struct wlr_output *out = vidow_view_display_output(view);
		if (out != NULL) {
			return out;
		}
	}
	if (view->ws != NULL && view->ws->output != NULL) {
		return view->ws->output;
	}
	if (view->output != NULL) {
		return view->output;
	}
	return vidow_view_display_output(view);
}

/* 拖拽中窗口**此刻画在哪块屏上**（平铺 / 浮窗 / 小窗都算）。不在拖拽里返回
 * NULL —— 那时候倍率该按归属屏算（view_home_output），不能按「画在哪」翻。
 *
 * 为什么拖拽要单独一份：窗口跟着光标越过屏边界之后，它**真的已经画在对面那块
 * 屏上了**（场景图是全局坐标，见 vidow_view_screen_box），而 view_home_output
 * 要到松手换工作区才变。两者不一致时：
 *
 *   - 装饰离屏缓冲按旧屏倍率分配（ensure_targets 按 fx->scale 缓存，换屏不重建），
 *     节点却摆在新屏上 —— 拖到 2 倍屏上整块是放大糊的；
 *   - 裁剪框取旧屏（view_clip_box），窗口一越过边界就被裁到旧屏框里，屏幕上
 *     表现为「拖到一定距离窗口就没了」；
 *   - 私有模糊底衬的区域原点 / 采样 UV 按旧屏设备像素算，装饰却画在新屏上 ——
 *     UV 在 V 方向恒定，就是用户报的「一片横条」。
 *
 * 判据取**窗口覆盖面积最大**的那块屏（和 vidow_view_display_output 里跨屏那段
 * 同一个思路），避免刚好骑在边界上时来回翻。 */
static struct wlr_output *view_drag_output(const struct vidow_view *view) {
	if (view->server == NULL || !view->mapped) {
		return NULL;
	}
	bool dragging = view->dragging || view->server->grab_view == view;
	if (!dragging) {
		return NULL;
	}
	/* **拖拽中按光标在哪块屏判，不按窗口面积判。**
	 *
	 * 面积判法在两块屏逻辑尺寸相近、而窗口本身又接近整屏宽的时候会失灵：
	 * 实测这台机器 DP-3 = 1920x1080@1x、DP-2 = 1920x1080@2x，从 DP-2 拖一个
	 * 1904x1025 的窗口去 DP-3，窗口要拖到 x<1000（中心早就越过边界）才够
	 * 让 DP-3 的可见面积超过 DP-2 —— 那之前一路都算「还在 DP-2」，倍率钉在
	 * 2 倍不变。用户看到的就是两件事：
	 *   1. 高分辨率屏往低分辨率拖，像被窗口大小卡住，过不去；
	 *   2. 跨屏那一半发糊（按 2 倍渲染，却画在 1 倍屏上）。
	 *
	 * 拖拽时用户看的是光标，不是窗口面积：光标落在哪块屏，就把那块屏的
	 * 倍率给窗口。这也和「窗口真的跟着光标画到对面那块屏上」的既有约定一致
	 * （vidow_view_display_output 依赖这里给出正确的屏来算模糊底衬 / UV）。
	 *
	 * 光标不在任何输出上（拖到布局外）才退回面积判法，保持原来的兜底。 */
	/* **光标所在屏和窗口相交的屏里，取倍率最高的那块。**
	 *
	 * 光看光标是不够的（2026-10-27 用户实测复现）：窗口 1904 宽停在 DP-2
	 * （2x）上，光标一越过边界进 DP-3（1x），这里就返回 DP-3 —— 而窗口
	 * 1904 像素里还有一大半画在 DP-2 上。三处判据于是分叉：
	 *
	 *   view_scale()      取「相交屏里最高倍率」= 2.0  ← 客户端按 2x 画
	 *   view_drag_output() 取光标屏        = DP-3  ← 底衬 / UV 按 1x 算
	 *
	 * 屏幕上看到的就是「跨屏时高分辨率那一半模糊」：2x 的缓冲被按 1x 的
	 * 设备像素采样。
	 *
	 * 一块 wl_surface 只有一个 buffer_scale，跨两块不同倍率的屏时**只能**
	 * 选一个（选低的必然糊），所以两处必须选同一个 —— 都按「相交屏里
	 * 最高的那个」，和 view_scale 的既有理由一致（见那里的注释）。
	 *
	 * 光标屏只作为「窗口一点都没跨到别的屏上」时的自然结果：窗口只和
	 * 光标那块屏相交时，最高倍率就是那块屏的。 */
	struct wlr_output *under = wlr_output_layout_output_at(
		view->server->output_layout,
		view->server->cursor->x, view->server->cursor->y);
	struct wlr_box wbox = vidow_view_screen_box(view);
	struct wlr_output *pick = NULL;
	double pick_scale = 0.0;
	struct vidow_output *o;
	wl_list_for_each(o, &view->server->outputs, link) {
		struct wlr_output *wo = o->wlr_output;
		if (wo == NULL || !wo->enabled) {
			continue;
		}
		if (wbox.width > 0 && wbox.height > 0) {
			struct wlr_box obox;
			wlr_output_layout_get_box(view->server->output_layout, wo,
				&obox);
			struct wlr_box hit;
			if (!wlr_box_intersection(&hit, &wbox, &obox)) {
				continue;
			}
		} else if (wo != under) {
			continue;
		}
		double sc = wo->scale > 0.0 ? wo->scale : 1.0;
		/* 倍率相同的时候优先光标那块屏 —— 用户看的是光标。 */
		if (pick == NULL || sc > pick_scale ||
				(sc == pick_scale && wo == under)) {
			pick = wo;
			pick_scale = sc;
		}
	}
	if (pick != NULL) {
		return pick;
	}
	if (under != NULL && under->enabled) {
		return under;
	}

	struct wlr_box box = vidow_view_screen_box(view);
	if (box.width <= 0 || box.height <= 0) {
		return NULL;
	}
	struct wlr_output *best = NULL;
	int64_t best_area = 0;
	struct vidow_output *output;
	wl_list_for_each(output, &view->server->outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		if (wlr_output == NULL || !wlr_output->enabled) {
			continue;
		}
		struct wlr_box obox;
		wlr_output_layout_get_box(view->server->output_layout, wlr_output,
			&obox);
		struct wlr_box hit;
		if (!wlr_box_intersection(&hit, &box, &obox)) {
			continue;
		}
		int64_t area = (int64_t)hit.width * (int64_t)hit.height;
		if (area > best_area) {
			best_area = area;
			best = wlr_output;
		}
	}
	return best;
}

static double view_scale(const struct vidow_view *view) {
	struct wlr_output *out = view_drag_output(view);
	if (out == NULL) {
		out = view_home_output(view);
	}
	if (out == NULL) {
		return 1.0;
	}
	double scale = out->scale;
	if (scale <= 0.0) {
		scale = 1.0;
	}
	/* **跨屏拖拽时取两块屏里较高的倍率。**
	 *
	 * 一块 wl_surface 只有一个 buffer_scale，横跨两块 scale 不同的屏时，
	 * 不管选哪个，另一半都只能靠缩放去凑：
	 *   选低的（1x）：2x 那半把 1 倍缓冲放大 —— 糊，用户报的
	 *                 「跨屏的时候高分辨率那一半就模糊」。
	 *   选高的（2x）：1x 那半把 2 倍缓冲缩小 —— 清晰，只是多画像素。
	 *
	 * 所以拖拽期间跨屏时按高的那个走。判据只看「拖拽中 + 真的和别的屏相交」，
	 * 不拖的时候仍然按归属屏（否则停在 2x 屏上的窗口会一直按别的屏的倍率
	 * 渲染，白烧带宽）。
	 *
	 * 注：这不改变「松手之后归到新工作区、按那块屏的倍率重画」的最终结果，
	 * 只影响拖拽过程里那几帧的观感。 */
	if (view->dragging && view->server != NULL) {
		struct wlr_box box = vidow_view_screen_box(view);
		if (box.width > 0 && box.height > 0) {
			/* **先数清楚窗口现在压在几块屏上。**
			 *
			 * 只有真跨屏（>= 2 块）才需要「取最高倍率」这个折中。窗口
			 * 完全落在某一块屏里的时候，那块屏的倍率就是唯一正确的答案
			 * —— 按别的屏的倍率渲染，底衬贴图的原点 / 尺寸全对不上，
			 * 屏幕上就是跨屏拖完之后背景错位 / 横条。
			 *
			 * 以前这里没有这一步：scale 的初值来自 view_drag_output()，
			 * 而它**本身**就返回「相交屏里最高倍率的那块」。于是
			 * `wo->scale <= scale` 这个跳过条件把每一块屏都跳过了 ——
			 * 循环一次都没生效过，scale 永远停在初值上。窗口从 2x 的
			 * DP-2 拖进 1x 的 DP-3、整个都在 DP-3 里了，还在按 2x 算。 */
			struct vidow_output *hits[8];
			size_t hit_n = 0;
			struct vidow_output *o;
			wl_list_for_each(o, &view->server->outputs, link) {
				struct wlr_output *wo = o->wlr_output;
				if (wo == NULL || !wo->enabled) {
					continue;
				}
				struct wlr_box obox;
				wlr_output_layout_get_box(view->server->output_layout,
					wo, &obox);
				struct wlr_box hit;
				if (!wlr_box_intersection(&hit, &box, &obox)) {
					continue;
				}
				if (hit_n < sizeof(hits) / sizeof(hits[0])) {
					hits[hit_n++] = o;
				}
			}
			if (hit_n == 1) {
				/* 只压着一块：就是它，不折中。 */
				double only = hits[0]->wlr_output->scale;
				scale = only > 0.0 ? only : 1.0;
			} else if (hit_n > 1) {
				/* 真跨屏：取最高的那个（一块 wl_surface 只能有一个
				 * buffer_scale，选低的必然糊 —— 见上面那段）。 */
				for (size_t i = 0; i < hit_n; i++) {
					double sc = hits[i]->wlr_output->scale;
					if (sc > scale) {
						scale = sc;
					}
				}
			}
		}
	}
	if (getenv("VIDOW_SCALE_TRACE") != NULL && view->dragging) {
		wlr_log(WLR_INFO, "SCALETRACE %s drag=1 -> scale=%.2f "
			"geom=%d,%d %dx%d grab_mode=%d",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			scale, view->geom.x, view->geom.y,
			view->geom.width, view->geom.height,
			(int)view->server->grab_mode);
	}
	return scale;
}

struct pin_scale_ctx {
	int32_t want;
};

static void pin_scale_surface(struct wlr_surface *surface, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	struct pin_scale_ctx *ctx = data;
	/* 值没变就一个事件都不发。以前每次 commit / 每帧 arrange 都发一遍：
	 * wlr_surface_set_preferred_buffer_scale 自己会去重，但
	 * wlr_fractional_scale_v1_notify_scale 不会 —— Electron / GTK 每收一次
	 * 就重算一次缓冲，surface 在 1x/2x 之间来回跳，屏幕上就是界面大小一直
	 * 微微抖动（layer / 2x 那条也是同一类）。 */
	if (surface->WLR_PRIVATE.preferred_buffer_scale == ctx->want) {
		return;
	}
	wlr_surface_set_preferred_buffer_scale(surface, ctx->want);
	/* **fractional-scale 那条也要发**。Chromium / Electron / 新版 GTK 优先听
	 * wp_fractional_scale_v1.preferred_scale，它比 wl_surface.preferred_buffer_scale
	 * 优先 —— 只发后者的话它们按「上一次进过的输出」的倍率画：窗口从 DP-2（2x）
	 * 搬到 DP-3（1x）之后仍然画 2x，像素多 4 倍（实测 Element 2304x2560 铺在
	 * 949x1025 上）。这是「所有应用都卡」的真根因。 */
	wlr_fractional_scale_v1_notify_scale(surface, (double)ctx->want);
}

/* 客户端按几倍渲染：**钉在它自己那块屏的倍率上**（view_scale）。子表面一起钉
 * （mpv / vlc 那类把画面放子表面里，只钉顶层的话它们会掉回 1 倍）。
 *
 * 不钉会怎样：窗口滚出视口之后会被停到**所有输出框外面**的停车位
 * （vidow_view_set_parked）—— 它就不再「在」任何输出上了，客户端（kitty 这类按
 * 进了哪些输出算倍率的）退到 1 倍重画：4K 屏上的窗口在按窗口录屏 / 预览里变成
 * 半分辨率、糊着，滚回视口才清楚（用户报的「窗口滚动出去后，又按照另一个屏幕的
 * 缩放变化了」）。wl_surface.preferred_buffer_scale 的优先级**高于**「进了哪些
 * 输出」（kitty: `integer_scale.preferred` 顶替 `deduced`），钉住它就跟滚动无关。
 *
 * 只在真的变了的时候发（滚动每帧都摆位置，不判一下就是每帧一个协议事件）。 */
static void view_pin_scale(struct vidow_view *view) {
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return;
	}
	/* X11 的缩放桥走 _XWAYLAND_GLOBAL_OUTPUT_SCALE，Xwayland 自己已经把
	 * 缓冲按那个倍数放大了；再发一遍 preferred_buffer_scale / fractional
	 * scale 会让它在这个倍数上**再乘一次**（2x 屏上变成 4x，用户报的
	 * 「Xwayland 窗口太大」）。X11 只发 enter / leave，不发倍率。 */
	if (view->kind == VIDOW_VIEW_X11) {
		return;
	}
	int32_t want = (int32_t)lround(view_scale(view));
	if (want < 1) {
		want = 1;
	}
	/* **只在值变化时发**（和 Hyprland 的 CFractionalScaleAddon::setScale 一致）：
	 *
	 *   if (m_scale && m_scale == scale) return;
	 *
	 * 之前这里改成了「每次 commit 都发」，理由是「Chromium 重建
	 * wp_fractional_scale_v1 对象收不到旧值」—— 那个理由站不住：协议实现本来
	 * 就该在客户端 get_fractional_scale 时补发当前值（Hyprland 的
	 * onGetFractionalScale 就是这么做的），wlroots 那份同样如此。
	 *
	 * 而每次 commit 都发是有代价的：wlr_surface_set_preferred_buffer_scale /
	 * wlr_fractional_scale_v1_notify_scale 都不去重，客户端每收一次事件就可能
	 * 重新分配缓冲 —— 实测 Electron 的 surface 每 6~12ms 在「声明 1 倍」和
	 * 「声明 2 倍」之间跳，正好等于每次 commit 的间隔。 */
	/* 根表面值变了才发协议事件；子表面仍然每帧走一遍（很便宜），这样
	 * 晚创建的子表面也能拿到当前倍率。值没变的表面在 pin_scale_surface
	 * 里直接跳过 —— 客户端不会每帧重算缓冲（见那里的注释）。 */
	bool changed = view->pref_scale != want;
	if (changed) {
		if (vidow_flip_on()) {
			vidow_flip_note("scale", view, want != 0);
		}
		view->pref_scale = want;
		wlr_surface_set_preferred_buffer_scale(surface, want);
		wlr_fractional_scale_v1_notify_scale(surface, (double)want);
	}
	struct pin_scale_ctx ctx = { .want = want };
	wlr_surface_for_each_surface(surface, pin_scale_surface, &ctx);
	if (changed) {
		/* 带上窗口编号（IPC 那个 id）：钉是在**第一次摆放**时发生的，那会儿
		 * 标题往往还没来 —— 日志按编号认人更稳（测试也按编号找这一行）。 */
		wlr_log(WLR_INFO, "surface %llu %s: 钉住 preferred_buffer_scale = %d"
			"（它那块屏的缩放，滚出视口 / 停到屏外也不跟着变）",
			(unsigned long long)view->id,
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			want);
	}
}

/* 表面进哪块输出：**只进它工作区那块**，别的全退掉，并且只发这一块的倍率。
 *
 * wlr_scene_surface 的 outputs_update 监听按「节点可见面积」发 enter +
 * preferred scale。节点跨在屏边界上时，隔壁 2 倍屏会赢；它只在输出几何变化
 * 时重算，我们 set_clip 改了也不重算 —— 补一次 leave 会被下一帧 enter 覆盖，
 * 客户端就在 1/2 倍之间反复重画。Hyprland / niri 都是自己按 monitor 发 enter
 * 和 fractional scale，不交给场景；这里照做（建表面时摘掉那个监听，见
 * view_takeover_surface_scale）。 */
static void view_surface_enter_one(struct wlr_surface *surface,
		int sx, int sy, void *data) {
	(void)sx;
	(void)sy;
	struct wlr_output *keep = data;
	struct wlr_surface_output *so, *tmp;
	wl_list_for_each_safe(so, tmp, &surface->current_outputs, link) {
		if (so->output != keep) {
			wlr_surface_send_leave(surface, so->output);
		}
	}
	wlr_surface_send_enter(surface, keep);
}

/* 拖拽跨屏：让表面**同时 enter 到它现在压着的那几块屏**。
 *
 * 这是「第一次拖到另一块屏一定看不见窗口」的根因那一层：表面只 enter 过归属屏，
 * 另一块屏的 wlr_scene 输出在合成时就没有这个表面的可见区域，窗口画过去也是空的
 * —— 裁剪、倍率、模糊全对也没用，那块屏压根没画。松手之后工作区真的搬过去，
 * 归属跟着变，窗口才「突然出现」（用户看到的就是「松手才出现」）。
 *
 * 只在拖拽期间走这条路：平时一块窗口只属于一块屏（view_surface_enter_one 那套
 * 「只留一块」是硬约束，见 view_clip_box 的注释）。拖完松手那一刻 owner 变了，
 * 会自己回到只留一块。 */
static void view_sync_drag_outputs(struct vidow_view *view,
		struct wlr_surface *surface) {
	struct vidow_server *server = view->server;
	if (server == NULL) {
		return;
	}
	struct wlr_box box = vidow_view_screen_box(view);
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_output *wlr_output = output->wlr_output;
		if (wlr_output == NULL || !wlr_output->enabled) {
			continue;
		}
		struct wlr_box obox;
		wlr_output_layout_get_box(server->output_layout, wlr_output, &obox);
		struct wlr_box hit;
		if (!wlr_box_intersection(&hit, &box, &obox)) {
			continue;
		}
		wlr_surface_send_enter(surface, wlr_output);
	}
}

static void view_sync_surface_outputs(struct vidow_view *view) {
	if (view->popup || !view->mapped) {
		return; /* 弹窗 / 还没上屏：各自由场景和自己的输出管 */
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return;
	}
	/* 拖拽中（平铺 / 浮窗都算）：表面要 enter 到它压着的每块屏，见上面
	 * view_sync_drag_outputs 的说明。 */
	if (view_drag_output(view) != NULL) {
		view_sync_drag_outputs(view, surface);
		return;
	}
	struct wlr_output *owner = vidow_view_owner_output(view);
	if (owner == NULL && view->mini != VIDOW_MINI_NONE) {
		owner = view->output; /* 小窗是屏级对象：没有工作区，按自己的 output */
	}
	if (owner == NULL || owner == view->surface_output) {
		vidow_portal_view_output(view);
		return;
	}
	view->surface_output = owner;
	wlr_surface_for_each_surface(surface, view_surface_enter_one, owner);
	view->pref_scale = 0;
	view_pin_scale(view);
	vidow_portal_view_output(view);
}

/* 摘掉场景那条「按可见面积发 enter + preferred scale」的监听。link 重新
 * 初始化成自环：wlr_scene_surface 销毁时无条件 wl_list_remove，自环上删自己
 * 是安全的（不会碰到已摘掉的邻居）。 */
static void view_takeover_surface_scale(struct vidow_view *view) {
	/* Popups are intentionally excluded from our workspace-driven enter/leave
	 * path. Keep scene output tracking for them, otherwise they never enter
	 * an output and Xwayland can leave their initial buffer unrefreshed. */
	if (view->scene_surface == NULL) {
		return;
	}
	if (view->popup) {
		/* Classification may change after association. Restore the original
		 * listener only when our normal-window path previously detached it. */
		struct wl_listener *listener =
			&view->scene_surface->WLR_PRIVATE.outputs_update;
		if (wl_list_empty(&listener->link)) {
			wl_signal_add(&view->scene_surface->buffer->events.outputs_update,
				listener);
		}
		return;
	}
	wl_list_remove(&view->scene_surface->WLR_PRIVATE.outputs_update.link);
	wl_list_init(&view->scene_surface->WLR_PRIVATE.outputs_update.link);
}

/* Redraw the view at its animated position/opacity. The tiled geometry itself
 * (view->geom) is never animated: clients are only ever configured with the
 * final size, so a tween can never race a resize. */
/* ------------------------------------------------ 视口外强制渲染（补帧回调） */


static bool view_alive(struct vidow_server *server, struct vidow_view *want);

/* 窗口的整棵表面树（含子表面 / 弹窗）都补一份帧回调。 */
struct pump_ctx {
	struct timespec *now;
	int surfaces;
};

static void pump_surface(struct wlr_surface *surface, int sx, int sy, void *data) {
	(void)sx;
	(void)sy;
	struct pump_ctx *ctx = data;
	/* 只有在等帧的表面才发，也只有它才计入 keep_going。
	 *
	 * 以前无条件 +1：滚出视口的窗口哪怕静止（没有 pending frame callback）
	 * 也会把 keep_going 顶成真，每帧 schedule_frame —— 静止桌面被推着出帧、
	 * GPU 空转。send_frame_done 对没有 callback 的表面本来就是 no-op，计数
	 * 也该跟着它走。 */
	if (wl_list_empty(&surface->current.frame_callback_list)) {
		return;
	}
	wlr_surface_send_frame_done(surface, ctx->now);
	ctx->surfaces++;
}

/* 数一下这棵 surface 树上还有多少个 frame callback 在等（只数，不发）。
 *
 * 用途只有一个：诊断「客户端 0fps / 卡死」时，一眼看出它到底是不是在等
 * frame_done。数出来 > 0 而输出又在正常出帧，那就是合成器漏发了；数出来 0
 * 说明客户端根本没请求下一帧，问题不在合成器这边。见 ipcstate.c 的
 * window 输出（frame_callbacks 字段）。 */
static void count_pending_frames(struct wlr_surface *surface, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	int *n = data;
	*n += wl_list_length(&surface->current.frame_callback_list);
}

int vidow_view_pending_frame_callbacks(const struct vidow_view *view) {
	if (view == NULL) {
		return 0;
	}
	struct wlr_surface *surface = vidow_view_surface(
		(struct vidow_view *)view);
	if (surface == NULL) {
		return 0;
	}
	int n = 0;
	wlr_surface_for_each_surface(surface, count_pending_frames, &n);
	return n;
}

/* 这个窗口的矩形和它自己那块屏有交集吗（都是布局坐标）。
 *
 * 为什么要问：wlr_scene 是一张**全局坐标**的场景图，每块屏的场景输出会画「落在
 * 自己那块坐标里的所有节点」。列往右滚出屏宽之后，它的坐标正好落进右边那块屏的
 * 框里 —— 于是就画到隔壁显示器上了（niri 不会：它每块屏各渲染自己那棵，屏外的
 * 自然被 framebuffer 裁掉）。所以这里按框判一次，越界的干脆不画。
 *
 * 比的是**这一帧画在哪儿**（几何 + 动画偏移，见 vidow_view_screen_box），不是布局
 * 目标位置：滚动 / 开窗动画里窗口是从屏边界上扫过去的，按目标位置判的话，滑回来的
 * 那一路（目标已经落在屏里、画面还在屏外）会被当成「在屏里」，越界的那一块就画到
 * 隔壁屏上了（用户报的「视口外的窗口回来的瞬间会显示在另一个显示器上」）。
 *
 * 还没摆放（几何是空的）和不知道归哪块屏的都当「画得到」：宁可多画一帧，也别把
 * 一个刚 map 的窗口藏没了。 */
bool vidow_view_in_output_box(struct vidow_server *server,
		const struct vidow_view *view) {
	/* 平铺窗口按**它那条列挂在哪块屏**判（跟滚动视口无关），浮动 / 弹窗
	 * 没有归属、按住址判。和 view_clip_box / view_home_output 同一个来源 ——
	 * 以前这里只读 view->output，热插拔 / 规则搬屏的中间态里它可能还指着
	 * 旧屏，于是停车位按旧屏算、裁剪按新屏算，窗口在视口外的那一帧被画到
	 * 隔壁显示器上。 */
	struct wlr_output *out = vidow_view_owner_output(view);
	if (out == NULL) {
		out = view->output;
	}
	if (out == NULL || view->geom.width <= 0 ||
			view->geom.height <= 0) {
		return true;
	}
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, out, &box);
	if (box.width < 1 || box.height < 1) {
		return true;
	}
	struct wlr_box at = vidow_view_screen_box(view);
	return at.x < box.x + box.width && at.x + at.width > box.x &&
		at.y < box.y + box.height && at.y + at.height > box.y;
}

/* **这个窗口现在是不是「正被手拖着」。**
 *
 * 两条路各置各的标志，判据必须同时认：
 *   - 浮窗 / X11 拖动走 input.c 那条抓取：view->dragging = true
 *     （server->grab_mode 是 MOVE / RESIZE）。
 *   - 平铺窗口拖拽走 drag.c：**不动 view->dragging**，只置
 *     drag_detached（见 tile_drag_activate）+ server->grab_view = view，
 *     grab_mode 是 TILE / RESIZE_TILE。
 *
 * 只看 view->dragging 就会漏掉平铺拖拽 —— 那正是用户 2026-10-27 报的
 * 「第一次拖拽过程会夹住窗口」：平铺窗口一越过屏边界就被 view_clip_box
 * 裁回原来那块屏，屏幕上就是停在交界处过不去。
 *
 * 改大小（RESIZE / RESIZE_TILE）不算：它不把窗口拖到别的屏上去，而且每帧
 * 换源缓冲尺寸，裁剪框跟着抖没有意义。 */
static bool view_is_being_dragged(const struct vidow_view *view) {
	if (view->dragging) {
		return view->server->grab_mode != VIDOW_GRAB_RESIZE &&
			view->server->grab_mode != VIDOW_GRAB_RESIZE_TILE;
	}
	if (view->server->grab_view == view) {
		if (view->server->grab_mode != VIDOW_GRAB_MOVE &&
				view->server->grab_mode != VIDOW_GRAB_TILE) {
			return false;
		}
		/* 平铺拖拽走完 16px 阈值才算真的拖起来（之前只是按着，
		 * 窗口还该老老实实待在自己的屏里）。 */
		return view->drag_detached || view->dragging;
	}
	return false;
}

/* 这块窗口现在该不该裁到自己那块屏里，裁的话边界是哪块屏的框。 */
bool view_clip_box(struct vidow_view *view, struct wlr_box *out) {
	struct vidow_server *server = view->server;

	/* 小窗（mini window，S3）：**强制裁到自己那块屏**。
	 *
	 * ⚠️ 这条和下面「浮动窗口一律不裁」是**相反**的语义，别合并：浮动窗口跨屏
	 * 是允许的（拖到一半正好骑在两块屏中间，裁了就会缺一块 —— test-mixdpi 钉着
	 * 这条），而小窗是**屏级对象**（像安卓的画中画），图标态还会按
	 * mini_icon_overflow 刻意探出屏边一点。不裁的话那半块会画到隔壁显示器上
	 * （场景图是全局坐标的，隔壁那块屏照画落在自己坐标里的所有节点）。
	 *
	 * 放在「浮动不裁」那条放行**之前**：小窗态 float_mode 是 FIXED，晚一步就
	 * 被上面那行 return false 放走了。 */
	if (view->mini != VIDOW_MINI_NONE) {
		if (!view->mapped || view->output == NULL ||
				view->geom.width <= 0 || view->geom.height <= 0) {
			return false;
		}
		if (vidow_overview_active(server) ||
				vidow_view_should_park(server, view)) {
			return false;
		}
		struct wlr_box mbox = {0};
		wlr_output_layout_get_box(server->output_layout, view->output, &mbox);
		if (mbox.width < 1 || mbox.height < 1) {
			return false;
		}
		/* 整块都在自己那块屏里就不裁：裁一次要把场景节点的显示范围改小，
		 * 白白多一次损伤和一次重烘（判据和下面那条一样，比的是这一帧画在
		 * 哪儿 —— 几何 + 动画偏移）。 */
		struct wlr_box mat = vidow_view_screen_box(view);
		struct wlr_box minter;
		if (wlr_box_intersection(&minter, &mat, &mbox) &&
				minter.width == mat.width && minter.height == mat.height) {
			return false;
		}
		*out = mbox;
		return true;
	}
	/* 有工作区的窗口平时一律裁到自己那块屏： 「窗口只在自己的工作区显示」
	 * 是硬约束。小窗和弹窗在上面 / 后面各自走自己的路。
	 *
	 * **但拖拽中的窗口是例外**（下面那段，用户 2026-10-23 报的「跨一半之后
	 * 突然被另一个屏幕裁剪了」）。 */
	if (!view->mapped || view->popup ||
			vidow_overview_active(server) ||
			view->geom.width <= 0 || view->geom.height <= 0) {
		return false;
	}
	/* **浮动窗口一律裁到自己那块屏**（2026-10-23 用户明确要求）。
	 *
	 * 以前这里对 float_mode != NONE 直接放过 —— 语义是「浮窗允许跨屏」，代价
	 * 是浮窗骑在两块屏中间时那半块会画到隔壁显示器上，而且两块屏的缩放不同时
	 * 那半块的模糊底衬只能选一块屏来采（一片横条）。用户的原话：「算了浮动
	 * 窗口也不要越界」。
	 *
	 * 小窗（mini）在上面已经单独处理，走的是自己那条更强的「强制裁」；这里只
	 * 剩下普通浮窗 / X11 的浮动窗口。判据和下面平铺那条完全一样，只是不参与
	 * 「拖拽中按画在哪块屏算」那一段 —— 浮窗**拖拽中**仍然按它此刻画在哪块屏
	 * 裁（view_drag_output），这样松手前对面那块屏本来就在画它，只是裁到那块
	 * 屏自己的工作区里。 */
	if (view->float_mode != VIDOW_FLOAT_NONE) {
		/* **拖拽中的浮窗不裁**（和平铺窗口下面那条同一个理由）。
		 *
		 * 用户 2026-10-27 报的「拖拽窗口到另一个显示器，第一次高分辨率拖
		 * 低分辨率，像是被窗口大小卡住一样过不去」就是这条：浮窗在拖拽时
		 * 仍然被裁到「当前那块屏」，而下面 view_drag_output() 又按光标换屏，
		 * 于是窗口越过边界那一刻被切掉，屏幕上就是「停在屏交界过不去」。
		 *
		 * 拖拽期间 view_sync_drag_outputs() 已经让表面 enter 到所有相交的
		 * 屏（没 enter 的屏根本不画它），所以这里再裁只剩「切掉一半」这一个
		 * 效果 —— 跨屏拖拽本来就该两块屏各画一半、拼起来是完整的窗口。
		 * 松手之后归到新工作区，裁剪自然恢复，不会留下永久越界的窗口。
		 *
		 * 只豁免移动：改大小（RESIZE）照旧裁。 */
		if (view_is_being_dragged(view)) {
			return false;
		}
		struct wlr_output *fout = vidow_view_display_output(view);
		if (fout == NULL) {
			fout = view_home_output(view);
		}
		if (fout == NULL) {
			fout = wlr_output_layout_output_at(server->output_layout,
				view->geom.x + view->geom.width / 2,
				view->geom.y + view->geom.height / 2);
		}
		if (fout == NULL) {
			return false;
		}
		struct wlr_box fbox = {0};
		wlr_output_layout_get_box(server->output_layout, fout, &fbox);
		if (fbox.width < 1 || fbox.height < 1) {
			return false;
		}
		struct wlr_box fat = vidow_view_screen_box(view);
		struct wlr_box fhit;
		if (wlr_box_intersection(&fhit, &fat, &fbox) &&
				fhit.width == fat.width && fhit.height == fat.height) {
			return false; /* 整块都在自己那块屏里：不用裁 */
		}
		*out = fbox;
		return true;
	}
	/* **拖拽中的窗口不裁**：窗口跟着光标越过边界时，它真的已经（也应该）
	 * 同时出现在两块屏上 —— 裁到自己原来那块屏就把跨过去的那半块整块切掉，
	 * 屏幕上就是「拖到一半突然少一半」。旧注释里写的「正在拖的窗口也不例外」
	 * 正是这个 bug 的来源。
	 *
	 * 和浮动窗口那条同理（浮动窗口跨屏本来就允许，裁了会缺一块，
	 * test-mixdpi 钉着那条）；区别只在于平铺窗口平时确实要「只在自己的
	 * 工作区显示」，而**拖拽是那个约束的临时例外**：松手之后窗口归到新
	 * 工作区，裁剪自然恢复，不会留下永久越界的窗口。
	 *
	 * 判据用 view_drag_output()（拖着 = view->dragging 或 grab_view 是
	 * 自己），和 view_scale 取倍率、view_clip_box 取屏框、模糊底衬归属
	 * 三处同一个来源 —— 三处不一致就会出「倍率和底衬对不上」。 */
	/* **拖拽窗口照裁**（2026-10-23 改）：以前这里对拖拽豁免过，理由是
	 * 「拖到一半被裁掉一半」——那是没做按屏绘制时的权宜之计，代价是整个
	 * 跨屏模型都成了「一个窗口横跨两块屏」，模糊底衬只能选一块屏来采，
	 * 另一半就成了横条纹。
	 *
	 * 照 Hyprland：没有「跨屏的窗口」。每块屏各自渲染一遍、各自裁到自己
	 * 那块屏（Renderer.cpp 的 renderMonitor 逐屏循环，窗口和屏的交集非空就
	 * 画一次，模煳用那一屏自己的 m_blurFB）。两块屏各画一半，拼起来就是
	 * 「看起来跨屏」，而每一半都用自己那块屏的底衬 —— 这才是对的。
	 *
	 * 松手之后窗口归到新工作区，裁剪收紧，不会留下永久越界的窗口。 */
	/* 弹出物先不裁：它的位置是相对父窗口算的，越界那点（菜单探出去一条）
	 * 先留着。停到停车位上的窗口也不裁：它本来就不在任何一块屏里（不用裁）。
	 * 判据现算（跟摆位那份同一份，见 vidow_view_should_park）：只看存下来的
	 * 标志会在「这一帧刚从停车位摆回来、还没走 arrange」的时候落后一帧。
	 *
	 * **有人录屏 / 预览也照裁**（以前这儿会跳过）：按窗口录屏的截图源现在渲染的
	 * 是这个窗口自己的一份镜像（src/wincapture.c），跟这块节点裁没裁没关系 ——
	 * 不裁的话，跨屏那半块会一直画在隔壁显示器上（用户报的「开了 obs 就完全没
	 * 藏住」）。
	 *
	 * ⚠️ 停车位**不再**在这里放行（2026-10-23 用户明确要求）：平铺窗口一律裁到
	 * 自己那块屏，只有**拖拽中**和**浮动**窗口可以越界。以前停在停车位上就
	 * return false，等于把「该不该画」和「该不该裁」绑成一件事 —— 而停车位到底
	 * 摆没摆、摆到哪儿还要看它那一帧的动画状态，于是同一个位置会在「裁」和
	 * 「不裁」之间来回翻（日志里就是同一个窗口一秒内先「裁到自己那块屏里」、
	 * 再「不裁（屏框 0,0 0x0）」）。现在两条判据分开：画不画由 workspace /
	 * view_anim_place 决定，裁哪儿只按归属。停车位本来就在所有输出框外面，
	 * 裁了也同一个像素都不画，不会多花一份烘图。 */
	/* **屏框拿不到时不能直接放行**：放行 = 整块画到隔壁屏上，wlroots 会给
	 * 这个表面发 enter + 隔壁屏的 preferred_buffer_scale，Chromium 收到 2 倍
	 * 之后就在 1/2 倍之间反复切（实测 15ms 一轮，内部页面一直重排 —— 用户
	 * 报的「浏览器还是会一直变化内容大小」）。view->output 在切工作区 / 摆放
	 * 动画里会短暂失效，这时改用工作区所属的那块屏 —— 和 view_scale 用的是
	 * 同一个归属，稳定。 */
	/* **拖拽中按它此刻画在哪块屏**：窗口跟着光标越过边界之后，它真的已经画在
	 * 对面那块屏上了（场景图是全局坐标），按旧归属屏裁就会把越界那半块整块切掉
	 * —— 屏幕上就是「拖到一定距离窗口就没了」（用户 2026-10-23 报的）。倍率、
	 * 裁剪、模糊底衬三处必须是同一个判据，见 view_drag_output()。 */
	/* **拖动中的窗口不裁**（用户 2026-10-25：「拖拽跨屏之后就能正常两个屏幕
	 * 渲染，但跨屏之前不行」）。
	 *
	 * view_drag_output() 按「重叠面积最大」选屏。拖到一半（窗口 70% 还在原屏）
	 * 它返回原屏，下面就把窗口裁到原屏框里 —— 伸出原屏的那一条被切掉，看起来
	 * 就是「跨屏之前另一块屏上没有内容」。等窗口大半越过去，判据翻到新屏，
	 * 整块又基本落在新屏框内，于是「跨屏之后正常」。
	 *
	 * 但拖拽期间 view_sync_drag_outputs() 已经让表面 enter 到**所有相交的屏**
	 * 了（那是另一条硬约束：表面没 enter 过的屏根本不画它）。既然两块屏本来
	 * 都在画，这里再裁就只剩「切掉一半」这一个效果 —— 和那条 enter 逻辑直接
	 * 打架。跨屏拖拽本来就该两块屏各画一半、拼起来是完整的窗口（Hyprland 的
	 * 逐屏渲染就是这个效果，每半块用自己那块屏的底衬）。
	 *
	 * 只豁免**移动**：改大小（RESIZE）不拖出去，照旧裁；而且它每帧换源缓冲
	 * 尺寸，裁剪框跟着抖没有意义。松手之后归到新工作区，裁剪自然恢复。
	 *
	 * 注意这和上面「浮窗拖拽中仍按 view_drag_output 裁」那段不冲突 —— 浮窗
	 * 走的是上面那条 return false / fbox 分支，根本到不了这里；这条只对
	 * 平铺窗口的拖拽生效。 */
	if (view_is_being_dragged(view)) {
		return false;
	}
	struct wlr_output *clip_out = view_drag_output(view);
	if (clip_out == NULL) {
		clip_out = view_home_output(view);
	}
	if (clip_out == NULL) {
		/* **别放行**：放行 = 命中测试拿不到屏框，窗口贴着屏幕边缘时
		 * 「右半边点不到」（用户实测：同一个窗口、同一个位置，一秒内
		 * 先后出现「裁到自己那块屏里」和「不裁（屏框 0,0 0x0）」，
		 * 而窗口右边正好压到 3832 / 屏幕到 3839）。
		 *
		 * view_home_output() 在切工作区、摆放动画的中间帧会短暂返回
		 * NULL（它要的 ws->output / view->output 那一刻都还没填上）。
		 * 这时按**几何框**找它落在哪块屏上兜底 —— 布局坐标系是唯一
		 * 的真值，和窗口此刻画在哪儿一致，正好是裁切要的那块。 */
		clip_out = wlr_output_layout_output_at(server->output_layout,
			view->geom.x + view->geom.width / 2,
			view->geom.y + view->geom.height / 2);
	}
	if (clip_out == NULL) {
		return false; /* 真的哪块屏都不沾（比如停在停车位上）：没地方可裁 */
	}
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, clip_out, &box);
	if (box.width < 1 || box.height < 1) {
		/* 屏框查询拿不到的时候**不能放行**：放行 = 整块画到隔壁屏上（场景图是
		 * 全局坐标）。实测这块屏刚 hotplug / 布局重排的那几帧会查到 0x0，窗口
		 * 于是几十毫秒里一直「不裁」，跨屏那一半直接铺到隔壁显示器上。回退到
		 * 归属那块屏的框；连归属都查不到才真的没法裁。 */
		struct wlr_output *owner = vidow_view_owner_output(view);
		if (owner != NULL && owner != clip_out) {
			wlr_output_layout_get_box(server->output_layout, owner, &box);
		}
		if (box.width < 1 || box.height < 1) {
			return false;
		}
	}
	/* 窗口**本体**整块都在自己那块屏里就不裁（比的是这一帧画在哪儿，动画偏移
	 * 算在内 —— 见 vidow_view_in_output_box 上面那段）。装饰的阴影内边距那点
	 * 越界（窗口贴着屏边时探出去十几个像素）先留着：裁它要把场景节点的显示尺寸
	 * 改小，而窗口截图源（portal 的按窗口录屏）盯着这棵树，节点一变形它就
	 * 重发一次约束 —— 客户端那边正好在等第一帧，撞上「session already has a
	 * frame object」直接断线（踩过：test-force-render.sh 的录屏那节）。窗口
	 * 本体越界才是要修的那个 bug，那一档照裁。 */
	struct wlr_box at = vidow_view_screen_box(view);
	struct wlr_box inter;
	if (wlr_box_intersection(&inter, &at, &box) &&
			inter.width == at.width && inter.height == at.height) {
		return false;
	}
	*out = box;
	return true;
}

/* 表面树在场景里挂的那个节点（wlroots 的 set_clip 要的就是它）。 */
static struct wlr_scene_node *view_surface_tree_node(const struct vidow_view *view) {
	if (view->scene_surface == NULL || view->scene_surface->buffer == NULL ||
			view->scene_surface->buffer->node.parent == NULL) {
		return NULL;
	}
	return &view->scene_surface->buffer->node.parent->node;
}

/* 抄一份「客户端声明的有效源区域」（缓冲像素）到 view->fx.content_box。
 *
 * 为什么要有这一份：越界裁剪走的是 wlroots 的
 * wlr_scene_subsurface_tree_set_clip()，而它是**改写节点的 src_box** 实现的
 * （裁完剩下的那一小条会变成整个 src_box）。装饰采源缓冲的哪一块照的正是这一份
 * （见 fx_geo_tex_map）—— 直接读那个字段，就会把那一小条当成整块有效内容、
 * 拉伸铺满整块窗口（用户报的「窗口只有一点在视口里时表面横向拉伸变扁、黑边随
 * 在视口里的范围大小变」）。
 *
 * 抄之前先把上一刀撤掉：wlroots 会按客户端声明的 viewport / 缓冲重算那一次
 * （它自己就是这么维护这个字段的），抄完再补回去。裁着的时候每来一次 commit
 * 抄一遍 —— 客户端改 viewport（chromium 那类）之后不用等裁剪自己变。 */
static void view_snapshot_content_box(struct vidow_view *view) {
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return;
	}
	/* **直接问 surface**，别再「先解裁剪 → 读 src_box → 再裁回去」。
	 *
	 * 解裁剪那一瞬间，场景节点的可见区域从「裁剩下的 4px」变回整块 949px：
	 * 它跨在隔壁那块 2 倍屏上，wlr_scene 立刻给表面发 enter + 2 倍；裁回去
	 * 又发 leave + 1 倍。客户端每 commit 一次就走一轮，屏幕上就是倍率 1/2
	 * 反复跳（实测 8ms 一轮）。wlr_surface_get_buffer_source_box() 读的是
	 * 客户端声明的 viewport / 缓冲，跟场景节点裁没裁无关 —— 要的就是这份
	 * 「没裁过」的有效区。 */
	wlr_surface_get_buffer_source_box(surface, &view->fx.content_box);
}

/* 缩放节点的越界裁剪（S3 修）：把表面那份内容节点裁成「这一帧落在屏里那一块」。
 *
 * 为什么不能借 wlroots 的 wlr_scene_subsurface_tree_set_clip()：那套是给
 * **1:1 逻辑坐标**的窗口准备的（内部把 clip 当节点位置用、src_box 按表面尺寸
 * 比例缩、dst 不动，见 types/scene/subsurface_tree.c 的
 * subsurface_tree_reconfigure_clip + types/scene/surface.c 的 src_box 换算）。
 * 缩放态（小窗保留原生 buffer：图标 / 迷你 / 自由小窗，以及过渡动画那几帧）
 * 下它三件事一起错：内容挪到错的地方、剩下那一小条拉伸铺满整块 dst、而且
 * dst 不缩 → 越界那半块照画到隔壁屏上（实测 16×64 条带 832 像素）。
 *
 * 这里自己裁：**源框取可见那一块、目标框也取可见那一块**，缩放比一个像素都不变，
 * 框外那部分根本不画。缓冲像素 ↔ 场景像素按「整块有效源区对应整块 dst」换算
 * （整块源区从 wlr_surface_get_buffer_source_box() 拿，客户端声明的 viewport 也
 * 算在里面）。
 *
 * 返回 true = 这一档接管了（调用方别再走 wlroots 那套）；false = 没接管（不是
 * 能安全处理的缩放表面：没有缓冲 / 旋转过 / 拿不到源区），退回原路。 */
static bool view_scaled_crop(struct vidow_view *view, const struct wlr_box *clip) {
	struct wlr_scene_buffer *sb = view->scene_surface != NULL ?
		view->scene_surface->buffer : NULL;
	struct wlr_surface *surface = vidow_view_surface(view);
	if (sb == NULL || sb->buffer == NULL || surface == NULL ||
			sb->dst_width <= 0 || sb->dst_height <= 0 ||
			surface->current.transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		return false;
	}
	int cx = 0, cy = 0;
	wlr_scene_node_coords(&sb->node, &cx, &cy);
	struct wlr_box cbox = {.x = cx, .y = cy,
		.width = sb->dst_width, .height = sb->dst_height};
	struct wlr_box vis;
	if (!wlr_box_intersection(&vis, &cbox, clip)) {
		/* 这一帧整个身子都在自己那块屏外：一个像素都不该画（场景图是全局
		 * 坐标，画了就是画到隔壁屏上）。甩到所有输出框外面去 —— 和 fx 那边
		 * 同一招（src_box 空着 wlroots 会当成「整块」，那是另一个意思）。 */
		wlr_scene_buffer_set_source_box(sb, NULL);
		wlr_scene_node_set_position(&sb->node,
			sb->node.x - cx - 1000000, sb->node.y - cy - 1000000);
		return true;
	}
	if (vis.width == cbox.width && vis.height == cbox.height) {
		wlr_scene_buffer_set_source_box(sb, NULL); /* 可见那块 = 整块：不用裁 */
		return true;
	}
	struct wlr_fbox full = {0};
	wlr_surface_get_buffer_source_box(surface, &full);
	if (full.width <= 0.0 || full.height <= 0.0) {
		return false;
	}
	double kx = (double)full.width / (double)cbox.width;
	double ky = (double)full.height / (double)cbox.height;
	struct wlr_fbox src = {
		.x = full.x + (double)(vis.x - cx) * kx,
		.y = full.y + (double)(vis.y - cy) * ky,
		.width = (double)vis.width * kx,
		.height = (double)vis.height * ky,
	};
	/* 裁之前先抄一份没裁过的有效区：装饰管线（fx）采源缓冲的哪一块照的是
	 * view->fx.content_box 那一份，别让它认成裁剩下的一小条（187 那个坑）。 */
	wlr_scene_buffer_set_source_box(sb, NULL);
	view_snapshot_content_box(view);
	wlr_scene_buffer_set_source_box(sb, &src);
	wlr_scene_node_set_position(&sb->node,
		sb->node.x + (vis.x - cx), sb->node.y + (vis.y - cy));
	wlr_scene_buffer_set_dest_size(sb, vis.width, vis.height);
	return true;
}

static void view_apply_clip_impl(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	struct wlr_box box = {0};
	bool on = view_clip_box(view, &box);
	bool was_on = view->fx.clip_on;
	if (on != view->fx.clip_on || (on && !wlr_box_equal(&box, &view->fx.clip))) {
		/* 裁掉的 / 放出来的那一块得重画。场景那边只按节点几何记损伤，我们
		 * 裁的是同一块节点里的显示范围，它看不见 —— 这里自己补一笔。 */
		vidow_backdrop_damage_view_rect(view, NULL);
	}
	if (vidow_flip_on()) {
		vidow_flip_note("clip", view, on);
	}
	view->fx.clip_on = on;
	view->fx.clip = box;
	/* overview：**只裁画**（fx 那一份离屏缓冲），不裁表面树 ——
	 * wlr_scene_subsurface_tree_set_clip 连**输入**一起裁，裁了预览就命不中
	 * （test-overview 的「点某个窗口 = 选中它」当场红）。
	 *
	 * 以前 overview 是整条放行（不裁），于是条带比屏宽的那些列直接画到隔壁
	 * 显示器上 —— 用户：「overview 窗口太多会跨屏」。裁到 overview 自己那块屏
	 * 的工作区（ov->out_* 就是它的工作区，布局坐标，和 fx->clip 同一坐标系）。 */
	if (vidow_overview_active(view->server) &&
			view->mini == VIDOW_MINI_NONE && view->mapped) {
		/* ⚠️ 裁到**显示器**，不是 ov->out_×（那是工作区 = 屏框扣掉 panel /
		 * gap 之后的可用区）。用户要的语义：「工作区内窗口可以溢出工作区，
		 * 但不会溢出显示器」—— 窗口探进 panel 那一条属于显示器，应该照常
		 * 显示，拿工作区当裁剪框就把它切掉了。 */
		/* 用**窗口自己所在那块屏**的框。以前固定用 overview.output（进来时
		 * 聚焦的那块），别的屏上的窗口就被裁到主屏那块矩形里 —— 越界部分全
		 * 被切掉，看起来就是非激活显示器没内容。 */
		struct wlr_box full = {0};
		if (view->output != NULL) {
			wlr_output_layout_get_box(view->server->output_layout,
				view->output, &full);
		}
		if (full.width > 0 && full.height > 0) {
			view->fx.clip_on = true;
			view->fx.clip = full;
			return; /* 表面树那一份不裁：保住命中测试 */
		}
	}
	if (on != was_on) {
		/* 这是个**状态**（跟「藏起 / 显示」一样）：裁不裁决定客户端被叫去按哪块屏
		 * 的缩放渲染（wlroots 的 wlr_scene_surface 按「可见面积最大的那块输出」
		 * 发 preferred_buffer_scale），日志里要看得见。只在翻转时记一行。 */
		wlr_log(WLR_INFO, "clip: %s %s（屏框 %d,%d %dx%d，窗口 %d,%d %dx%d）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			on ? "裁到自己那块屏里" : "不裁", box.x, box.y, box.width,
			box.height, view->geom.x, view->geom.y, view->geom.width,
			view->geom.height);
	}

	/* 没装饰（着色器也关着）的时候，窗口就是客户端表面自己在画 —— 那一份也得
	 * 裁。
	 *
	 * 这一份要调 wlroots 的 wlr_scene_subsurface_tree_set_clip()，它**只认
	 * subsurface 树**，别的树上找不到就直接 assert（`found` failed，vidow 当场
	 * abort，踩过两次：X11 的对话框窗口、X11 的弹出物）。所以只给 Wayland 的
	 * xdg toplevel —— X11 / 弹出物那两份靠 fx 那块离屏缓冲（默认配置装饰开着，
	 * 窗口画的就是它）。
	 *
	 * 裁的是**显示**：截图源抓的是窗口自己那份镜像，不在这一棵树上。 */
	if (view->xdg_toplevel == NULL) {
		/* X11 / 弹出物：表面那一份裁不了（见上），记成「没裁过」—— 免得
		 * 以后哪天真拿到一棵能裁的树时，被一个过期的局部框挡掉。 */
		view->fx.clip_local = (struct wlr_box){0};
		return;
	}
	/* 这一帧画在哪儿（几何 + 动画偏移）：表面树挂在那个位置上。 */
	struct wlr_box at = vidow_view_screen_box(view);
	struct wlr_scene_node *node = view_surface_tree_node(view);
	if (node == NULL) {
		view->fx.clip_local = (struct wlr_box){0};
		return;
	}
	/* 装饰管线（fx）画这个窗口的时候，表面那一份是隐藏的（0 透明度，见
	 * view_anim_bake 里那次 set_opacity）—— 但**照样得裁**。
	 *
	 * 表面这一份的裁剪不只是"别把内容画到隔壁屏上"：wlroots 的
	 * wlr_scene_surface 会替我们发 preferred_buffer_scale（客户端按几倍渲染），
	 * 判据是「**可见面积最大**的那块输出的缩放」（见 wlr_scene.h 里
	 * wlr_scene_surface_create 的说明）。窗口滚出视口之后它的框跨在隔壁屏上，
	 * 不裁的话那边算出来的可见面积最大 —— 客户端就被叫去按隔壁那块屏的缩放
	 * 重画：2 倍屏上录 / 预览这个窗口，它一直按 1 倍糊着，滚回视口才清楚
	 * （用户报的「窗口滚动出去后，又按照另一个屏幕的缩放变化了」）。
	 *
	 * 上一版这里是「fx 开着就让开」—— 因为 set_clip 改的是 src_box，而装饰采源
	 * 缓冲的哪一块照的正是那个字段（fx_geo_tex_map），一裁就把裁剩下的一小条
	 * 拉伸铺满整块窗口（187）。现在那边改成认 view->fx.content_box（没裁过的
	 * 那一版，见 view_snapshot_content_box），裁剪和装饰就互不干扰了。 */
	if (on) {
		/* 缩放态（迷你 / 图标 / 保留原生 buffer 的自由小窗，也包括过渡动画那几
		 * 帧）**不走 wlroots 那套 clip**：它是「1:1 逻辑坐标」语义 ——
		 * types/scene/subsurface_tree.c 里把 clip 直接当节点位置用
		 * （`set_position(clip.x, clip.y)`）、src_box 只按**表面尺寸**比例缩，
		 * dst 一个像素都不动。缩放态下这三件事一起错：① 内容被挪到错的地方；
		 * ② 剩下那一小条被拉伸铺满整块 dst；③ 越界那半块**照画**（dst 没缩、
		 * 节点还在原处）—— 实测贴右边的图标漏到隔壁屏那 16×64 条带里 832 个
		 * 像素。这一档自己裁，见 view_scaled_crop()。 */
		if (vidow_view_hit_scale(view) != 1.0 &&
				view->mini != VIDOW_MINI_NONE &&
				view_scaled_crop(view, &box)) {
			view->fx.clip_local = (struct wlr_box){0};
			return;
		}
		/* 表面局部坐标（逻辑单位，wlroots 自己乘缓冲倍率）。原点取**这一帧
		 * 画在哪儿**（几何 + 动画偏移）：表面树挂在那个位置上，减布局目标
		 * 位置的话，滚动动画里裁出来的那块会跟着偏移。位置变了就得重调一次
		 * —— 这个函数每帧都跑（重烘那一帧开头），别只按 clip_on 判断。
		 *
		 * ⚠️ 必须**先和窗口自己的框求交**，再减 at：直接把整块屏框
		 * （box = 0,0 1280x720）减 at 会得到负数 x / 屏宽的宽（例如窗口在
		 * x=1000 时 local = -1000,0 1280x720），wlroots 拿它当 dst 尺寸，
		 * 表面就被摆到屏原点、宽 1280 —— 逻辑坐标上等于把窗口铺满整块屏，
		 * 右缘正好压在隔壁屏上，隔壁屏就 enter + 2 倍。用户报的「窗口超出
		 * 屏幕就碰到隔壁显示器的工作区、缩放反复跳」就是这一条。 */
		struct wlr_box visible;
		if (!wlr_box_intersection(&visible, &box, &at)) {
			/* 一点交集都没有：停车位那条路会把它挪走，这里别留半块。 */
			view->fx.clip_local = (struct wlr_box){0};
			wlr_scene_subsurface_tree_set_clip(node, NULL);
			return;
		}
		struct wlr_box local = {
			.x = visible.x - at.x, .y = visible.y - at.y,
			.width = visible.width, .height = visible.height,
		};
		if (!wlr_box_equal(&local, &view->fx.clip_local)) {
			/* 下刀之前先抄一份没裁过的有效区（撤销上一刀 → wlroots 重算
			 * src_box → 读出来 → 再补回去），装饰那边只认这一份。 */
			view_snapshot_content_box(view);
			view->fx.clip_local = local;
			wlr_scene_subsurface_tree_set_clip(node, &local);

		}
	} else if (view->fx.clip_local.width > 0) {
		view->fx.clip_local = (struct wlr_box){0};
		wlr_scene_subsurface_tree_set_clip(node, NULL);
	}
}

/* 裁剪状态一变，**强制**再钉一次客户端的 buffer_scale。
 *
 * wlroots 的 wlr_scene_surface 只在输出几何变化时重算「进了哪些输出」；我们
 * 用 set_clip 把节点缩到屏里，它不重算，之前因为跨到隔壁 2 倍屏而收到的 enter
 * 和 2 倍 preferred scale 就一直留着（客户端每 commit 一次都按 2 倍交，屏幕上
 * 1/2 反复跳）。裁剪改完主动发一次当前归属屏的倍率，把客户端拉回来。
 *
 * pref_scale 先清 0：view_pin_scale 平时只在值变化时发，这里要的就是「没变也
 * 再发一次」。 */
/* 让这个表面（和它的子表面）退出**不属于它工作区**的输出。
 *
 * wlroots 的 wlr_scene_surface 只在输出几何变化时重算「进了哪些输出」；我们用
 * set_clip 把节点缩进屏里，它不重算 —— 之前跨到隔壁 2 倍屏收到的 enter 一直留着，
 * 客户端就按 2 倍画（wlr_surface 的 preferred scale 取 current_outputs 里最大的）。
 * niri 也是自己管 enter/leave（workspace.rs 的 enter_output_for_window /
 * output_leave），不交给场景。 */
void vidow_view_apply_clip(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	bool was_on = view->fx.clip_on;
	struct wlr_box was_clip = view->fx.clip;
	if (getenv("VIDOW_CLIP_TRACE") != NULL) {
		struct wlr_box cb = {0};
		bool con = view_clip_box(view, &cb);
		struct wlr_output *dragged = view_drag_output(view);
		struct wlr_output *home = view_home_output(view);
		struct wlr_output *owner = vidow_view_owner_output(view);
		struct wlr_box at = vidow_view_screen_box(view);
		wlr_log(WLR_INFO,
			"cliptrace view=%p owner=%s home=%s drag=%s clip_on=%d "
			"box=%d,%d %dx%d screen_box=%d,%d %dx%d floating=%d "
			"dragging=%d grab=%d",
			(void *)view,
			owner != NULL ? owner->name : "-",
			home != NULL ? home->name : "-",
			dragged != NULL ? dragged->name : "-",
			con ? 1 : 0, cb.x, cb.y, cb.width, cb.height,
			at.x, at.y, at.width, at.height,
			view->float_mode != VIDOW_FLOAT_NONE ? 1 : 0,
			view->dragging ? 1 : 0,
			view->server->grab_view == view ? 1 : 0);
	}
	view_apply_clip_impl(view);
	bool clip_changed = view->fx.clip_on != was_on ||
		!wlr_box_equal(&view->fx.clip, &was_clip);
	if (clip_changed) {
		view->pref_scale = 0;
	}
	view_sync_surface_outputs(view);
	view_pin_scale(view);
}

/* 补帧判定的迟滞帧数（见 vidow_view_render_pump 里的说明）。
 * view_on_screen() 依赖 view->geom / 工作区可见性，arrange 期间这些值会逐帧
 * 变化；把它当瞬时真值会让 render_pumping 每帧来回翻，每翻一次喂客户端一帧
 * frame_done。连续这么多帧结论一致才承认状态变化。 */
#define PUMP_HYSTERESIS 3

/* 这个窗口现在画得到吗：所在工作区显示着 **而且** 它的矩形和这块屏有交集。
 * 画得到就不该补帧（wlr_scene 已经给过回调了，再补一次会让客户端多画一遍）。 */
static bool view_on_screen(struct vidow_server *server,
		struct vidow_output *output, struct vidow_view *view) {
	if (!vidow_workspace_view_visible(server, view)) {
		return false;
	}
	/* overview 里所有窗口都被缩到画面里了，这时它们都画得到。 */
	if (vidow_overview_active(server)) {
		return true;
	}
	/* **判据是「它的几何落在哪块屏的框里」，不是「它是不是被指派给 output」。**
	 *
	 * 这个函数的语义是「它到底有没有被画到屏幕上」，只用来决定要不要补帧。
	 * 以前它只拿**传进来的那块** output 的框去比 view->geom：
	 *
	 *     wlr_output_layout_get_box(..., output->wlr_output, &box);
	 *     return 相交(view->geom, box);
	 *
	 * 而调用方（vidow_view_render_pump）是按 `view->output == output` 分发
	 * 的，两者本该一致 —— 但 view->output 和 view->geom 会短暂（甚至长期）
	 * 分叉：拖动 / 布局重排 / 换工作区之后，geom 已经是新位置，output 还
	 * 停在旧屏上。于是 Steam 明明画在 DP-2 上（点击坐标 2586,92 都命中它），
	 * view->output 却是 DP-2、geom 却是 0,0（DP-3 的坐标），矩形不相交
	 * → 判成「它滚出视口了」→ 每 5~6ms 强制补帧一次，整机卡。
	 *
	 * 用户实测的旁证：**滚出去一次再滚回来就不卡了** —— 那一下会重算 geom
	 * 和 output，两者对齐，判据立刻恢复正常。
	 *
	 * 所以这里改成问「有没有落在任意一块屏里」。判据本身还是几何相交，
	 * 只是不再相信 view->output 这一个字段。 */
	bool on = false;
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_box box = {0};
		wlr_output_layout_get_box(server->output_layout, o->wlr_output, &box);
		if (box.width < 1 || box.height < 1) {
			continue;
		}
		if (view->geom.x < box.x + box.width &&
				view->geom.x + view->geom.width > box.x &&
				view->geom.y < box.y + box.height &&
				view->geom.y + view->geom.height > box.y) {
			on = true;
			break;
		}
	}
	(void)output;
	return on;
}

/* **每帧给 X11 视图无条件发一次 frame_done。**
 *
 * 这是照 Hyprland 的做法补上的（`/tmp/hypr-src` 的 src/output/MonitorFrameScheduler.cpp）：
 * 它的帧事件里统一调 renderMonitor()，frame_done 由 aquamarine/wlroots 的标准
 * 路径发，**不需要任何「补帧预算」**。Vidow 以前没有这一步，靠一套
 * 「x11_want_frames 欠帧预算」去凑 —— 那套要穿过三道闸（在屏闸 / 迟滞闸 /
 * 节流闸），任何一道判错就是黑屏（预算被扣光但没发出去）或者卡顿（闸门全开、
 * 180Hz 空转）。两个症状来回摆了很多轮，2026-10-27 把那套预算整个删了。
 *
 * 为什么不能只靠 wlr_scene_output_send_frame_done()：
 * 它只给**场景认为可见**的节点发回调。而 Vidow 的 FX 管线把真实表面的
 * opacity 强制设成 0（内容由 decor 烘焙图来画，见 fx.c），wlroots 认为
 * 这块表面「看不见」，于是跳过它 —— XWayland 就永远等不到 frame callback，
 * 交一帧空白之后彻底停住。实测：Steam 重开后 40 秒只有 1 次 commit。
 *
 * 所以这里不看可见性、不看工作区、不看是否在屏上：**这块屏画完一帧，就给
 * 所有归属它的 X11 视图发一次**。语义和 Wayland 客户端收到 wl_callback.done
 * 完全一致 —— 「我用过你的画面了，你可以交下一帧」。
 *
 * 唯一保留的判据是 `view->output == output`：frame_done 应该按屏发，一块屏
 * 一帧一次，不能两块屏各发一次（那会让 X11 客户端被催着跑双倍帧率）。 */
/* frame_done 的**最小间隔**：这块屏的刷新周期，减去一点容差。
 *
 * 为什么要减：原来的判据是「间隔 < 1/刷新率 就整帧不发」，**零容差**。而渲染
 * 循环的一帧本来就是围绕刷新周期抖的 —— 160Hz 屏预算 6.25ms，客户端跑
 * 148fps（6.76ms/帧），只要某一帧早到 0.1ms，这一帧的 frame_done 就整个丢掉：
 * 客户端多等一拍、下一帧连收两个，屏幕上就是「前后两帧反复拉扯」（实测每秒
 * 丢 12 个 = 160−148，正好是用户看到的抖动频率）。
 *
 * 容差取周期的 1/8、上限 2ms：160Hz → 5.47ms（上限约 183fps），60Hz → 14.6ms
 * （上限约 68fps）。既保住「别被客户端顶成 300fps 空转」那条初衷，又不会因为
 * 零点几毫秒的调度抖动漏帧。 */
static int64_t fd_min_interval_ns(const struct wlr_output *wlr_output) {
	int refresh_mhz = wlr_output != NULL ? wlr_output->refresh : 0;
	int refresh_hz = refresh_mhz > 0 ? refresh_mhz / 1000 : 60;
	if (refresh_hz <= 0) {
		refresh_hz = 60;
	}
	int64_t budget = 1000000000LL / refresh_hz;
	int64_t slack = budget / 8;
	if (slack > 2000000LL) {
		slack = 2000000LL;
	}
	return budget - slack;
}

void vidow_views_send_x11_frame_done(struct vidow_server *server,
		struct vidow_output *output) {
	if (server == NULL || output == NULL) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	/* **按这块屏的刷新率节流**（见 server.h 的 last_x11_fd_ns 说明）。
	 *
	 * 这一句以前是无条件每帧发的：XWayland 客户端拿到 frame callback 就立刻
	 * 画下一帧 → commit → 我们渲染 → 又发回调 → 自激。静止桌面实测 300fps、
	 * 损伤 97%/帧、GPU 一直满转，抓帧（vshot）抢不到 GPU，比 grim 慢 4 倍 /
	 * 卡半秒。上限取输出刷新率：比显示刷新率更快的帧本来就看不到。refresh
	 * 单位是 mHz，<=0 表示没报，按 60 兜底。
	 *
	 * 间隔留了容差（见 fd_min_interval_ns）：零容差会在刷新率边缘把 callbell
	 * 整帧吃掉，那正是「Xwayland 游戏一抖一抖」的根。 */
	int64_t fd_min_ns = fd_min_interval_ns(output->wlr_output);
	int64_t fd_now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
	if (getenv("VIDOW_X11_FD_UNTHROTTLED") == NULL &&
			output->last_x11_fd_ns != 0 &&
			fd_now_ns - output->last_x11_fd_ns < fd_min_ns) {
		return;
	}
	output->last_x11_fd_ns = fd_now_ns;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->kind != VIDOW_VIEW_X11 || view->x11 == NULL ||
				view->output != output->wlr_output) {
			continue;
		}
		struct wlr_surface *surface = vidow_view_surface(view);
		if (surface == NULL) {
			continue;
		}
		/* 子表面也要发：XWayland 的窗口可能带子表面（输入法、tooltip 挂载）。 */
		struct pump_ctx ctx = { .now = &now, .surfaces = 0 };
		wlr_surface_for_each_surface(surface, pump_surface, &ctx);
	}
}

/* **每帧给 native Wayland 视图兜底发一次 frame_done。**
 *
 * 这是照 Hyprland 的 sendFrameEventsToWorkspace() 抄的模型：它显式遍历每个窗口
 * 的整棵 surface 树发 frame（`breadthfirst([](r,...){ r->frame(now); })`），
 * **不依赖场景的可见性判定**。Vidow 原来完全靠
 * wlr_scene_output_send_frame_done()，而它只给「场景认为可见」的节点发回调。
 *
 * 场景会跳过一些**明明该画**的窗口：
 *   - 抓帧遮挡把真实表面的 opacity 压成 0（fx.c: fx->masked ? 0.0f : 1.0f），
 *     内容由 decor 烘焙图来画 —— wlroots 认为这块表面看不见；
 *   - 窗口有一部分被裁到输出框外；
 *   - FX 烘焙节点顶替了真实表面。
 *
 * 被跳过的后果是那个客户端永远等不到 frame_done。**Chromium / Electron
 * （Element、Discord）严格按 frame_done 节流**：拿不到就彻底停摆 —— 0fps、
 * 主线程睡在 do_sys_poll 里、输入全堵在 socket 里，用户看到的就是「卡死」。
 * 不按 frame_done 驱动的客户端（多数 GTK/Qt 程序）不受影响，所以只有 Electron
 * 这类出问题。
 *
 * **为什么不会自激**：pump_surface 只在 surface->current.frame_callback_list
 * 非空时才发，发完 wlroots 就把 list 清空 —— 场景已经发过的那些，这里再发是
 * no-op。没有 pending callback 的静止窗口一个字节都不发。所以这条兜底只补
 * 「场景漏发的」，不会多喂帧，也不会把输出顶成满刷新率空转。
 *
 * 节流和 X11 那条同源（按输出刷新率限速，比显示刷新率更快的帧本来也看不到），
 * 但用独立的时间戳：两条兜底互不影响。 */
void vidow_views_send_wayland_frame_done(struct vidow_server *server,
		struct vidow_output *output) {
	if (server == NULL || output == NULL) {
		return;
	}
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	int64_t fd_min_ns = fd_min_interval_ns(output->wlr_output);
	int64_t fd_now_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
	if (output->last_wl_fd_ns != 0 &&
			fd_now_ns - output->last_wl_fd_ns < fd_min_ns) {
		return;
	}
	output->last_wl_fd_ns = fd_now_ns;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		/* **不按 view->output 过滤。**
		 *
		 * 这条兜底要回答的问题是「谁在等帧、没人给它发」。而
		 * view->output 是个会漂的字段：拖动 / 换工作区 / 拔插屏之后它可能
		 * 变 NULL、或者停在旧屏上（view.c 的 view_on_screen 那段注释记着
		 * 这个分叉）。一旦它不匹配，这个 view 就**两条路都收不到**
		 * frame_done（render_pump 的 want 也按它过滤），客户端永久停摆 ——
		 * 而且只在某次操作之后才发生，表现就是「用一阵子才卡死」。
		 *
		 * 按 output 过滤本来也换不来什么：frame_done 是幂等的，谁先发谁把
		 * callback list 清空，第二块屏再遍历到就是 no-op。所以宁可每块屏都
		 * 遍历一遍所有 view（几十个，微秒级），也不要留下「某个 view 谁都
		 * 不管」的死角。 */
		if (view->kind == VIDOW_VIEW_X11 || !view->mapped) {
			continue;
		}
		/* 图标态画的是最后一张快照，就该停帧（补帧会把它推活，和
		 * 「静止的最后一帧」自相矛盾）。 */
		if (vidow_mini_uses_snapshot(view)) {
			continue;
		}
		struct wlr_surface *surface = vidow_view_surface(view);
		if (surface == NULL) {
			continue;
		}
		/* 整棵 surface 树（主子表面都发）：Chromium 的合成器进程可能挂在
		 * 子表面上。popup 也在这里发 —— 它是独立的 view，之前被 popup
		 * 判据排除在外，弹菜单 / tooltip 卡住时同样会拖死宿主程序。 */
		struct pump_ctx ctx = { .now = &now, .surfaces = 0 };
		wlr_surface_for_each_surface(surface, pump_surface, &ctx);
	}
}

/* 每帧调一次：把「规则要求保活」或者「正被人拿着（录屏 / 预览）」但**没画到**的
 * 窗口补上帧回调，并且让输出继续出帧 —— 不然补帧链自己就断了（没东西要画的时候
 * wlroots 不会给帧事件）。
 *
 * 为什么需要这个：Wayland 的帧回调是「我要用你的画面了」的许可。窗口滚到视口外 /
 * 所在工作区被藏起来时，场景里什么都没画到它，客户端拿不到许可 —— 视频播放器、
 * 游戏、任何按帧节流的程序就此停帧。切回来是一张冻住的画面，录屏录到的也是它。 */
void vidow_view_render_pump(struct vidow_server *server,
		struct vidow_output *output, uint32_t msec) {
	struct timespec now = {0};
	now.tv_sec = msec / 1000;
	now.tv_nsec = (long)(msec % 1000) * 1000000L;

	bool keep_going = false;
	int64_t next_due = 0; /* 被节流挡下的那些窗口里，最早该醒的时刻 */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		/* 出帧策略跟 force_render 走：翻面了就接上（见 frames_recheck）。 */
		if (view->mini == VIDOW_MINI_ICON || view->mini_snapshot != NULL) {
			vidow_mini_frames_recheck(view);
		}
		/* 所有 native Wayland 窗口都进来判一次，是否真补由下面的
		 * view_on_screen() 决定：在屏上的由场景那边正常发 frame_done
		 * （这里 continue，不重复发），不在屏上的（滚出视口 / 工作区藏起来 /
		 * 最小化）才补。
		 *
		 * 以前这里还多要一道 `force_render || 录屏 || mini` 的许可，普通
		 * 窗口被挡在门外 —— 一旦滚出视口就再没人给它发 frame_done，
		 * Chromium（Element）交完一帧就停摆：拿不回 buffer，合成器不提交，
		 * 主线程睡死，输入全堵在 socket 里（现场 fd 写缓冲 213504 字节 =
		 * socket 满）。这跟 X11 那条「照 Hyprland 无条件发」是同一个病。
		 *
		 * X11 不走这条路：vidow_views_send_x11_frame_done()（output.c）
		 * 已经无条件兜底，这里再发一遍就是重复催帧。
		 * 走快照的图标仍然排除（S5：屏上画的是最后一帧快照，就该停帧，
		 * 补帧会把它又推活、和「静止的最后一帧」自相矛盾）。 */
		bool want = view->mapped && !view->popup &&
			view->kind != VIDOW_VIEW_X11 &&
			view->output == output->wlr_output &&
			!vidow_mini_uses_snapshot(view);
		if (!want) {
			view->pump_hidden_frames = 0;
			view->pump_shown_frames = 0;
			if (view->render_pumping) {
				if (vidow_flip_on()) {
					vidow_flip_note("pump", view, false);
				}
				view->render_pumping = false;
				wlr_log(WLR_DEBUG, "强制渲染: %s 停（不再需要）",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)");
			}
			continue;
		}

		/* **先问「它是不是已经画在画面上了」**，再谈补帧。
		 *
		 * 这条顺序是有意义的：补帧本来是给「滚出视口 / 工作区被藏起来 / 最小化」
		 * 的窗口续命的，已经画在画面上的窗口由场景那边正常发 frame_done，根本
		 * 不该走这条路。老代码把 force_render 放在前面先放行，再在这里补一句
		 * 「在屏上就停」—— 等于每帧先进来再把 render_pumping 清掉，只要上面
		 * 的判定有任何抖动（arrange 期间 view->geom / 工作区会逐帧变），
		 * 状态就跟着每帧翻一次，翻一次喂客户端一帧 frame_done：全屏游戏被反复
		 * 唤醒重画，GPU 功耗异常（实测 85 次/秒的状态翻转）。
		 *
		 * 所以这里加**迟滞**：连续 PUMP_HYSTERESIS 帧「看不上屏」才真的开始补，
		 * 连续同样多帧「看得上屏」才停。抖动幅度小于这个窗口时状态不再翻转，
		 * 真实的状态变化（切工作区 / 最小化）只晚这几帧，用户感知不到。 */
		/* **在屏上就不补帧**（迟滞防抖）。
		 *
		 * 已经画在画面上的窗口由场景那边正常发 frame_done，不该走这条路。
		 * 迟滞是为了防 arrange 期间 view->geom / 工作区逐帧变化导致状态
		 * 每帧翻转（翻一次喂客户端一帧，全屏游戏被反复唤醒重画）。
		 *
		 * 2026-10-27：这里原来还有一条 `x11_owes` 旁路，让「X11 欠帧」的
		 * 窗口跳过这道闸。那条旁路已经删了 —— 见下面关于 X11 欠帧的说明。 */
		if (view_on_screen(server, output, view)) {
			view->pump_hidden_frames = 0;
			if (view->pump_shown_frames < PUMP_HYSTERESIS) {
				view->pump_shown_frames++;
			}
			if (view->render_pumping &&
					view->pump_shown_frames >= PUMP_HYSTERESIS) {
				view->render_pumping = false;
				wlr_log(WLR_DEBUG, "强制渲染: %s 停（它已经画在画面上）",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)");
			}
			/* 还在迟滞窗口里（刚被判定为在屏上，但还没连续够帧数）：不补帧，
			 * 也不改状态 —— 等下一次判定。 */
			continue;
		}
		view->pump_shown_frames = 0;
		if (view->pump_hidden_frames < PUMP_HYSTERESIS) {
			view->pump_hidden_frames++;
		}
		if (view->pump_hidden_frames < PUMP_HYSTERESIS) {
			continue; /* 刚看不上屏，还没连续够：先不动 */
		}

		/* 节流（逐窗口，规则 / 全局配置说了算）。还没到点的记下最早该醒的
		 * 时刻，收尾时定个闹钟 —— 不能什么都不做：客户端提交一次之后就没人
		 * 再推了，30Hz / 15Hz 的补帧会断在半路（踩过：全局 30 补出来 0 帧）。
		 *
		 * **节流绝不允许退化成不限速。**
		 *
		 * pump_interval_ms 为 0 时老代码直接放行（不设下一次的到点时刻），
		 * 于是每个渲染帧都补一次 —— Vidow 出帧 4~6ms 一张，那就是 180Hz
		 * 空转。实测：游戏滚出视口时日志「不限速」每 6ms 一条，主循环被
		 * 占满，按键队列延迟一路累积到 2078ms（KEYPROBE 的 lag 每 40ms
		 * 涨一次）。
		 *
		 * 现在 0 一律按全局 force_render_hz 兜底；那个值也是 0（用户显式
		 * 要「不限」）时按 60Hz 兜底 —— 「不限」这个语义在补帧路径上没有
		 * 正当用途，只会把主循环烧穿。 */
		int interval = view->pump_interval_ms > 0 ?
			view->pump_interval_ms :
			(server->config.force_render_hz > 0 ?
				(1000 / (server->config.force_render_hz > 1000 ?
					1000 : server->config.force_render_hz)) : 16);
		if (interval < 1) {
			interval = 1;
		}
		if (msec < (uint32_t)view->pump_next_ms) {
			if (next_due == 0 || view->pump_next_ms < next_due) {
				next_due = view->pump_next_ms;
			}
			continue;
		}
		view->pump_next_ms = (int64_t)msec + interval;

		struct wlr_surface *surface = vidow_view_surface(view);
		if (surface != NULL) {
			struct pump_ctx ctx = { .now = &now, .surfaces = 0 };
			wlr_surface_for_each_surface(surface, pump_surface, &ctx);
			if (ctx.surfaces > 0) {
				keep_going = true;
			}
		}
		if (!view->render_pumping) {
			view->render_pumping = true;
			char rate[32];
			snprintf(rate, sizeof(rate), "节流 %dHz", 1000 / interval);
			/* 「为什么它没画在画面上」说清楚：最小化 / 工作区被藏起来 / 滚出
			 * 视口 —— 三种的判据都是 view_on_screen()，这里只是把原因写出来。 */
			const char *why = view->minimized ? "它最小化了" :
				(!vidow_workspace_view_visible(server, view) ?
					"它那块工作区藏起来了" : "它滚出视口了");
			wlr_log(WLR_DEBUG, "强制渲染: %s %s，照样补帧回调"
				"（%s，%s）", vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)", why,
				vidow_view_capture_held(view) ? "有人正在录屏 / 预览它" :
					"窗口规则要求", rate);
		}
	}

	/* 补帧得有人推：不主动排下一帧的话，画面没变化时 wlroots 就不给帧事件了。 */
	if (keep_going) {
		vidow_output_schedule_frame(output->server, output->wlr_output,
			"布局/视图改了");
	}

	/* 被节流挡下的：定个到点闹钟（比当前定的更早就改早一点）。 */
	if (next_due > 0 && output->pump_timer != NULL) {
		if (output->pump_at_ms == 0 || next_due < output->pump_at_ms) {
			int64_t delay = next_due - (int64_t)msec;
			if (delay < 1) {
				delay = 1;
			}
			wl_event_source_timer_update(output->pump_timer, (int)delay);
			output->pump_at_ms = next_due;
			/* 影子调度：补帧闹钟也是一个「到时候要一帧」的来源（见
			 * output.c 的 vidow_output_note_deadline）。 */
			vidow_output_note_deadline(output,
				vidow_now_ns() + delay * 1000000LL, "补帧");
		}
	} else if (output->pump_at_ms != 0 && output->pump_timer != NULL) {
		wl_event_source_timer_update(output->pump_timer, 0); /* 没人被挡下：撤闹钟 */
		output->pump_at_ms = 0;
	}
}

/* 有人正拿着这个窗口的画面吗（有一条持有记录就算）。 */
bool vidow_view_capture_held(const struct vidow_view *view) {
	return !wl_list_empty(&view->capture_sources);
}

static void capture_hold_drop(struct vidow_capture_hold *hold,
		const char *why) {
	wl_list_remove(&hold->link);
	wl_list_remove(&hold->destroy.link);
	if (hold->client != NULL) {
		wl_list_remove(&hold->client_destroy.link);
	}
	if (hold->view != NULL && view_alive(hold->server, hold->view)) {
		wlr_log(WLR_INFO, "portal: %s，%s 不再强制渲染", why,
			vidow_view_title(hold->view) != NULL ?
				vidow_view_title(hold->view) : "(无题)");
	}
	free(hold);
}

static void capture_source_gone(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_capture_hold *hold = wl_container_of(listener, hold, destroy);
	capture_hold_drop(hold, "窗口截图源没了");
}

/* 录屏 / 预览的客户端断开：它拿着的那份「保活」也要放掉，不然一个崩掉的录屏程序
 * 会让窗口一直按 60fps 补帧。 */
static void capture_client_gone(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_capture_hold *hold =
		wl_container_of(listener, hold, client_destroy);
	capture_hold_drop(hold, "录屏 / 预览的客户端断了");
}

void vidow_view_capture_hold(struct vidow_view *view,
		struct vidow_server *server,
		struct wlr_ext_image_capture_source_v1 *source,
		struct wl_client *client) {
	if (view == NULL || source == NULL) {
		return;
	}
	struct vidow_capture_hold *hold = calloc(1, sizeof(*hold));
	if (hold == NULL) {
		return;
	}
	hold->view = view;
	hold->server = server;
	hold->source = source;
	hold->client = client;
	hold->destroy.notify = capture_source_gone;
	wl_signal_add(&source->events.destroy, &hold->destroy);
	if (client != NULL) {
		hold->client_destroy.notify = capture_client_gone;
		wl_client_add_destroy_listener(client, &hold->client_destroy);
	}
	wl_list_insert(view->capture_sources.prev, &hold->link);
}

/* 窗口要没了：把持有记录清干净（源自己会跟着场景节点销毁，这里只管我们这份）。 */
static void view_capture_holds_clear(struct vidow_view *view) {
	struct vidow_capture_hold *hold, *tmp;
	wl_list_for_each_safe(hold, tmp, &view->capture_sources, link) {
		wl_list_remove(&hold->link);
		wl_list_remove(&hold->destroy.link);
		if (hold->client != NULL) {
			wl_list_remove(&hold->client_destroy.link);
		}
		free(hold);
	}
}

/* 切工作区的过渡里「正被推开的那一块」上的窗口：它不属于现在显示着的工作区
 * （vidow_workspace_view_visible 为假），但过渡期间照样得画 —— 它正从屏上滑
 * 出去。滑到屏外之后 should_park 最后那条 !in_output_box 会把它停掉。
 *
 * 只放行「画」：可见性本身不含它，所以 pick-window / 切换器清单 / 录屏判据都
 * 不会被过渡里那半屏旧窗口带偏（test-ipc.sh 的「空工作区上 pick-window 是
 * null」就是这个坑）。 */
static bool view_sliding_out(struct vidow_server *server,
		const struct vidow_view *view) {
	if (view->ws == NULL) {
		return false;
	}
	/* 找**工作区住的那块屏**，不是 view->output 这个住址：热插拔 / 规则搬屏
	 * 的中间态里两者会短暂分叉，按住址找会漏掉正在滑走的那块，过渡里旧的
	 * 窗口突然消失。 */
	struct wlr_output *want = view->ws->output != NULL ?
		view->ws->output : view->output;
	if (want == NULL) {
		return false;
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		if (output->wlr_output == want) {
			return output->slide_from == view->ws;
		}
	}
	return false;
}

/* 「它现在该停到所有输出框外面吗」—— 判据只有这一份：workspace.c 的可见性那一趟
 * 和 view_anim_place（每帧现算）都问它。
 *
 * 为什么每帧要现算、不能只在 arrange 里存一个标志：滚动 / 开窗的动画帧会自己摆
 * 位置（不经过 arrange），窗口从屏边界上扫过去的时候存下来的标志就过期了 ——
 * 摆回屏里之后还停在停车位上，那个窗口就再也不出现了。 */
bool vidow_view_should_park(struct vidow_server *server,
		struct vidow_view *view) {
	if (view->popup || view->scene_tree == NULL) {
		return false; /* 弹出物：各有各的路（见 workspace.c） */
	}
	/* 还没上屏的（从没 commit 过 buffer）：**也 park**。
	 *
	 * 原来这里还有 `!view->mapped` 一条，理由是「各有各的路」—— 但没上屏的
	 * 窗口根本没有路：它留在场景里、留在布局原点，每帧都要渲染一遍。实测一个
	 * 会话里 41 个 view 有 40 个是这种（vivaldi 一个就开一堆子表面），全都
	 * 在画，于是**所有应用都卡**（桌面是静态背景，不重画，所以不卡）。
	 * 第一次 map 时 view_map 会调 vidow_view_set_parked(false) 放回来。 */
	if (!view->mapped) {
		return true;
	}
	/* 小窗（S5）：图标态 + 该窗口不保活 + 手里有最后一帧 → 真实节点停到停车位
	 * （它不在任何输出上，客户端于是停帧），屏上由快照节点画（见
	 * mini_snapshot_place）—— 这就是「图标是静止的最后一帧」那条路。 */
	if (vidow_mini_uses_snapshot(view)) {
		return true;
	}
	if (!vidow_workspace_view_visible(server, view) &&
			!view_sliding_out(server, view)) {
		/* overview 里**所有屏、每块屏上所有工作区都要画**（它们各自上下排成
		 * 一列，见 overview.c）—— 平时「藏起来的工作区不画」，全景里得放回来，
		 * 不然那一行是空的。最小化的照旧不画（它连列里都不占位置，摆不出坐标）。
		 *
		 * 以前这里还要求 view->output == server->overview.output（唯一那块
		 * 「进来时聚焦的屏」），别的屏上的窗口一律停着 —— 于是非激活显示器
		 * 上什么都看不到（用户 2026-10-25：「overview 触发的时候应该所有
		 * 显示器都进 overview，现在虽然都进了但非激活显示器不显示内容」）。
		 * 现在每块屏各有一份行表，所有屏上的窗口都该摊开。 */
		bool in_overview = vidow_overview_active(server) && !view->minimized;
		if (!in_overview) {
			return true; /* 最小化 / 工作区被藏起来：也不画 */
		}
	}
	if (vidow_overview_active(server)) {
		return false; /* overview 全缩进画面 */
	}
	/* 浮窗 / **正被拖的**窗口允许跨屏，照旧画。
	 *
	 * 为什么要放行：拖拽的时候窗口就是该跟着光标走到隔壁屏上 —— 用户
	 * 2026-10-20 的原话「拖拽窗口的时候应该允许显示在另一个屏幕」。不放行的
	 * 话几何框一越过屏边界就整块停到停车位，屏上表现为「拖到边上窗口就没了」，
	 * 跨屏拖动直接抄作不出来。
	 *
	 * 「正在拖」的两个来源都得认：平铺窗口拖拽走 drag.c（view->dragging），
	 * 浮窗拖动 / 列宽拖动走 input.c 那条抓取（server->grab_view）。只看前者
	 * 漏掉浮窗，只看后者漏掉还没起拖阈值的平铺窗口。 */
	if (view->float_mode != VIDOW_FLOAT_NONE || view->dragging ||
			server->grab_view == view) {
		return false;
	}
	/* 其余窗口（有工作区的）只在自己的屏里画：越界就停到停车位，别让几何框
	 * 落进隔壁屏的坐标范围（否则场景图是全局坐标，会画到隔壁显示器上）。 */
	return !vidow_view_in_output_box(server, view);
}

/* 停到屏外 / 放回屏里。**只有这一个入口**改这个标志：日志和损伤都在这儿 ——
 * 「藏起 / 显示」这行状态是给人和测试看「它现在是什么样」的，所以每次翻转都得
 * 写一行（view_map 里那次翻转也算）。 */
void vidow_view_set_parked(struct vidow_view *view, bool park) {
	if (view->scene_parked == park) {
		return;
	}
	if (vidow_flip_on()) {
		vidow_flip_note("park", view, park);
	}
	view->scene_parked = park;
	/* 它自己进/出画面了：底衬的合成结果变了。账记在它头上（它自己的私有底衬
	 * 不受影响），别的窗口因为世界代次前进而重抓。 */
	vidow_backdrop_damage_view(view);
	/* **必须在这儿排一帧，否则补帧链永远启动不了。**
	 *
	 * 补帧（vidow_view_render_pump）只在 vidow_output_render 里跑，而
	 * vidow_output_render 只在有帧事件时跑 —— 输出静止就没有帧事件。补帧
	 * 闹钟（output->pump_timer）又只在 render_pump 内部才会被设上。于是：
	 *
	 *   窗口滚出视口 → 拿不到 frame_done → 客户端停止提交 → 不再产生损伤
	 *   → 输出静止 → render_pump 不跑 → 闹钟从没设过 → 永远没人再推一帧
	 *
	 * 这就是「Element / Discord 滚出视口后 0fps + 不可交互」的死锁：客户端
	 * 在等 frame_done，合成器在等损伤，两边互等。Hyprland 不会这样，因为它
	 * 每块屏每帧无条件渲染（vsync 驱动），sendFrameEventsToWorkspace 每帧都
	 * 跑，补帧这件事根本不需要额外的触发源。
	 *
	 * Vidow 是按需渲染，就得自己制造这个触发点：窗口一进「停到屏外」这个
	 * 状态就排一帧，让 render_pump 跑一次、把补帧闹钟设上，链子就接起来了。
	 * 滚回来时同样排一帧（下面 park == false 那条也走这里），立刻恢复。 */
	vidow_output_schedule_frame(view->server, view->output,
		park ? "窗口滚出视口" : "窗口回到屏上");
wlr_log(WLR_DEBUG, "workspace: %s %s（%s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		park ? "藏起（停到屏外）" : "显示",
		view->ws != NULL ? view->ws->name : "-");
}

/* 停到哪儿：所有输出框外面的「停车位」—— 左上角再往外挪 64K，按窗口编号横着
 * 排开（一格 16384 逻辑像素，够 16K 宽的窗口）。为什么按编号排开：按窗口录屏的
 * 虚拟输出框就是节点自己那块框，两个停着的窗口挨在一起的话，录一个会连旁边那个
 * 一起录进去。 */
static void view_park_pos(const struct vidow_view *view, int *px, int *py) {
	struct wlr_box all = {0};
	wlr_output_layout_get_box(view->server->output_layout, NULL, &all);
	if (all.width < 1 || all.height < 1) {
		all.x = 0;
		all.y = 0;
	}
	int slot = (int)(view->id % 64);
	*px = all.x - 65536 - slot * 16384;
	*py = all.y - 65536;
}

/* overview 里这个窗口的透明度：别的屏上的淡出（老规矩）、**同一块屏上别的工作区**
 * 的跟着 t 淡入淡出 —— 它们平时不画，全景里才露出来，不这么办进出时会「啪」地
 * 冒出来 / 消失。摆位（view_anim_place）和重烘（vidow_view_anim_bake）必须算出
 * 同一个值，所以只有这一份。 */
static double view_overview_alpha(struct vidow_server *server,
		struct vidow_view *view) {
	double alpha = view->anim_alpha;
	if (server->overview.t <= 0.0 || server->overview.output == NULL) {
		return alpha;
	}
	/* **不再按「是不是焦点屏」发暗**：所有屏一起进 overview，每块屏都摊开
	 * 自己那份行表，所以每块屏上的窗口都该正常显示。以前非焦点屏的窗口会被
	 * 乘上 (1 - t) 淡到全透明 —— 那正是「非激活显示器不显示内容」的成因之一
	 * （就算把行摆过去，窗口也是透明的）。 */
	/* overview 里**不做整体隐藏**：溢出工作区的部分由 fx.c 的 ov_clip 裁掉
	 * （它把这块窗口裁到「这一行的矩形」，也就是屏幕上可见的那一块）。
	 *
	 * 这里以前有一段「整块落在行矩形外就 alpha = 0」的隐藏，是 fx.c 还没有
	 * ov_clip 时用来挡「overview 窗口太多会跨屏」的。现在裁剪已经有了，那层
	 * 隐藏就成了多余而且有害的：判据（行矩形）和裁剪框（整块屏）不是同一个
	 * 东西，落在行间隙里、本来该被**裁一半**显示的窗口被整个藏掉了 ——
	 * 用户要的是「所有窗口都显示，超出的部分由显示器裁」。 */
	if (!vidow_workspace_view_visible(server, view)) {
		return alpha * server->overview.t;
	}
	return alpha;
}

/* 小窗（mini window，S3）：内容的**原生**尺寸（场景里的 1:1 尺寸）。X11 那条在
 * commit 时记过 base_dst（已经除过 xwayland_scale），没有就取表面自己的逻辑尺寸
 * —— 两者都是「没被小窗缩过」的量，所以每帧算出来稳定（拿节点现在的 dst 算就会
 * 越缩越小）。 */
void view_mini_native(const struct vidow_view *view, int *w, int *h) {
	int nw = view->base_dst_w, nh = view->base_dst_h;
	/* xdg（非 X11）：表面的逻辑尺寸**就是**场景里的 1:1 尺寸 —— 小窗 / 图标态
	 * 不下发 configure（保留原生 buffer，见 vidow_view_set_size），客户端那一份
	 * 一直没变，随时读都是当前值。这里优先读它，别读 base_dst_*：那个字段 xdg
	 * 只在小窗退出那一刻记一次（commit 那条路不记），输出缩放一变、工作区逻辑
	 * 尺寸跟着变、客户端被重新 configure 成另一个大小之后就永久过期了 ——
	 * 重进小窗时 contain 系数按过期的原生尺寸算，盒里内容只有该有的一半
	 * （实测：172x190 物理脚印变成 84x94；从小窗态里改 scale 那条不重进，反而
	 * 是对的，因为 base_dst_* 还是 0、走的正是这个兜底）。 */
	if (view->kind != VIDOW_VIEW_X11) {
		struct wlr_surface *live = vidow_view_surface(view);
		if (live != NULL && live->current.width > 0 &&
				live->current.height > 0) {
			*w = live->current.width;
			*h = live->current.height;
			return;
		}
	}
	if (nw <= 0 || nh <= 0) {
		struct wlr_surface *surface = vidow_view_surface(view);
		nw = surface != NULL ? surface->current.width : 0;
		nh = surface != NULL ? surface->current.height : 0;
	}
	*w = nw > 0 ? nw : 0;
	*h = nh > 0 ? nh : 0;
}

/* 小窗（mini window，S3）：把原生内容缩到小窗盒的系数。1.0 = 不缩。
 *
 * contain（取宽高比里小的那个）：内容整个塞得进盒子、按比例不拉伸。宽高比策略
 * 本身（mini_aspect / PAspect / 拖拽锁比例）是 S3b 的事，这里只保证「缩了、没
 * 拉伸」。盒子比内容大时按比例放大 —— 图标是固定边长，小窗口也该填满它。
 *
 * 关掉保留（mini_preserve_buffer = false）时恒为 1.0：那条路是直接把客户端
 * configure 成小窗尺寸，场景节点不动。 */
static double view_mini_scale(const struct vidow_view *view) {
	if (view->mini == VIDOW_MINI_NONE || view->float_geom.width <= 0 ||
			view->float_geom.height <= 0 ||
			!vidow_mini_preserve_buffer(view)) {
		return 1.0;
	}
	int nw = 0, nh = 0;
	view_mini_native(view, &nw, &nh);
	if (nw <= 0 || nh <= 0) {
		return 1.0;
	}
	double sx = (double)view->float_geom.width / (double)nw;
	double sy = (double)view->float_geom.height / (double)nh;
	double s = sx < sy ? sx : sy;
	return s > 0.0 ? s : 1.0;
}

/* 给小窗的过渡动画用（float.c 的 mini_transition_start / vidow_mini_set_state）：
 * 这一帧内容画出来的缩放、以及内容原点相对盒子原点的居中偏移。必须和
 * view_anim_place 里那两处（view_mini_scale 与 mini_cx/mini_cy）同源 —— 补间
 * 的起点是「上一帧画在哪」，拿别的量推出来就会在转换的第一帧跳一下（用户报的
 * 「退出小窗的一瞬间显示原始大小」就是这个跳）。 */
double vidow_view_mini_scale_export(const struct vidow_view *view) {
	return view_mini_scale(view);
}

void vidow_view_mini_content_offset(const struct vidow_view *view, int *dx,
		int *dy) {
	if (dx != NULL) {
		*dx = 0;
	}
	if (dy != NULL) {
		*dy = 0;
	}
	if (view == NULL) {
		return;
	}
	double ms = view_mini_scale(view);
	if (ms == 1.0 || view->float_geom.width <= 0 ||
			view->float_geom.height <= 0) {
		return;
	}
	/* 几何取**可见窗口**（和 view_anim_bake 的 mini_cx/mini_cy 同一个来源）：
	 * 装饰那一条路画的就是它。 */
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	int cw = geo.w, ch = geo.h;
	if (cw <= 0 || ch <= 0) {
		view_mini_native(view, &cw, &ch);
	}
	if (cw <= 0 || ch <= 0) {
		return;
	}
	if (dx != NULL) {
		*dx = (int)lround(((double)view->float_geom.width -
			(double)cw * ms) / 2.0);
	}
	if (dy != NULL) {
		*dy = (int)lround(((double)view->float_geom.height -
			(double)ch * ms) / 2.0);
	}
}

/* 「摆在哪」落到场景节点上：位置 / 透明度 / overview 的显示尺寸 / 装饰的采样
 * 偏移和 ext-background-effect 的区域。**便宜**，拖动跟手时每个指针事件都走这条。
 * 装饰图的重烘（贵：整块窗口那么大的离屏渲染）在 vidow_view_anim_bake 里，一帧
 * 只做一次（见 vidow_tile_grab_update / output.c 的帧回调）。 */
static void view_anim_place(struct vidow_view *view) {
	/* 场景节点可能已经没了：X11 销毁窗口时先摘掉表面（scene_tree 归 NULL）再撤
	 * 动画，而撤销动画会回调 view_open_done() 回到这里 —— 以前就死在这行上。 */
	if (view->scene_tree == NULL) {
		return;
	}
	/* 表面进哪块输出：换工作区 / 换屏 / 刚 map，都在这里跟过去。 */
	view_sync_surface_outputs(view);

	/* overview 里：位置走「工作区局部 -> 屏幕」变换（每块工作区一行，见
	 * overview.c），内容尺寸乘上缩放；不在这块屏上 / 不在现在这块工作区里的
	 * 窗口淡出。 */
	struct vidow_server *server = view->server;
	double ov = vidow_overview_scale(server);
	double alpha = view_overview_alpha(server, view);
	int x = 0, y = 0;
	double sx_mapped = 0, sy_mapped = 0;
	vidow_overview_map(server, view, view->anim_off_x, view->anim_off_y,
		&sx_mapped, &sy_mapped);
	x = (int)lround(sx_mapped);
	y = (int)lround(sy_mapped);
	if (vidow_xtrace_on() && view->kind == VIDOW_VIEW_X11 &&
			(view != NULL && vidow_view_title(view) != NULL &&
			strcmp(vidow_view_title(view), "Steam") == 0)) {
		wlr_log(WLR_INFO, "place: %s geom=%d,%d %dx%d anim_off=%.0f,%.0f"
			" -> xy=%d,%d ov_t=%.2f",
			vidow_view_title(view), view->geom.x, view->geom.y,
			view->geom.width, view->geom.height,
			view->anim_off_x, view->anim_off_y, x, y,
			vidow_overview_scale(view->server) < 1.0 ? 1.0 : 0.0);
	}

	/* 停车：滚出自己那块屏 / 工作区藏起来的窗口不关节点，改成停到所有输出框外面
	 * （见 server.h 的 scene_parked）。位置只有这一个出口 —— 藏不藏、摆哪儿都在
	 * 这儿定，别在别处再 wlr_scene_node_set_position 一次。判据每帧现算（动画帧
	 * 里窗口会扫过屏边界，存下来的标志会过期）。 */
	vidow_view_set_parked(view, vidow_view_should_park(server, view));
	/* 小窗（S5）：图标态走快照那条路时，屏上那块由快照节点画 —— 位置取
	 * 「图标画在哪」（geom + anim_off，和下面 park 用的是同一个来源），
	 * 不是停车位。 */
	if (view->mini == VIDOW_MINI_ICON || view->mini_snapshot != NULL) {
		mini_snapshot_place(view);
	}
	if (view->scene_parked) {
		view_park_pos(view, &x, &y);
	}

	wlr_scene_node_set_position(&view->scene_tree->node, x, y);
	/* 跟手探针（touch /tmp/vidow-x11-trace 开）：只在拖拽期间、每次摆位一行。
	 * 看的就是「摆出去的这一下到底是什么」—— 偏移是不是被别处抹成 0、是不是
	 * 被停在停车位上、算出来的 xy 有没有跟着光标走。 */
	if (vidow_xtrace_on() && (server->grab_view == view || view->dragging)) {
		vidow_gtrace("place v=%d geom=%d,%d %dx%d off=%.0f,%.0f -> %d,%d "
			"park=%d alpha=%.2f ov_t=%.2f",
			view->id, view->geom.x, view->geom.y,
			view->geom.width, view->geom.height,
			view->anim_off_x, view->anim_off_y, x, y,
			view->scene_parked ? 1 : 0, alpha,
			vidow_overview_scale(server) < 1.0 ? 1.0 : 0.0);
	}
	struct wlr_scene_node *probe_n;
	int probe_i = 0;
	if (vidow_xtrace_on() && (server->grab_view == view || view->dragging)) {
		wlr_log(WLR_INFO, "place-set: %s 节点摆在 %d,%d（en=%d）",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)", x, y,
			view->scene_tree->node.enabled);
	}
	wl_list_for_each(probe_n, &view->scene_tree->children, link) {
		if (probe_i++ > 5) {
			break;
		}
		if (!vidow_xtrace_on() ||
				!(server->grab_view == view || view->dragging)) {
			break;
		}
		if (probe_n->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(probe_n);
			wlr_log(WLR_DEBUG, "place-set:   子 buf en=%d off=%d,%d "
				"dst=%dx%d op=%.2f buf=%d", probe_n->enabled, probe_n->x,
				probe_n->y, sb->dst_width, sb->dst_height, sb->opacity,
				sb->buffer != NULL);
		} else {
			wlr_log(WLR_DEBUG, "place-set:   子 type=%d en=%d off=%d,%d",
				probe_n->type, probe_n->enabled, probe_n->x, probe_n->y);
		}
	}
	vidow_fx_set_alpha(&view->scene_tree->node, alpha);

	/* 客户端该按几倍渲染也在这儿钉（跟位置一起，每帧一次、只在真的变了时发）：
	 * 停到停车位之后它就不在任何输出上了，不钉的话客户端会掉回 1 倍。 */
	view_pin_scale(view);

	/* 小窗（mini window，S3）：保留原生分辨率时，把场景节点缩到小窗盒 ——
	 * 客户端 buffer 一个像素都不动（不 configure、不重建 swapchain，见
	 * float.c 的 on_commit / view.c 的 vidow_view_set_size）。
	 *
	 * **必须放在下面那个 fx.active 判断之前**：装饰全关（fx 不 active）正是
	 * 「全屏游戏进小窗」最可能的状态，写在里面就等于不生效。
	 *
	 * 为什么在这儿设得住：wlroots 每次 commit 都会把 dst 重置成表面自己的尺寸
	 * （types/scene/surface.c 的 surface_reconfigure），而我们的 commit 监听器
	 * 排在它的后面（xdg：wlr_scene_xdg_surface_create 先注册；X11：见
	 * x11_attach_surface 的顺序），所以这里设的值当帧就留下 —— 但**每个 commit
	 * 都得重设一次**，view_commit 那边因此对迷你态也调 apply_anim。 */
	if (view->scene_surface != NULL && view->scene_surface->buffer != NULL) {
		struct wlr_scene_buffer *mini_src = view->scene_surface->buffer;
		double ms = view_mini_scale(view);
		if (ms != 1.0) {
			int nw = 0, nh = 0;
			view_mini_native(view, &nw, &nh);
			int dw = (int)lround((double)nw * ms);
			int dh = (int)lround((double)nh * ms);
			if (dw < 1) {
				dw = 1;
			}
			if (dh < 1) {
				dh = 1;
			}
			if (!view->mini_scaled) {
				/* 记下「还没居中之前内容节点在它父节点里的位置」：xdg 的窗口
				 * 几何留白（CSD 阴影那圈）有可能是 wlroots 设在这一层的，退出
				 * 缩放态时要还原成它。 */
				view->mini_base_dx = mini_src->node.x;
				view->mini_base_dy = mini_src->node.y;
			}
			wlr_scene_buffer_set_dest_size(mini_src, dw, dh);
			/* S3b：内容在盒子里**居中** —— contain 留出来的信件箱两边平分。
			 * 以前是左上对齐（内容贴着盒子左上角，留白全堆在右下），FREE 盒
			 * 里「看着是内容中间」的那一点其实落在留白上，点不着。
			 *
			 * 相对的是 scene_tree（盒子原点），所以要把父节点那一层的偏移
			 * （xdg 的窗口几何）减掉：两种留白挂在哪一层都算得对。
			 *
			 * 每帧重设（跟 dst 一样）：wlroots 每次 commit 会把节点位置按自己
			 * 的规则重算一遍，我们排在它的监听器后面，重设才留得住。 */
			int vx = 0, vy = 0, px = 0, py = 0;
			wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy);
			if (mini_src->node.parent != NULL) {
				wlr_scene_node_coords(&mini_src->node.parent->node, &px, &py);
			}
			int off_x = view->mini_base_dx +
				(view->float_geom.width - dw) / 2 - (px - vx);
			int off_y = view->mini_base_dy +
				(view->float_geom.height - dh) / 2 - (py - vy);
			wlr_scene_node_set_position(&mini_src->node, off_x, off_y);
			view->mini_scaled = true;
		} else if (view->mini_scaled || view->mini != VIDOW_MINI_NONE) {
			/* 退出缩放态，**或者**本来就不缩放的小窗态（FREE：保留 buffer 不再
			 * 管它，客户端已经被 configure 成盒子大小，内容是 1:1 的）。
			 *
			 * 第二种情形必须也钉一遍：wlroots 每次 commit 都会把 dst 重算成
			 * 「缓冲像素 ÷ 缓冲缩放」，而那个值不一定等于表面自己的逻辑尺寸
			 * —— 实测 FREE 里一个逻辑 544x355 的表面被设成 **277x355**。屏上
			 * 画的是装饰图（fx 那条路）所以看着没错，但**命中测试认的是内容
			 * 节点**（wlroots 的 scene_node_get_size + 表面输入区域）⇒ 半个
			 * 窗口「看得见、点不着」：实测点盒心 (1018,167) 命中不到窗口、命中
			 * 区只剩 x∈[746,1023]（= 277 宽），而盒子是 766..1270。
			 *
			 * 位置只在**离开缩放态**时才撤居中偏移；FREE 这种本来就 1:1 的，
			 * 位置由 wlroots 按窗口几何留白（CSD 阴影）摆，别去动它。 */
			bool was_scaled = view->mini_scaled;
			view->mini_scaled = false;
			if (was_scaled) {
				/* 居中偏移一起撤掉（位置还原成进缩放态之前那一版）。 */
				wlr_scene_node_set_position(&mini_src->node,
					view->mini_base_dx, view->mini_base_dy);
			}
			if (view->kind == VIDOW_VIEW_X11) {
				vidow_view_x11_scale(view);
			} else {
				/* 钉成**表面自己的逻辑尺寸**（wlroots 下一次 commit 也会设
				 * 成这个值）—— 不用 0：0 在 wlroots 那边是「按缓冲像素当场景
				 * 单位」，2 倍缓冲的客户端会被先画大一倍，而且它下一次 commit
				 * 之前一直错（窗口不再交帧的话就一直错下去）。 */
				struct wlr_surface *surface = vidow_view_surface(view);
				if (surface != NULL && surface->current.width > 0 &&
						surface->current.height > 0) {
					wlr_scene_buffer_set_dest_size(mini_src,
						surface->current.width, surface->current.height);
				} else if (was_scaled) {
					wlr_scene_buffer_set_dest_size(mini_src, 0, 0);
				}
			}
			view_dst_capture(view);
		}
	}


	if (!view->fx.active || view->scene_surface == NULL) {
		return;
	}
	struct wlr_scene_buffer *src = view->scene_surface->buffer;
	if (src == NULL) {
		return;
	}
	/* overview 里**内容节点的盒子就是命中盒**：wlroots 的 scene_node_get_size()
	 * 拿 dst_width/dst_height 当节点盒子，wlr_scene_surface 再按表面尺寸卡一遍
	 * 边界 —— 所以这个盒子必须跟着缩放走，不然全景里点谁都不是谁（实测 3 列时
	 * 最后一个窗口的盒子从 646 一路伸到 1291，把整排吞掉）。src_box 不是原因：
	 * 它一直是整块缓冲（实测 0,0 665x740）。
	 *
	 * 1:1 尺寸走 view_mini_native()，不走 base_dst_*：那个字段 xdg 从来不记
	 * （只在 X11 的 commit 和「退出小窗」两处写），恒为 0，下面这个守卫根本进不来
	 * —— 这正是「xdg 窗口在全景里内容尺寸一次都没缩过」的原因。view_mini_native()
	 * 是唯一一处按 kind 分派的 1:1 尺寸来源（小窗那套缩放用的也是它）：非 X11 读
	 * 表面当前逻辑尺寸（随时读都是当前值），X11 读 base_dst_*（已除过
	 * xwayland_scale）—— X11 这边行为因此一个字没变。
	 *
	 * 偏移（内容节点在它父树里的位置 = xdg 的 CSD 留白）也要一起缩：只缩尺寸不缩
	 * 偏移，盒子会整体往左上偏 ~20px、右边还盖不满格子（实测右侧差 2px、左侧多出
	 * 20px 伸进两列之间的缝里）。X11 的窗口几何 x/y 是 0，这条对它天然是空操作。
	 *
	 * 小窗（MINI / ICON）不进这一支：那两态的**内容节点几何是小窗那套机制管的**
	 * （min 尺寸按 mini 缩放、位置在盒子居中，见上面 mini_scaled 那段 + 图标快照），
	 * 而且小窗不参与行布局、全景里不重映射（vidow_overview_map 对它原样返回）——
	 * 照这里乘 ov 会把盒子撑到 306x340，比小窗那块卡片还高，白盖掉旁边一列的
	 * 一条（实测 14px 宽、160px 高）。X11 那一支保持原样：它读的就是 base_dst_*，
	 * 和小窗态的既有行为一致。 */
	int nw = 0, nh = 0;
	view_mini_native(view, &nw, &nh);
	if (ov != 1.0 && nw > 0 && nh > 0 &&
			(view->mini == VIDOW_MINI_NONE || view->kind == VIDOW_VIEW_X11) &&
			view->mini_snapshot == NULL) {
		int dw = (int)lround(nw * ov);
		int dh = (int)lround(nh * ov);
		wlr_scene_buffer_set_dest_size(src, dw > 0 ? dw : 1, dh > 0 ? dh : 1);
		if (view->kind != VIDOW_VIEW_X11) {
			/* 目标是「内容节点在场景里的绝对位置 = 窗口矩形原点 - 留白 ×
			 * 缩放」—— 留白（xdg 的窗口几何偏移）不一定挂在缓冲节点自己身上：
			 * 它是 wlroots 设在这一层还是父树那一层，得减掉父节点的偏移才算
			 * 得对（和上面小窗居中那段同一个算法）。只按「节点自己的 x」设就
			 * 会在父树再叠一份 -20px。 */
			struct wlr_box geo = vidow_view_geometry(view);
			int vx = 0, vy = 0, px = 0, py = 0;
			wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy);
			if (src->node.parent != NULL) {
				wlr_scene_node_coords(&src->node.parent->node, &px, &py);
			}
			wlr_scene_node_set_position(&src->node,
				(int)lround((double)vx - (double)geo.x * ov) - px,
				(int)lround((double)vy - (double)geo.y * ov) - py);
		}
	}
	int vx = 0, vy = 0, sx = 0, sy = 0;
	wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy);
	wlr_scene_node_coords(&src->node, &sx, &sy);
	view->fx_dx = sx - vx;
	view->fx_dy = sy - vy;
}

/* 只摆位置（给拖动跟手那条路用，见 drag.c）。
 *
 * **位置变了就得让装饰重烘一次。**
 *
 * 装饰图里烘死了窗口在**本屏上的位置**（decor_geo 的 origin_x / origin_y，
 * 进着色器的 u_origin —— 底衬模糊按它算采样 UV）。窗口一挪，这个 UV 就错
 * 了：屏幕上表现为底衬整块横向错位 / 一条条横纹（就是最初那个跨屏 bug）。
 * 所以位置一变必须重烘，光挪节点不够。
 *
 * 缓存键（fx.c 的 view_bake_for_output）里**没有窗口位置** —— 因为平时位置
 * 只在 vidow_view_apply_anim 里变，那里会推进 bake_revision。拖拽走的是这条
 * 「只摆位置」的轻路，于是拖动全程缓存判据一个都不变，这块屏的装饰被判
 * 「还是这一版」、永远不重烘，节点停在上一次烘出来的位置。
 *
 * 推进一格就够：它让「这一帧的位置」和「烘这张图时的位置」区分开，
 * 下一帧自然重烘一次。不推进 blur_ver（模糊贴图没变，不用重抓）。
 *
 * 试过「位置变了只挪节点、不重烘」（2026-10-27）：跨屏拖拽当场回归到最初的
 * 底衬错位，因为 u_origin 是烘死的。真要省这笔钱，得把 u_origin 从烘死的
 * uniform 改成每帧传进去的 —— 那是另一层改动。 */
void vidow_view_apply_anim_place(struct vidow_view *view) {
	view->fx.bake_revision++;
	view_anim_place(view);
}

/* 全屏直通：这一帧这个窗口能不能整块交给场景直画（不烘装饰那趟离屏渲染），让
 * wlroots 有机会把客户端的缓冲直接扫上屏（判据见 fx.c 的 vidow_view_scanout_ok，
 * wlroots 那一侧的要求见 wlr_scene.c 的 scene_entry_try_direct_scanout）。
 *
 * 返回 true = 这一帧按直通处理，调用方什么都不用做了。
 *
 * 为什么来回切换看不出变化：判据成立的时候「合成出来的像素」就是「客户端缓冲
 * 本身」（装饰参数全零、内容整块不透明、不裁），所以
 *   * 进直通：先把真实表面露出来 —— 内容一样，装饰节点这一帧还盖在上面，看不出
 *     变化；同一帧把装饰节点收掉。
 *   * 退直通：先把装饰那条路接回来，下面照常烘一帧，**烘成了才**把表面藏起来
 *     （见函数尾那句 set_opacity），中间没有任何一帧是空的。
 * 两个方向都会让场景节点自己报损伤（改透明度 / 开关节点都算整块），全屏窗口那
 * 一块就是整块输出 —— 屏幕上不会留下上一帧的残影（那是「跳一下」的来源）。 */
static bool view_scanout_switch(struct vidow_view *view) {
	bool ok = vidow_view_scanout_ok(view);
	if (ok == view->fx.scanout) {
		return ok;
	}
	view->fx.scanout = ok;
	if (ok) {
		if (view->scene_surface != NULL &&
				view->scene_surface->buffer != NULL) {
			wlr_scene_buffer_set_opacity(view->scene_surface->buffer, 1.0f);
		}
		vidow_fx_end(view->server, &view->fx);
		wlr_log(WLR_INFO, "scanout: %s 交给场景直画（全屏 + 装饰全零）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	} else {
		/* 装饰那条路接回来：和 view_open_done / view_swap_decor 一个套路 ——
		 * 装饰开着就重建 fx 节点（自定义的 idle shader 再挂上）。 */
		struct vidow_config *cfg = &view->server->config;
		view->fx.view = view; /* 裁剪要按窗口算（见 struct vidow_fx_target） */
		if (vidow_fx_begin(view->server, &view->fx, view->scene_tree, NULL) &&
				cfg->shader_idle[0] != '\0') {
			vidow_fx_set_shader(view->server, &view->fx, cfg->shader_idle,
				VIDOW_SHADER_IDLE);
			view->fx.progress = 1.0;
		}
		wlr_log(WLR_INFO, "scanout: %s 回到合成（装饰 / 模糊 / 动画又要了）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	}
	return ok;
}

/* 重烘这个窗口的装饰图（含底衬采样）。贵，一帧一次。位置先用 view_anim_place
 * 摆好；这里把同一套位置参数再算一遍（几个整数，跟重烘比起来不算什么）。 */
void vidow_view_anim_bake(struct vidow_view *view) {
	if (view->scene_tree == NULL) {
		return;
	}
	/* 裁不裁、裁到哪儿，**这一帧**就得是最新的：重烘（下面 vidow_fx_frame）
	 * 把裁剪算进场景节点里，烘完再改就晚了一帧 —— 那一帧里越界的那一块照样
	 * 画在隔壁屏上（而且静止下来之后再也没有下一帧来纠正）。 */
	vidow_view_apply_clip(view);
	/* 直通那一路：不烘，整块交给场景（见上）。 */
	if (view_scanout_switch(view)) {
		/* **直通这条早退也要校 dest。**
		 *
		 * 下面那趟重烘（vidow_fx_frame）会把 dest 算成窗口逻辑尺寸；直通
		 * 一 return，就再没人管它，节点停在 wlroots 上一次 commit 设的
		 * **surface 逻辑尺寸**上（Steam 的 3808x2050，2 倍），右下一半落
		 * 在屏外 —— 屏幕上就是纯黑。
		 *
		 * 复现链（探针实测）：重开时 bake 打印「缓存命中跳过重烘」，说明
		 * 装饰图被判定仍然有效、这趟根本没进；能改 dest 的只剩这里。
		 * 首开不黑是因为首开走了重烘那条路，dest 被设成 1904。 */
		if (view->kind == VIDOW_VIEW_X11) {
			vidow_view_x11_scale(view);
		}
		return;
	}
	struct vidow_server *server = view->server;
	double ov = vidow_overview_scale(server);
	double alpha = view_overview_alpha(server, view);
	int x = 0, y = 0;
	/* 摆位那份（view_anim_place）怎么算的，这里就得怎么算：重烘的位置和缩放
	 * 必须跟场景节点上那套一致，差一个像素就是糊 / 错位。 */
	double sx_mapped = 0, sy_mapped = 0;
	vidow_overview_map(server, view, view->anim_off_x, view->anim_off_y,
		&sx_mapped, &sy_mapped);
	x = (int)lround(sx_mapped);
	y = (int)lround(sy_mapped);

	if (!view->fx.active || view->scene_surface == NULL) {
		return;
	}
	struct wlr_scene_buffer *src = view->scene_surface->buffer;
	if (src == NULL) {
		return;
	}
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);

	/* ext-background-effect：客户端要的那块才模糊。区域是 surface 局部坐标，
	 * 着色器里比的是窗口局部坐标 —— 两者差一个 CSD 留白偏移（没留白就是 0）。 */
	struct wlr_box region;
	if (vidow_bgeffect_blur_box(view->server,
			vidow_view_surface(view), &region)) {
		view->fx.blur_region_on = true;
		view->fx.blur_region[0] = (float)(region.x - geo.off_x);
		view->fx.blur_region[1] = (float)(region.y - geo.off_y);
		view->fx.blur_region[2] = (float)region.width;
		view->fx.blur_region[3] = (float)region.height;
	} else {
		view->fx.blur_region_on = false;
	}

	/* The decoration shader applies the fade itself, so the node stays opaque
	 * and the real surface is only kept alive for hit-testing. */
	/* 装饰节点摆在「可见窗口」左上角（视图树的原点就是它，见 vidow_fx_geo），
	 * 不在源缓冲的原点上：源缓冲多出来的那圈 CSD 留白落在装饰矩形外面。 */
	view->fx.view = view;
	view->fx.anim_scale = view->anim_scale;
	/* 小窗（mini window，S3）：装饰那条路也得跟着缩 —— 烘图那块画布是按
	 * 「可见窗口几何 × 倍率」开的，倍率里不含小窗系数的话，图标里的窗口还是
	 * 原生那么大一块（装饰节点盖在缩过的表面节点上，看着就是没缩）。
	 *
	 * 复用 overview 的那个 ov：它同时决定「画布开多大」和「节点画多大」，两个
	 * 一起乘上小窗系数，结果正好是「内容按比例缩到小窗盒」。out_scale（设备像素
	 * / 逻辑像素）= scale / ov 里两者同乘一个系数，所以输出倍率一点没变。
	 * 副作用只有一个：ov < 0.98 会关掉这一次渲染的底衬模糊 —— 缩到几十像素的
	 * 小窗本来也不该叠一层按原尺寸算的模糊（overview / MRU 缩略图同理）。 */
	double fx_ov = ov * view_mini_scale(view);
	/* 小窗（S3b）：装饰那条路也要**居中**。装饰节点画的是「内容」这一块
	 * （节点原点 = 内容左上角，见 fx.c 的 node_x = x - pad_ov），x/y 传的就是
	 * 内容相对窗口树原点的偏移 —— 留白两边平分才和表面那条路一致（不传的话
	 * 装饰会把内容画在盒子左上角，表面节点缩在中间也看不见）。
	 *
	 * 只在保留原生分辨率（mini_scaled）时有非零值：那正是盒子比内容大的时候。 */
	int mini_cx = 0, mini_cy = 0;
	if (view->mini_scaled) {
		mini_cx = (int)lround(((double)view->float_geom.width -
			(double)geo.w * fx_ov) / 2.0);
		mini_cy = (int)lround(((double)view->float_geom.height -
			(double)geo.h * fx_ov) / 2.0);
	}
	int64_t bake_t0 = vidow_now_ns();
	/* 这里以前有一行每次重烘就打的 DEBUG（node/geom/screen_box），日常使用是
	 * 每帧 × 每个在动的窗口一行 —— 排查摆放错位时临时加回来，别常驻。 */
	/* **这张装饰图是给哪块屏烘的，就按那块屏的倍率烘**（不是 view_scale）。
	 *
	 * view_scale 是「客户端 surface 该按几倍画」的答案，一块 wl_surface 只能有
	 * 一个 buffer_scale，所以跨屏拖拽时必须取两块屏里高的那个（选低的必然糊）。
	 * 装饰缓冲不一样：它是 per-output 的（见 server.h 的 struct vidow_blur_slot
	 * 的 dec* / dec_scale），每块屏各有一份。
	 *
	 * 以前这里无条件用 view_scale：窗口骑在 1x 的 DP-3 和 2x 的 DP-2 中间时，
	 * 两块屏的装饰都按 2 倍烘 —— 而底衬贴图的原点 / 尺寸（blur_out_*）是按
	 * **这块屏自己的设备像素**记的。2 倍的装饰去采 1 倍的底衬，UV 的除数和
	 * 实际贴图对不上，屏幕上就是：
	 *   - 屏交界处一条模糊彩色带（1x 那半的底衬被按 2 倍采样）；
	 *   - 跨屏那一半忽明忽暗（两块屏轮流烘、各自的 dec_scale 又都是 2，
	 *     缓存判据在两边来回失效 → 底衬闪烁）；
	 *   - 拖回来之后窗口丢半透明（装饰图被当成 2 倍尺寸贴到 1 倍节点上，
	 *     alpha 那一项被拉伸）。
	 *
	 * bake_output 是「这次烘是给哪块屏的」：归属屏（vidow_blur_update 里设）
	 * 或这一趟临时借的那块屏（view_bake_for_output 里设）。有它就按它的倍率，
	 * 没有（layer / 预览 / 关闭快照那些老路径）才退回 view_scale。 */
	double bake_scale = view_scale(view);
	if (view->fx.bake_output != NULL && view->fx.bake_output->scale > 0.0) {
		bake_scale = view->fx.bake_output->scale;
	}
	bool fx_drawn = vidow_fx_frame(view->server, &view->fx, src, &geo,
		mini_cx, mini_cy, x, y, alpha, view->server->focused == view,
		view->fullscreen, bake_scale * fx_ov, fx_ov, &view->rule_eff);
	/* 探针：整块窗口那么大的离屏渲染一共多少钱（见统计行的「重烘」；次数由
	 * vidow_fx_frame 的 vidow_stats.bakes 记）。拖动 / 滚动动画里每个在动的
	 * 窗口每帧都要烘一次，所以这项随窗口数线性涨。 */
	vidow_stats.bake_ns += vidow_now_ns() - bake_t0;
	/* 装饰图真的画上屏了才把真实表面藏起来（只留给命中测试）。没画出来
	 * 还藏就是窗口整个消失 —— 关掉 decor/shader 时踩过，屏幕一片黑。
	 * 拖着果冻的时候也一样：内容归 jelly.c 那个节点画。
	 *
	 * 这里**不要**再加「等开窗动画 alpha 走满才藏」之类的条件：真实表面是
	 * 一张直角矩形，装饰节点才有圆角和模糊底衬。半透明阶段把它留在上面，
	 * 方角就会盖住圆角、底衬也被这块不透明的表面挡掉 —— 表现就是 SSD 圆角
	 * 裁不出来、窗口背后的模糊变成错的（回滚过一次，就是这么坏的）。 */
	wlr_scene_buffer_set_opacity(src,
		(fx_drawn || view->mask_opacity || view->jelly != NULL) ? 0.0f : 1.0f);
}

void vidow_view_apply_anim(struct vidow_view *view) {
	/* Commits, placement and animation updates invalidate every output instance.
	 * The per-output refresh loop itself must not advance this revision. */
	if (getenv("VIDOW_PUMP_TRACE") != NULL) {
		wlr_log(WLR_INFO, "ANIMTRACE %s rev=%u blur=%u/%u anim=%d jelly=%d",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			view->fx.bake_revision, view->fx.blur_drawn,
			view->fx.blur_ver,
			view->anim_open != NULL ? 1 : 0, view->jelly != NULL ? 1 : 0);
	}
	view->fx.bake_revision++;
	view_anim_place(view);
	vidow_view_anim_bake(view);
}

/* 从 wlr_surface 找回它的窗口：文本输入和 ext-background-effect 都要用。
 * 子表面按根表面匹配，所以焦点在子表面上时也能找到。 */
struct vidow_view *vidow_view_from_surface(struct vidow_server *server,
		struct wlr_surface *surface) {
	if (surface == NULL) {
		return NULL;
	}
	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (vidow_view_surface(view) == root) {
			return view;
		}
	}
	return NULL;
}

static void view_move_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_view *view = data;
	/* 滑动两端都算损伤：漏了老位置，别的窗口底衬里会留着它上一帧的残影。 */
	struct wlr_box rest = view->geom;
	/* 果冻还在收尾（松手之后那几百毫秒）：屏幕上画的是果冻节点，比窗口框
	 * 大一圈 —— 损伤得按它算（见 jelly.c 的 vidow_jelly_pad）。 */
	double rest_pad = vidow_jelly_pad(view) + 8.0;
	rest.x -= (int)rest_pad;
	rest.y -= (int)rest_pad;
	rest.width += 2 * (int)rest_pad;
	rest.height += 2 * (int)rest_pad;
	view->anim_off_x = view->anim_from_x * (1.0 - t);
	view->anim_off_y = view->anim_from_y * (1.0 - t);
	vidow_backdrop_damage_box(view->server, &rest);
	vidow_backdrop_damage_view(view); /* 只有它自己在动 */
	vidow_view_apply_anim(view);
}

static void view_move_done(void *data, bool cancelled) {
	struct vidow_view *view = data;
	view->anim_move = NULL;
	if (!cancelled) {
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
	}
	vidow_view_apply_anim(view);
}

static void view_open_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_view *view = data;
	struct vidow_config *cfg = &view->server->config;
	/* 回弹的曲线会过冲（t 稍微越过 1）：位移和缩放跟着过冲才是回弹，透明度
	 * 不行 —— 负的 alpha 在场景节点上没定义。
	 *
	 * 入场这里透明度**照旧跟曲线走**：过冲只会让它提前到 1（早点变成不透明），
	 * 没有「提前结束」的问题。关闭那边正好反过来 —— 斜坡是 1 → close_alpha_to，
	 * 过冲等于提前结束，所以那边必须用线性进度（见 fx.c 的 closing_update）。 */
	view->anim_alpha = cfg->open_alpha_from +
		(1.0 - cfg->open_alpha_from) * t;
	if (view->anim_alpha < 0.0) {
		view->anim_alpha = 0.0;
	} else if (view->anim_alpha > 1.0) {
		view->anim_alpha = 1.0;
	}
	view->anim_off_y = cfg->open_slide_px * (1.0 - t);
	view->anim_scale = cfg->open_scale_from +
		(1.0 - cfg->open_scale_from) * t;
	view->fx.progress = t;
	/* 开窗动画（缩放 + 滑入）：静止那一块（缩放 1、偏移 0）也算损伤。 */
	{
		struct wlr_box rest = view->geom;
		vidow_backdrop_damage_box(view->server, &rest);
	}
	vidow_backdrop_damage_view(view); /* 只有它自己在动 */
	vidow_view_apply_anim(view);
}

static void view_open_done(void *data, bool cancelled) {
	(void)cancelled;
	struct vidow_view *view = data;
	struct vidow_server *server = view->server;
	struct vidow_config *cfg = &server->config;

	view->anim_open = NULL;
	view->anim_alpha = 1.0;
	view->anim_off_y = 0.0;
	view->anim_scale = 1.0;
	wlr_log(WLR_DEBUG, "open done %s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");

	/* Drop the opening shader but keep the window decorated. */
	if (cfg->shader_idle[0] != '\0' &&
			vidow_fx_set_shader(server, &view->fx, cfg->shader_idle,
				VIDOW_SHADER_IDLE)) {
		view->fx.progress = 1.0;
	} else {
		view->fx.shader = NULL;
		view->fx.phase = VIDOW_SHADER_NONE;
	}
	if (!view->fx.active) {
		vidow_fx_end(server, &view->fx);
	}
	vidow_view_apply_anim(view);
}

void vidow_view_open_anim(struct vidow_view *view) {
	struct vidow_config *cfg = &view->server->config;
	struct vidow_rule_anim anim;
	vidow_rule_anim(cfg, &view->rule_eff, false, &anim);

	/* 弹出物（X11 菜单 / 提示 / 下拉框）和 Wayland 的 xdg_popup 一样：不装饰、
	 * 不跑 shader、也不做入场动画。它是跟着操作「啪」地出现的一块小图，滑动
	 * 或淡入都显得慢半拍；而且它常在自己的位置上重开，动画会看着像窗口在飘。
	 * 这里只把场景节点摆到最终位置。
	 * 窗口规则 anim = false / open_ms = 0 是同一回事，只不过由用户点名
	 * （终端那类「开出来就该在」的窗口）。
	 *
	 * 但**装饰 / 模糊那条路（fx）照样得建**：窗口的背景模糊和边框都挂在 fx
	 * 节点上，不建 fx 这个窗口就永远没有模糊、也没有边框 —— 「不跑动画」不是
	 * 「不要 fx」。所以这里走 view_open_done()（它做的正是「没有入场动画的开窗
	 * 收尾」：换 idle shader、摆到位），而不是把窗口晾在裸表面状态。
	 * （`window_open_ms = 0` 一直走的就是这条路，test-blurpartial.sh 拿它当
	 * 默认配置跑；规则写 anim = false 之前会跳过 fx，等于顺手把模糊关了。） */
	if (view->popup || anim.open_ms <= 0) {
		vidow_anim_cancel(view->anim_open);
		view->anim_open = NULL;
		view->anim_alpha = 1.0;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		view->anim_scale = 1.0;
		if (view->popup) {
			vidow_view_apply_anim(view);
			return;
		}
		vidow_fx_begin(view->server, &view->fx, view->scene_tree,
			cfg->shader_open);
		view_open_done(view, true);
		return;
	}

	if (view->scene_surface != NULL && view->scene_surface->buffer != NULL) {
		int vx = 0, vy = 0, sx = 0, sy = 0;
		wlr_scene_node_coords(&view->scene_tree->node, &vx, &vy);
		wlr_scene_node_coords(&view->scene_surface->buffer->node, &sx, &sy);
		view->fx_dx = sx - vx;
		view->fx_dy = sy - vy;
	}

	view->anim_alpha = cfg->open_alpha_from;
	view->anim_scale = cfg->open_scale_from;
	/* 入场动画接管位置：把可能刚起的位移补间撤掉。新窗口 map 的同一帧里
	 * arrange 已经先跑过一遍（带着列偏移的变化），那会儿还没有入场动画，
	 * 位移补间就起来了 —— 两个动画抢同一组 anim_off、每帧互相覆盖，看着就是
	 * 「闪一下再弹一下」（上滑被抹成 0，还横着挪几像素）。 */
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	view->anim_off_x = 0.0;
	view->anim_off_y = cfg->open_slide_px;

	vidow_fx_begin(view->server, &view->fx, view->scene_tree, cfg->shader_open);

	vidow_anim_cancel(view->anim_open);
	view->anim_open = vidow_anim_start(view->server, "open",
		anim.open_ms, cfg->open_easing, view_open_update,
		view_open_done, view);
	if (view->anim_open == NULL) {
		view_open_done(view, true);
	}
}

/* The surface is gone by the time we get here, so the close animation runs on
 * a snapshot of the last committed buffer. */
static void view_snapshot_close(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct vidow_rule_anim anim;
	vidow_rule_anim(&server->config, &view->rule_eff, false, &anim);
	/* 弹出物不做关闭动画：菜单收起来就是收起来了（它常常在关掉的同一帧里被
	 * 重开或换成另一个菜单，淡出的残影会串到下一个菜单上）。 */
	if (view->popup || view->last_buffer == NULL || anim.close_ms <= 0) {
		/* 「怎么没有关闭动画」是收到过的反馈，而这里有三条不同的路都能让它
		 * 不出现（弹出物 / 没有最后一帧快照 / 时长是 0）。日志里说清楚是哪条，
		 * 不然只能靠猜（`vidowmsg windows` 里能查回 popup / float_mode）。 */
		wlr_log(WLR_DEBUG, "关闭动画跳过 %s：%s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			view->popup ? "弹出物（菜单 / 提示 / 下拉框）不做关闭动画" :
			view->last_buffer == NULL ? "手里没有最后一帧快照" :
			anim.on ? "close_ms = 0（配置或规则里关了）" :
				"规则写了 anim = false");
		return;
	}
	/* 快照是整块源缓冲（表面），可见窗口在里面偏着 —— 把偏移一起交给关闭动画，
	 * 否则 CSD 窗口淡出的时候会整体斜出去那圈留白。 */
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	if (geo.w <= 0 || geo.h <= 0 || geo.src_w <= 0 || geo.src_h <= 0) {
		/* 已经拆干净了（X11 的 surface 没了 / 还没 commit）：整块就是窗口。 */
		geo = (struct vidow_fx_geo){0};
		if (view->scene_surface != NULL && view->scene_surface->buffer != NULL) {
			struct wlr_scene_buffer *sb = view->scene_surface->buffer;
			if (sb->dst_width > 0) {
				geo.src_w = sb->dst_width;
			}
			if (sb->dst_height > 0) {
				geo.src_h = sb->dst_height;
			}
		}
		if (geo.src_w <= 0) {
			geo.src_w = view->geom.width;
		}
		if (geo.src_h <= 0) {
			geo.src_h = view->geom.height;
		}
		geo.w = geo.src_w;
		geo.h = geo.src_h;
	}
	int w = geo.src_w, h = geo.src_h;
	int cx = 0, cy = 0;
	/* 小窗（mini）：关闭动画画的是**上一帧那个小窗**，不是它原生的样子。
	 *
	 * 快照是整块源缓冲（原生尺寸），而小窗台上画着的那份是「原生 × contain 系数」
	 * 居中摆在盒子里（S3 的 view_mini_scale + S3b 的居中）。以前这条路上一个
	 * 小窗系数都没有：关掉一个 504x315 的自由小窗时，关闭动画第一帧直接按原生
	 * 649x724（含装饰留白）铺开 —— 用户报的「关闭小窗的时候也会显示原始大小」。
	 * 修法：画布 / 快照节点的尺寸都乘上 ms，位置挪到**内容**原点（盒子原点 +
	 * 居中偏移）—— 和 S5 的过渡动画取同一个「上一帧画在哪」。
	 *
	 * 纹理映射得显式写成「整块缓冲里可见窗口那一块」：原来的兜底公式拿 geo 的
	 * 偏移量当 uv 起点，而这里要把那个偏移按 ms 缩（留给**没装饰那条路**摆快照
	 * 节点用），不显式写就会把缩过的 geo 当成裁切。 */
	double mini_ms = view_mini_scale(view);
	if (mini_ms != 1.0 && mini_ms > 0.0 &&
			view->float_geom.width > 0 && view->float_geom.height > 0) {
		int off_x = geo.off_x, off_y = geo.off_y;
		if (geo.tex_du <= 0.0f || geo.tex_dv <= 0.0f) {
			geo.tex_u0 = geo.src_w > 0 ?
				(float)off_x / (float)geo.src_w : 0.0f;
			geo.tex_v0 = geo.src_h > 0 ?
				(float)off_y / (float)geo.src_h : 0.0f;
			geo.tex_du = geo.src_w > 0 ?
				(float)geo.w / (float)geo.src_w : 1.0f;
			geo.tex_dv = geo.src_h > 0 ?
				(float)geo.h / (float)geo.src_h : 1.0f;
		}
		cx = (int)lround(((double)view->float_geom.width -
			(double)geo.w * mini_ms) / 2.0);
		cy = (int)lround(((double)view->float_geom.height -
			(double)geo.h * mini_ms) / 2.0);
		geo.off_x = (int)lround((double)off_x * mini_ms);
		geo.off_y = (int)lround((double)off_y * mini_ms);
		geo.w = (int)lround((double)geo.w * mini_ms);
		geo.h = (int)lround((double)geo.h * mini_ms);
		if (geo.w < 1) {
			geo.w = 1;
		}
		if (geo.h < 1) {
			geo.h = 1;
		}
		w = (int)lround((double)w * mini_ms);
		h = (int)lround((double)h * mini_ms);
		/* 烘图倍率**不乘** ms：geo.w/h 已经是逻辑尺寸了，fx.c 那边
		 * 节点尺寸 = 画布像素 ÷ out_scale = geo.w + 2*pad，乘两遍就小一圈。
		 * 代价只有一个：装饰留白（pad）还是原生那点逻辑像素，140ms 的关闭
		 * 动画里那圈边框看着比静止时厚一点点。 */
	}
	vidow_closing_start(server, server->layer_toplevels, view->last_buffer,
		&geo, view->geom.x + (int)lround(view->anim_off_x) + cx,
		view->geom.y + (int)lround(view->anim_off_y) + cy, w, h,
		anim.close_ms, server->config.close_slide_px,
		true, server->config.shader_close, view_scale(view),
		server->config.close_easing, server->config.close_scale_to,
		&view->rule_eff, view->fullscreen);
}

/* 视口过渡是不是正在这块屏上跑（定义在 output_arrange 那一段：窗口摆位要靠
 * 它决定「横向归视口、自己不补间」）。 */
static bool viewport_sliding(struct vidow_server *server,
		struct wlr_output *output);

void vidow_place_view(struct vidow_view *view, int x, int y, int w, int h) {
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}

	struct vidow_config *cfg = &view->server->config;
	bool moved = view->geom.x != x || view->geom.y != y;
	bool resized = view->geom.width != w || view->geom.height != h;

	/* 「现在画在哪」必须在覆盖 geom 之前取：覆盖之后再拿 geom 算补间起点，
	 * 得到的是新位置减新位置 = 0，动画照跑 190ms / 30 帧但一格都不动。 */
	int drawn_x = view->geom.x + (int)lround(view->anim_off_x);
	int drawn_y = view->geom.y + (int)lround(view->anim_off_y);
	/* 补间起点就是这个值，**被 park 的视图也不例外**：park 只把节点摆到屏外
	 * 的停放位、不动 geom/anim_off（见 view_anim_place），而 park 的判据恰恰是
	 * "画出来的盒子跟自己的输出框一点都不沾"（vidow_view_should_park）—— 所以
	 * 滑出去时这个值必然是屏外**它进来的那一侧**上的一点，起点本来就对：滚回来
	 * 它跟条带一起平移，位移跟别的列一模一样，不会单独飞。
	 * 别按"起点落在输出框里就把它推到屏幕边上"去改（ee769a7 干过）：那个判据在
	 * **正常**情况 —— 它明明画在屏上、只是被布局挪了一格 —— 也成立，于是每滚一列
	 * 窗口都被瞬移到屏幕边再滑回来：往左滚时起点算成输出框右缘（看着"一直占着
	 * 右边那一列、像跟别的窗口换了位置"），往右滚时起点算成 框.x - 窗口宽，而全屏
	 * 视图的宽 == 整屏宽，那个值正好是屏幕最左边（用户报的"从最左边移入"）。
	 * 列就是列，这里不按 fullscreen 分支。 */
	struct wlr_box before = {
		.x = drawn_x, .y = drawn_y,
		.width = view->geom.width, .height = view->geom.height,
	};

	view->geom = (struct wlr_box){ .x = x, .y = y, .width = w, .height = h };
	/* 布局真的把它挪/缩了（或者第一次摆）：这一笔记在**它自己**头上。它自己的
	 * 私有底衬（抓的时候它藏着）不受影响；别的窗口因为世界代次前进而重抓 ——
	 * 拖动时每动一下就重摆一次，分账全靠这里（见 vidow_arrange）。
	 *
	 * 老位置也单独记一笔：别的窗口的底衬里画着它，它挪走了那张图就过期了 ——
	 * 只记新位置的话，一步跳出去老远（拖得快、或者工作区切换）就漏掉了老位置
	 * 那一块，别的窗口底衬里会留着它的残影。 */
	if (moved || resized || !view->placed) {
		/* 果冻还在收尾（拖完松手那一小段）：屏幕上画的是果冻节点，比窗口
		 * 框大一圈，老位置要按它算（见 jelly.c 的 vidow_jelly_pad）。 */
		double bpad = vidow_jelly_pad(view) + 8.0;
		struct wlr_box old = before;
		old.x -= (int)bpad;
		old.y -= (int)bpad;
		old.width += 2 * (int)bpad;
		old.height += 2 * (int)bpad;
		vidow_backdrop_damage_box(view->server, &old);
		vidow_backdrop_damage_view(view);
	}
	wlr_log(WLR_DEBUG, "place %s -> %d,%d %dx%d",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		x, y, w, h);
	/* 布局把它挪/缩了：订阅者（状态栏、脚本）要知道新几何。第一次摆放
	 * （placed == false）也报 —— 那是「它现在在哪儿」的第一份事实。 */
	if (view->mapped && (moved || resized || !view->placed)) {
		vidow_ipc_emit_window(view->server, "window-moved", view);
	}
	/* set_size 自己会判断客户端准备好了没有；平铺提示 X11 不吃。 */
	vidow_view_set_size(view, w, h);
	vidow_view_set_tiled(view, true);

	/* 正被鼠标拖着：位置归 drag.c 管（apply_anim 用 anim_off 把它摆在光标下面），
	 * 这里别起补间、也别把偏移清零，不然拖到一半会被 arrange 拽回去。 */
	if (view->dragging || view->server->grab_view == view) {
		/* 平铺拖拽（grab_mode == VIDOW_GRAB_TILE）也算：它不动 view->dragging，
		 * 只置 grab_view / drag_detached，以前这里漏了它 —— 光标越过另一块屏
		 * 的边界、触发贴边自动滚 / 重排之后，arrange 进来起一条摆放补间，
		 * 补间每帧把 anim_off_* 按 anim_from_* 重新插值，把拖拽写进去的偏移
		 * 盖掉，窗口就**钉在屏边界上不再跟光标**（用户报的「拖到一定距离
		 * 窗口就看不见了」）。判据和 view_drag_output() 保持一致：拖拽期间
		 * 位置只归拖拽那条路管。 */
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		vidow_view_apply_anim(view);
		return;
	}

	/* 浮动窗口正被鼠标拖着：位置 1:1 跟光标，别起补间。
	 *
	 * 指针每动一下就走到这儿（grab 改 float_geom → arrange → place），补间于是被
	 * 每帧重起一次：弹性缓动在 t≈0 那一段几乎不动，窗口**一直落在光标后面**
	 * （用户那份 dbg 日志里量出来的：拖 273px/s 时中位滞后 199px、最大 387px，
	 * 起拖的头 125ms 窗口干脆没动）。平铺窗口的拖拽（drag.c）本来就是「只动场景
	 * 节点、布局一行都不重排」，浮动窗口没理由不跟手。 */
	if (view->server->grab_view == view &&
			view->server->grab_mode == VIDOW_GRAB_MOVE) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	/* 小窗（mini window，S5）形态转换正跑着：位置 / 缩放归那条补间管 ——
	 * 它的起点是「上一帧画在哪」（float.c 的 mini_transition_start），这里只
	 * 更新 geom 并把节点摆出去。**绝不能把 anim_off 清零 / 起位移补间**：
	 * 清零等于中间帧瞬移到终点（判据 18 量的是这个）。 */
	if (view->mini_anim != NULL) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		vidow_view_apply_anim(view);
		return;
	}

	if (!view->placed) {
		view->placed = true;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	/* 入场动画正跑着：位置归它管。新窗口 map 的同一帧里 arrange 会跑好几遍
	 * （建列、抢焦点、视口滚动各一次），第二遍起就满足 moved 了 —— 于是位移
	 * 补间和入场动画抢同一组 anim_off，每帧互相覆盖：上滑被抹成 0、还横着挪
	 * 几像素，看着就是「闪一下再弹一下」。这里只更新 geom，offset 一个字都不碰
	 * （淡入那点时间差里看不出这几像素的位移）。 */
	if (view->anim_open != NULL) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		vidow_view_apply_anim(view);
		return;
	}

	/* 弹出物不做移动补间：它是「跟着手指出现」的一块图，滑动会显慢半拍
	 * （Wayland 的 xdg_popup 同样不走任何动画）。 */
	if (view->popup || cfg->window_move_ms <= 0) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	/* 列宽过渡正跑着：这一帧它右边那些列的位置变化**是过渡本身推出来的** ——
	 * 每帧重起一次位移补间的话它们会一直吊在过渡后面（补间刚走 5%，下一帧又
	 * 从那儿重新起一条 190ms 的），过渡结束后再自己飘到位，看着就是「隔壁那
	 * 几列慢半拍」。过渡期间直接落位：宽度本身是按曲线走的，它们跟着曲线走
	 * 正是「整条带子刚性跟着变」的样子。
	 *
	 * 放在 moved 判断**后面**：没动的窗口照旧走上面那条（手里正跑着位移补间
	 * 的那种不该被过渡掐掉）。 */
	/* 切工作区的过渡正跑着：这一帧所有窗口的纵向位移**是过渡本身推出来的** ——
	 * 每帧重起一次位移补间的话它们会一直吊在过渡后面（和上面列宽那条一个道理），
	 * 看着就是「工作区滑过去了、窗口还在原地飘」。过渡期间直接落位，整块工作区
	 * 刚性跟着曲线走。 */
	if (view->server->workspace_slide_n > 0) {
		/* **切工作区期间，窗口一律直接落位，不做位移补间。**
		 *
		 * 整块工作区是靠 ws->slide_y 平移的（过渡自己推），窗口各自再补一条
		 * 位移就变成「工作区在滑、里面的窗口还在自己挪」—— 两套动画叠在一起，
		 * 看着就是窗户没跟着工作区走。用户要的是「整块一起动」：窗口相对位置
		 * 一点不变，只有整块在平移。
		 *
		 * 横向也一起归零：切工作区那一下 viewport 会换成新工作区自己的那份
		 * （workspace.c 的 focus_workspace），新旧视口不同时窗口的 x 会跳一下 ——
		 * 这一跳发生在**新工作区还在屏外**的时刻（slide_start 里 to->slide_y
		 * 先设成 ±屏高），所以根本看不见。让它瞬时到位，之后整块滑进来，
		 * 窗口就已经在自己的位置上了。
		 *
		 * 以前这里是 `&& !x_moved`，横向一动就走下面的补间分支 —— 那正是
		 * 「切工作区时窗口自己挪一段」的来源。 */
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	if (view->server->column_anim_n > 0) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	/* **视口过渡正跑着：窗口直接落位，不跑位移补间。**
	 *
	 * 视口每帧都在动，窗口的 x 也跟着每帧变 —— 不掐补间的话每个窗口都会从
	 * 「上一帧画的位置」补一条 window_move_ms 的位移，而那条永远追不上，
	 * 于是整条带子看起来软塌塌地拖在后面（正是「窗口各走各的」那个观感）。
	 *
	 * 让视口负责全部横向位移、窗口只做刚体：相对位置一帧都不变。 */
	if (viewport_sliding(view->server, view->output)) {
		/* **这里不记底衬损伤。**
		 *
		 * 视口过渡期间这个函数是每帧、对每个窗口都调一遍的，而底衬损伤
		 * 会经 blur_note_damage() 分发给**所有** layer surface（noctalia
		 * 的 panel / notification 全在里面）。每个窗口各记一笔 = 每帧几十
		 * 笔 × 每个 layer，panel 的模糊底衬永远追不上，越堆越多最后卡死
		 * （用户：「layer 动画会卡，而且会死掉（noctalia」）。
		 *
		 * 整条带子横向平移，受影响的是一整块屏 —— 损伤统一由
		 * viewport_anim_update() 每帧记一笔整屏就够了。 */
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
		return;
	}

	/* 目标位置没变：这一趟布局没打算动它。
	 *
	 * **手里要是正跑着一个位移补间，那个补间就是朝这个位置跑的 —— 别掐它。**
	 * 以前这里不分青红皂白 cancel + anim_off = 0：而同一帧里 arrange 会跑好几
	 * 遍（关一个窗口那条路实测 3 遍：unmap 一次、摘列一次、换焦点一次），第二
	 * 遍就把上一遍刚起的补间清掉、偏移归零 —— 窗口当场落到终点，看起来就是
	 * 「关窗的时候隔壁那块瞬移」（用户报「窗口关闭的时候移动动画不够自然，好像
	 * 有点瞬移」）。没有补间在跑时照旧把残留偏移清掉（保持原来的语义）。
	 *
	 * ⚠️ **必须放在 viewport_sliding 那条之后**（da69176 把它放前面，回弹就
	 * 只在一部分窗口上出现）。
	 *
	 * 视口过渡期间这个函数是每帧、对每个窗口都调一遍的。视口每帧挪一点，但
	 * 有些窗口的 geom 恰好一帧都没变（它本来就在条带上的正确位置，或者这一帧
	 * 视口的位移被取整吃掉了）—— 那些窗口走到这条早退，**保留**手里的
	 * anim_move，于是它的 anim_off 和视口的过冲叠加在一起；而走了上面
	 * viewport_sliding 分支的窗口 anim_off 是 0，只有视口的过冲。两拨窗口
	 * 走的不是同一条曲线，观感就是「头尾几个有回弹、中间几个直接停住」
	 * （用户 2026-10-25：「回弹只有头尾有，中间几个窗口没有」）。
	 *
	 * 视口在动的时候，位置归视口管，这里一律早退、什么都不碰。 */
	if (!moved) {
		if (view->anim_move == NULL) {
			view->anim_off_x = 0.0;
			view->anim_off_y = 0.0;
		}
		vidow_view_apply_anim(view);
		return;
	}

	/* Tween from wherever the window is drawn right now, so a new layout
	 * change mid-flight does not snap it back. */
	view->anim_from_x = (double)(drawn_x - x);
	view->anim_from_y = (double)(drawn_y - y);

	vidow_anim_cancel(view->anim_move);
	view->anim_move = vidow_anim_start(view->server, "move",
		cfg->window_move_ms, cfg->window_move_easing,
		view_move_update, view_move_done, view);
	if (view->anim_move != NULL) {
		/* 切工作区的过渡里：纵向归过渡（起点 0），横向照常补间。 */
		if (view->server->workspace_slide_n > 0) {
			view->anim_from_y = 0.0;
		}
		wlr_log(WLR_DEBUG, "move tween %s: %d,%d -> %d,%d（%dms）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			drawn_x, drawn_y, x, y, cfg->window_move_ms);
	} else {
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		vidow_view_apply_anim(view);
	}
}

struct wlr_box vidow_work_area(struct vidow_server *server,
		struct wlr_output *output) {
	struct wlr_box area = {0};
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == output) {
			area = o->usable_area;
			break;
		}
	}
	if (area.width <= 0 || area.height <= 0) {
		wlr_output_layout_get_box(server->output_layout, output, &area);
	}

	/* 缩进可用区：窗口不贴屏幕边、也不贴状态栏。四边可以分别设
	 * （gaps_top/bottom/left/right 或一张表 gaps = { top = … }），
	 * 没设就一起用统一值 gaps。每块屏可以用 layout{} 覆盖（niri 的
	 * output { layout { … } }）：vidow_layout_for 算的就是「全局 + 这块屏的
	 * 覆盖」，所以这里读的是这块屏最终生效的那份值。 */
	struct vidow_layout lay;
	vidow_layout_for(server, output, &lay);
	int top = vidow_layout_gap_side(&lay, lay.gaps_top);
	int bottom = vidow_layout_gap_side(&lay, lay.gaps_bottom);
	int left = vidow_layout_gap_side(&lay, lay.gaps_left);
	int right = vidow_layout_gap_side(&lay, lay.gaps_right);
	area.x += left;
	area.y += top;
	area.width -= left + right;
	area.height -= top + bottom;
	/* 四边加起来超过屏幕就退化成 1px：宁可挤扁，也别给布局一个负的框。 */
	if (area.width < 1) {
		area.width = 1;
	}
	if (area.height < 1) {
		area.height = 1;
	}
	return area;
}

/* 这一帧该按哪个比例画（定义在下面「列宽过渡」那一段）：补间在跑就是补间当前
 * 值，否则就是逻辑值。arrange 在上面，先声明一下。 */
static double column_layout_fraction(const struct vidow_column *col);

/* 摆一块工作区的列（Pass 3 那一套）。
 *
 * slide_y：切工作区过渡里这块工作区被推开的纵向偏移（正常 0）。
 * viewport：用这块工作区自己的滚动位置 —— 过渡期间两块工作区各滚各的。
 * fullscreen_out：列里那个全屏窗口（过渡里被推开的那块传 NULL：它那块不参与
 *   「谁该压在别人上面」的收口，它正在走人）。 */
static void arrange_columns(struct wl_list *shown,
		const struct wlr_box *work, const struct wlr_box *full_box,
		int inner, double viewport, int slide_y,
		struct vidow_view **fullscreen_out) {
	struct vidow_column *col;
	wl_list_for_each(col, shown, link) {
		int x = work->x + col->offset - (int)viewport;
		/* 全屏的那块也**跟着条带走**：左边缘 = 它自己那条列画在屏上的位置，
		 * 尺寸给整块屏。它在条带上占一整屏宽（见 Pass 1 那个 slot），所以
		 * 聚焦旁边那条列时它整块滑到屏外、旁边的列并排露出来；它自己那条列
		 * 被聚焦时视口正好落在它的 offset 上 —— 左边缘 0，严丝合缝盖住整屏。
		 * 滑出去那半块由 view_clip_box 裁在自己那块屏里（不会画到隔壁显示器）。 */
		int fx = full_box->x + col->offset - (int)viewport;

		/* 记一笔「这块列画在屏上的哪一块」：拖动找落点（drag.c）要按**屏幕上
		 * 看得见的**几何算，而全屏那列占一整屏、整列最小化的宽度是 0 这些
		 * 规矩只有这里知道。 */
		bool col_full = column_has_fullscreen(col);
		col->screen_x = col_full ? fx : x;
		/* 列宽占满整块工作区 = dock 眼里的「最大化」（vidow 没有单独的
		 * maximize 概念，niri 也是拿整列宽度当的）。同步给 foreign-toplevel，
		 * 否则 dock 永远以为没占满、不自动隐藏。 */
		{
			struct vidow_view *mv;
			wl_list_for_each(mv, &col->views, column_link) {
				if (mv->ftm_handle != NULL) {
					wlr_foreign_toplevel_handle_v1_set_maximized(
						mv->ftm_handle,
						col_full || col->width_fraction >= 0.999);
				}
			}
		}
		col->slot_width = col_full ? work->width : col->width;

		int n = 0;
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			if (!view_shows(view)) {
				continue;
			}
			if (view->fullscreen && view->mapped) {
				if (fullscreen_out != NULL) {
					*fullscreen_out = view;
				}
				vidow_place_view(view, fx, full_box->y + slide_y,
					full_box->width, full_box->height);
				continue;
			}
			n++;
		}
		if (n == 0) {
			continue;
		}

		int y = work->y + slide_y;
		wl_list_for_each(view, &col->views, column_link) {
			if (!view_shows(view) || (view->fullscreen && view->mapped)) {
				continue;
			}
			int h = (work->height - (n - 1) * inner) / n;
			vidow_place_view(view, x, y, col->width, h);
			y += h + inner;
		}
	}
}

int vidow_workspace_preview_layout(struct vidow_server *server,
		struct vidow_workspace *ws, int work_w, int work_h,
		void (*cb)(struct vidow_view *view, int x, int y, int w, int h,
			void *data),
		void *data) {
	if (server == NULL || ws == NULL || work_w < 1 || work_h < 1) {
		return 0;
	}
	/* 这块工作区住的那块屏的布局值（没住屏 = 全局那份）。 */
	struct vidow_layout lay;
	vidow_layout_for(server, ws->output, &lay);
	int inner = vidow_layout_gap_inner(&lay);
	int cursor = 0;
	struct vidow_column *col;
	/* 就是上面 arrange_columns 那一套（Pass 1 + Pass 3），只是**不写进
	 * view->geom**、坐标从工作区左上角起算：overview 要摆的是藏起来的工作区，
	 * 它们从来没被 arrange 过 —— col->offset / view->geom 还是上次显示时的老值
	 * （窗口被挪进来时新列的 offset 甚至是 0），照着画会叠在一起。 */
	wl_list_for_each(col, &ws->columns, link) {
		/* 整列都最小化了：这条列不占位置（宽度 0），剩下的列自然合拢。 */
		if (!column_has_shown_views(col)) {
			continue;
		}
		int width = vidow_layout_column_width_px(&lay, work_w,
			column_layout_fraction(col));
		bool col_full = column_has_fullscreen(col);
		int x = cursor;
		int n = 0;
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			if (!view_shows(view)) {
				continue;
			}
			if (view->fullscreen && view->mapped) {
				if (cb != NULL) {
					cb(view, x, 0, work_w, work_h, data);
				}
				continue;
			}
			n++;
		}
		int y = 0;
		wl_list_for_each(view, &col->views, column_link) {
			if (!view_shows(view) || (view->fullscreen && view->mapped)) {
				continue;
			}
			int h = (work_h - (n - 1) * inner) / n;
			if (cb != NULL) {
				cb(view, x, y, width, h, data);
			}
			y += h + inner;
		}
		/* 全屏的那条列在条带上占**一整屏**（同 Pass 1）：条带宽度 / 滚动范围
		 * 都按它算，全景里它也就跟着滚。 */
		cursor += (col_full ? work_w : width) + inner;
	}
	return cursor > 0 ? cursor - inner : 0;
}

/* ------------------------------------------------ 条带横向滚动的过渡
 *
 * 换焦点时 focus_viewport_target() 会给一个新的视口值。原来是**直接赋值**，
 * 然后每个窗口各自补一条 window_move_ms 位移 —— 问题在于每条的起点是「它
 * 上次画的位置」，终点是它自己的新位置，各走各的距离，观感就是几个窗口在
 * 互相追（用户报的「窗口在聚焦过渡动画的时候作为一个整体」）。
 *
 * 改成让**视口**走曲线：每帧按当前视口摆窗口、窗口本身不跑位移补间。整条
 * 条带刚性平移，窗口之间的相对位置一帧都不会变。 */
struct vp_anim {
	struct vidow_server *server;
	struct vidow_output *output;
	/* 自己这条动画（vidow_anim_start 的返回值）。done 回调要靠它判断
	 * 「field 里挂的还是不是我」：改道路径（见 output_arrange 的
	 * viewport_want_slide 分支）是先把 field 换成新动画再 cancel 旧的，
	 * 而在动画 tick 外面 cancel 会**同步**调 done —— 无条件清 field 会把
	 * 刚起好的新动画的引用抹掉，之后 arrange 看到 NULL 就把视口瞬移到
	 * 终点（用户 2026-10-30：「没有完成过渡动画的时候再切换聚焦会直接
	 * 瞬移」）。 */
	struct vidow_anim *anim;
};

static void viewport_anim_update(void *data, double t, double linear) {
	(void)linear;
	struct vp_anim *a = data;
	if (a->output == NULL) {
		return;
	}
	double v = a->output->viewport_anim_from +
		(a->output->viewport_anim_to - a->output->viewport_anim_from) * t;
	a->output->viewport_x = v;
	/* **整块屏记一笔损伤就够，不要每个窗口各记一笔。**
	 *
	 * 整条带子在横向平移，受影响的本来就是一整块屏 —— 一帧记一笔，而不是
	 * 每个窗口各记一笔（那会经 blur_note_damage 分发给所有 layer surface，
	 * 每帧几十笔 × 每个 layer，panel 的模糊底衬永远追不上，最后卡死。用户：
	 * 「layer 动画会卡，而且会死掉（noctalia」）。
	 *
	 * 但**不能记给 layer surface**：layer 不跟着视口走，它们的位置和底衬
	 * 一点没变。以前这里用的是 vidow_backdrop_damage_box()（会记给 layer），
	 * 而 5b7d441 为了躲开上面那个卡死，干脆连窗口的损伤也一起不记了 —— 于是
	 * 窗口横移之后上一帧画在隔壁屏上的那一块没人擦，别的窗口底衬里留着它的
	 * 残影（用户：「窗口聚焦移动的时候也会漏一点内容到隔壁显示器」）。
	 *
	 * 正解是分开：记给窗口（别人的底衬里画着它，必须重抓），不记给 layer。 */
	struct wlr_box obox = {0};
	wlr_output_layout_get_box(a->server->output_layout,
		a->output->wlr_output, &obox);
	if (obox.width > 0 && obox.height > 0) {
		vidow_backdrop_damage_box_views_only(a->server, &obox);
	}
	/* **arrange 期间把「世界变了」那笔账挂起**（见 vidow_arrange 末尾）。
	 *
	 * 视口过渡每帧都要重摆一遍，而 vidow_arrange() 收口时会调
	 * vidow_backdrop_damage() → gen_scene++。gen_scene 是模糊金字塔的键
	 * （fx.c: oslot->blur_set.gen != gen_scene - self_gen 就算过期），
	 * 每帧 +1 = 每帧整块重烘一次模糊链（实测 1280x1280、levels=3）。
	 * 主线程 100%，Wayland 事件不再派发，Xwayland 收不到唤醒于是不
	 * accept 新连接 —— 表现为「X 卡住、X 应用打不开、自启动流程卡住」。
	 *
	 * 视口平移这件事本身由上面的 vidow_backdrop_damage_box 记了损伤，
	 * 足够让受影响的底衬重抓，不需要再推世界代次。 */
	a->server->arrange_skip_world_damage = true;
	vidow_arrange(a->server);
	a->server->arrange_skip_world_damage = false;
}

static void viewport_anim_done(void *data, bool cancelled) {
	struct vp_anim *a = data;
	if (a->output != NULL) {
		if (!cancelled && a->output->viewport_anim != NULL) {
			a->output->viewport_x = a->output->viewport_anim_to;
		}
		/* **先把这一帧摆完，再摘掉 viewport_anim。**
		 *
		 * 以前是反过来的：viewport_anim = NULL 之后才 vidow_arrange()。于是
		 * 这一趟 arrange 里 viewport_sliding() 已经是假，每个窗口都发现
		 * 「我的 geom 跟上一帧画的位置不一样」（视口从过冲位置落到精确终点，
		 * 差那一点点），于是各起一条 window_move_ms 的位移补间 —— 整条带子
		 * 滚到位之后，又慢悠悠补 480ms 的位移，回弹全堆在这一段。
		 *
		 * 用户 2026-10-25：「窗口回弹动画是在聚焦滚动完最后才跑的」「聚焦过渡
		 * 动画怎么不能一次性滚到目标，变成一个一个窗口过渡动画完了才能动」。
		 *
		 * 正确的语义：**视口过渡结束的那一帧，位置仍然归视口管**。摆完再摘，
		 * 窗口的 geom 就在这一帧落定，不产生残余位移差，也就不会起那条多余
		 * 的补间。 */
		if (!cancelled) {
			vidow_arrange(a->server);
		}
		/* 只在 field 还挂着**自己**的时候清：改道路径已经把 field 换成
		 * 新动画了（cancel 旧的会同步走到这里），清掉就是抹掉新动画 ——
		 * 之后 arrange 视口瞬移到终点。 */
		if (a->output->viewport_anim == a->anim) {
			a->output->viewport_anim = NULL;
		}
	}
	free(a);
}

/* 把视口从 from 滑到 to（毫秒 ms，0 = 直接到位）。 */
static void viewport_slide(struct vidow_server *server,
		struct vidow_output *output, double from, double to, int ms) {
	if (output == NULL) {
		return;
	}
	/* **改道就重起一条：从当前位置出发，时长给足 ms。**
	 *
	 * 这里先后试过两版「不重起、接着跑」，都不可行：
	 *   1. 只换目标、反解起点保位置连续 —— e(t0) 接近 1 时 (1-e(t0))→0，
	 *      反解出的起点炸成几万像素，视口**飞出屏幕**；
	 *   2. 按「距离 / 时长」估速度，把剩余距离折算成时间加到总时长上 ——
	 *      估速用的是整段平均速度，改道后该段变短、速度被低估，而时长
	 *      只许变长，连按几次就累加成几十秒，观感是「快速滚动**极慢**」。
	 *
	 * 现在回到可预测的语义：每次改道 = 一条**完整时长**的新动画，起点是
	 * 当前位置（调用方传进来的 from 就是 output->viewport_x），所以位置
	 * 不跳、时长不累积、速度恒定。
	 *
	 * 代价：快速连按时每小段都从 t=0 起步（停在曲线加速段），过冲幅度按
	 * 该段距离算，比单次慢速切要小。这是「幅度」与「可预测」之间的取舍，
	 * 这里选后者 —— 极慢和飞出屏幕是故障，幅度偏小只是观感。 */
	if (output->viewport_anim != NULL) {
		vidow_anim_cancel(output->viewport_anim);
		output->viewport_anim = NULL;
	}
	if (ms <= 0 || from == to) {
		output->viewport_x = to;
		return;
	}
	struct vp_anim *a = calloc(1, sizeof(*a));
	if (a == NULL) {
		output->viewport_x = to;
		return;
	}
	/* 曲线跟着配置走（`viewport_easing`），默认是弹簧。
	 *
	 * 弹簧和固定时长曲线的差别是根本性的：它的时长 T = −ln(ε)/β 由包络
	 * 衰减决定，**与距离无关** —— 走 100px 和走 3000px 是同一条曲线、同一个
	 * 时长；而 out_back 时长固定、过冲幅度按距离算，距离一短回弹就看不见
	 * （用户报「快速切换的幅度不如慢速」），拉长时长又会变成「快速滚动极慢」。
	 *
	 * 换成固定时长的曲线（out_back / bezier / linear…）时，时长仍取
	 * window_move_ms —— 也就是回到老行为，配置说了算。 */
	struct vidow_ease curve = server->config.viewport_easing;
	int dur = vidow_ease_duration_ms(&curve);
	if (dur <= 0) {
		dur = ms;
	}
	a->server = server;
	a->output = output;
	output->viewport_anim_from = from;
	output->viewport_anim_to = to;
	output->viewport_anim = vidow_anim_start(server, "视口滚动", dur, curve,
		viewport_anim_update, viewport_anim_done, a);
	a->anim = output->viewport_anim;
	if (output->viewport_anim == NULL) {
		output->viewport_x = to;
		free(a);
		return;
	}
	/* INFO 级：这条一帧最多打一行，而「滚动被打断成一段一段」这种问题只有
	 * 看见「一次预期 → 一串 tween」才能判断。用户报「每滚到一个窗口回弹一次」
	 * 时就是靠它定位的。 */
	wlr_log(WLR_INFO, "viewport tween: %.0f -> %.0f（%dms，%s，want_slide=%d）",
		from, to, dur,
		curve.kind == VIDOW_EASE_SPRING ? "弹簧" : "定时长",
		(int)output->viewport_want_slide);
}

static bool viewport_sliding(struct vidow_server *server,
		struct wlr_output *output) {
	if (output == NULL) {
		return false;
	}
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == output) {
			return o->viewport_anim != NULL;
		}
	}
	return false;
}

static void output_arrange(struct vidow_server *server,
		struct vidow_output *output) {
	struct wlr_box work = vidow_work_area(server, output->wlr_output);
	if (work.width < 1 || work.height < 1) {
		return;
	}

	/* 全屏窗口占的那块 = **整块屏**（不吃 gap、也不理状态栏的占位）—— niri 的
	 * 「全屏窗口盖住整个屏幕」。取布局框：它和窗口摆位是同一个坐标系（逻辑坐标，
	 * 缩放 / 旋转已经算进去了），view_clip_box / vidow_work_area 用的也是它。 */
	struct wlr_box full_box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output,
		&full_box);
	if (full_box.width < 1 || full_box.height < 1) {
		full_box = work; /* 布局里还查不到这块屏：退回工作区，别给个空框 */
	}

	/* 窗口之间（列与列、同列上下）的间距 = gaps_inner，没设就跟 gaps 一样。
	 * 每块屏可以用 layout{} 覆盖（niri 的 output { layout { … } }）：这里读的是
	 * 这块屏最终生效的那份值（全局 + 覆盖，见 vidow_layout_for）。 */
	struct vidow_layout lay;
	vidow_layout_for(server, output->wlr_output, &lay);
	int inner = vidow_layout_gap_inner(&lay);

	/* Pass 1: column widths and offsets, measured from the work area. */
	struct wl_list *shown = output_shown_columns(server, output->wlr_output);
	int cursor = 0;
	struct vidow_column *col;
	wl_list_for_each(col, shown, link) {
		/* 整列都最小化了：这条列从条带里消失（宽度 0、不占位置），剩下的列
		 * 自然合拢 —— 最小化的窗口自己还在列里，放回来就照旧。 */
		if (!column_has_shown_views(col)) {
			col->width = 0;
			/* slot_width 是 arrange_columns 写的；整列不占位时也得清零，
			 * 否则拖动找落点还会把这条空列当成条带上的一条缝（被拖窗口
			 * 已经 drag_detached，列里可能就剩它一个）。 */
			col->slot_width = 0;
			col->offset = cursor;
			continue;
		}
		/* 比例 → 像素用 vidow_column_width_px（连 gap 一起分那块区域）：
		 * 两条 0.5 连着中间的 gap 正好铺满可用区，不会多出一个 gap。
		 * 比例取 column_layout_fraction：列宽过渡在跑时是补间中间值。 */
		int width = vidow_layout_column_width_px(&lay, work.width,
			column_layout_fraction(col));
		col->width = width;
		col->offset = cursor;
		/* 全屏的那条列在条带上占**一整屏**：它画出来就是整块屏那么大，让位 /
		 * 滚走的距离自然也该是整块屏（用户报的「全屏的窗口也要参与滚动」）。
		 * 列自己的 width_fraction 不动 —— 退出全屏回来还是那个宽度。 */
		cursor += (column_has_fullscreen(col) ? work.width : width) + inner;
	}
	int total_width = cursor > 0 ? cursor - inner : 0;

	/* **视口滚动归谁管：切工作区那条过渡开始时清一次，之后不再掐。**
	 *
	 * 以前这里是每帧无条件掐掉：只要 workspace_slide_n > 0，正在跑的视口滚动
	 * 就取消。用意是「新工作区要整块滑进来，视口还在自己滚的话窗口会横着飘
	 * 一段」—— 那个顾虑只在**滑动开始那一帧**成立（视口里还留着切工作区之前
	 * 那次聚焦的残留滚动）。
	 *
	 * 但无条件掐的代价是：**滑动期间切窗口焦点，横向过渡动画完全消失**。
	 * 焦点换列要横向滚动 → viewport_want_slide 置位 → viewport_slide() 刚起
	 * 一条动画，下一帧走到这儿就被掐掉；同时 view_anim_place 的
	 * `workspace_slide_n > 0` 分支又把窗口位移补间清零。两条叠加 = 窗口直接
	 * 瞬移到终点（用户 2026-10-29：「切换工作区动画没结束的时候进行窗口焦点
	 * 切换，会失去几乎失去窗口过渡动画」）。
	 *
	 * 现在改成：残留滚动在 slide_start 里清一次（见 workspace.c），这里不再
	 * 干预。滑动期间新起的视口滚动照常跑 —— 纵向归 slide_y、横向归视口，
	 * 两者本来就不冲突。 */

	/* Pass 2: scroll the minimum amount that makes the focused column fully
	 * visible. Focus moves, the view follows. */
	double viewport = output->viewport_x;
	int max_viewport = total_width - work.width;
	if (max_viewport < 0) {
		max_viewport = 0;
	}
	/* 手指正拖着条带（三指横扫那种连续手势）：这期间视口是手指说了算 —— 不跑
	 * 下面那套「把焦点列滚回可见区」，不然刚把焦点列拖出屏幕就会被它拽回来。
	 * 松手那段吸附动画也走这里（gesture_drag 一直挂到动画结束）。 */
	if (output->gesture_drag) {
		viewport = output->gesture_viewport;
	} else if (output->drag_scroll) {
		/* 平铺拖拽期间视口归拖拽自己（贴边自动滚条带，见 drag.c 的
		 * vidow_tile_drag_autoscroll）：不能跑下面那套「把焦点列滚回可见区」
		 * —— 刚为了看落点滚出去，立刻就被它拽回来，等于滚不动。 */
		viewport = output->drag_viewport;
	} else {
		/* 视口要滚到新位置时**走过渡**，而不是瞬跳 + 每个窗口各自补位移。
		 * 见 viewport_slide 上面那段：整条带子刚性平移，窗口作为整体走。 */
		double target = focus_viewport_target(server, output, viewport,
			work.width);
		if (target < 0) {
			target = 0;
		}
		if (target > max_viewport) {
			target = max_viewport;
		}
		if (output->viewport_anim != NULL) {
			/* 过渡正跑着：这一帧**别覆盖**视口，让它按曲线走。
			 *
			 * ⚠️ **这里绝对不能因为 target 变了就重起动画**（以前就是
			 * 这么写的，结果是 CPU 打满、X 新连接进不来、整个会话假死）。
			 *
			 * 死循环链条：window_move_easing 默认 out_back，曲线会过冲，
			 * viewport_anim_update 把过冲值写进 viewport_x → 下一帧
			 * focus_viewport_target 拿过冲后的 viewport 算「最小滚动」，
			 * 过冲让 `right > target + work_width` 这类边界判断翻面 →
			 * 算出不同的 target → 这里重启动画 → viewport_slide 里
			 * vidow_anim_cancel + vidow_anim_start 把 start_ns 重置 →
			 * raw 永远涨不到 1.0 → 下一帧重复。动画永远走不完，每帧
			 * 重烘 1280x1280 的模糊链，主线程 100%，Wayland 事件不再
			 * 派发，Xwayland 收不到唤醒于是不 accept 新连接。
			 *
			 * 目标只该在**动画启动那一刻**定下来；跑到一半改主意属于
			 * 「用户又按了一次」—— 那条路走下面 viewport_want_slide
			 * 分支（那时 anim 已经是 NULL）。 */
			/* **这里必须把 want_slide 清掉。**
			 *
			 * 它是「下一次 arrange 让视口走过渡」的一次性许可，只有消费掉才
			 * 算数。以前这个分支不清，标志会一直留到动画跑完 ——
			 * viewport_anim_done() 里那句 vidow_arrange() 进来时 anim 已经
			 * 摘掉、want_slide 还是 true，于是**又起一条**视口过渡，滚完再
			 * 起下一条…… 一次聚焦被拆成一串动画，每滚一段回弹一次（用户
			 * 2026-10-25：「现在不是一次滚到目标，是每滚到一个窗口回弹一次」）。 */
			/* **动画期间又换了一次焦点列 = 中途改道，一条动画滚到底。**
			 *
			 * 用户 2026-10-26 两次反馈：
			 *   「现在窗口聚焦不能一口气移动到目标窗口了」
			 *   「不对啊 应该是丝滑到底 现在怎么是一个一个窗口过度」
			 *
			 * 上一版把丢掉的那次请求记成欠账、等动画跑完再补一条 —— 补出来
			 * 的效果就是「滚一段、停一下、再滚一段」，一个窗口一个窗口地过。
			 * 要的是一路上改道：接着现在这个位置继续滚向最新的目标。
			 *
			 * 上面那条「绝不能因为 target 变了就重启动画」的警告针对的是
			 * **每帧拿过冲后的视口重算 target** 那种触发：out_back 过冲让
			 * 边界判断翻面 → target 变 → 重启 → start_ns 重置 → raw 永远
			 * 到不了 1.0 → 每帧重启，主线程 100%。
			 *
			 * 这里的触发条件是 viewport_want_slide，它只在 vidow_view_focus()
			 * 真的换了焦点列时置一次（见 5071 行上面那段），不是每帧比较
			 * 出来的 —— 重启次数等于用户按键次数，天然有界。改完 want_slide
			 * 立刻清掉，下一帧 anim 还挂着但标志是假，走的还是「别覆盖」
			 * 那一支，不会再进这里。
			 *
			 * from 用 output->viewport_x，也就是动画当前（带过冲）的位置，
			 * 所以改道那一刻视口不跳，只是换个方向继续走。 */
			if (output->viewport_want_slide && target != viewport &&
					server->config.window_move_ms > 0) {
				output->viewport_want_slide = false;
				/* **先摘掉 field，再 cancel。**
				 *
				 * vidow_anim_cancel() 在 tick 回调里（现在就是从
				 * output_render → vidow_anim_tick 进来的）只标 cancelled、
				 * 不调 done，所以"先 cancel 后置 NULL"在 tick 里是安全的；
				 * 但这条路径随时可能从 tick 外面进来（arrange 也会被 IPC /
				 * 输入事件直接调到），那时 cancel 会同步调 done，done 里
				 * 又 arrange 一遍，重入的 arrange 看到 viewport_anim 还是
				 * 非 NULL 就走"别覆盖"分支。摘 field 再 cancel 两头都对：
				 * done 先看到 NULL，不会再回 arrange，cancel 那条也只是把
				 * 这一帧剩下的活收掉。 */
				struct vidow_anim *old = output->viewport_anim;
				output->viewport_anim = NULL;
				viewport_slide(server, output, viewport, target,
					server->config.window_move_ms);
				/* viewport_slide 已经起好新动画；这里一帧最多一行
				 * INFO，看见"一串 tween"才用得上。 */
				if (old != NULL) {
					vidow_anim_cancel(old);
				}
				viewport = output->viewport_x;
			} else {
				output->viewport_want_slide = false;
				viewport = output->viewport_x;
			}
		} else if (target != viewport && server->config.window_move_ms > 0 &&
				output->viewport_want_slide) {
			/* **只有「换了焦点列」才让视口走过渡。**
			 *
			 * 关窗 / 列宽变化 / 拖拽落位也会让视口需要挪，但那时候各列自己
			 * 在重排（列没了、宽度变了），该由每个窗口自己的位移补间表现
			 * ——「隔壁那几列滑过来补位」。这里再起一条视口过渡的话，
			 * viewport_sliding() 会让**所有**窗口直接落位，补位动画全没，
			 * 看着就是「关窗瞬间移动」。
			 *
			 * 用显式标记而不是比较 last_focused_col：那个字段在
			 * vidow_view_focus() 里就已经更新过了，等排到这儿早相等了，
			 * 判据永远为假。 */
			output->viewport_want_slide = false;
			viewport_slide(server, output, viewport, target,
				server->config.window_move_ms);
			viewport = output->viewport_x;
		} else {
			output->viewport_want_slide = false;
			viewport = target;
		}
	}
	/* **视口过渡在跑时不 clamp**：window_move_easing 默认是 out_back，
	 * 它的曲线本来就会过冲（1.0 + c3·u³ + c1·u² 在 t≈0.85 处冲过 1）。
	 * 那个过冲正是「回弹」的观感 —— 以前视口是瞬跳、回弹靠每个窗口的位移
	 * 补间自己做，所以看得见；现在横向位移全归视口，这里一 clamp 就把它
	 * 削平了，回弹整个消失。
	 *
	 * 动画期间放过冲值，落位时由 viewport_anim_done 写回精确的
	 * viewport_anim_to（那个值在启动时就 clamp 过了），所以最终位置不会
	 * 越界 —— 只是中间几帧允许画到条带外面一点。 */
	bool vp_animating = output->viewport_anim != NULL;
	if (!vp_animating) {
		if (viewport < 0) {
			viewport = 0;
		}
		if (viewport > max_viewport) {
			viewport = max_viewport;
		}
	}
	if (vidow_xtrace_on()) {
		wlr_log(WLR_INFO, "vp: %s viewport=%.0f（旧 %.0f）max=%d "
			"work=%dx%d total=%d focus=%s", output->wlr_output->name,
			viewport, output->viewport_x, max_viewport,
			work.width, work.height, total_width,
			server->focused != NULL ?
				vidow_view_title(server->focused) : "无");
	}
	output->viewport_x = viewport;

	/* 把最终视口写回**当前显示的那块工作区**。
	 *
	 * viewport_x 挂在 output 上（一块屏只有一个滚动位置），而每块工作区想记住
	 * 「自己离开时滚到哪」—— 那份记忆就是 ws->viewport_x。老代码只在切换的
	 * 那一瞬间抄一次（workspace.c: prev->viewport_x = out->viewport_x），可那一刻
	 * 抄到的是**上一次 arrange 之后**的值：焦点跟随、新窗口插入、列宽变化都会
	 * 让 arrange 把视口滚到别处，于是抄下去的是个陈旧值，切回来的位置就不对 ——
	 * 用户看到的「切工作区窗口自己动一下」。
	 *
	 * 所以在这里（所有滚动都算完、clamp 过之后）落账：谁显示着，谁的 viewport_x
	 * 就是屏幕上这个值。切换时那份快照只是兜底，真值一直跟着屏幕走。 */
	struct vidow_workspace *vp_ws =
		vidow_workspace_shown_for(server, output->wlr_output);
	if (vp_ws != NULL) {
		vp_ws->viewport_x = viewport;
	}

	/* Pass 3: place the tiled windows. Floating views are placed afterwards
	 * so that a follow-float can read its parent's fresh tile geometry.
	 * 最小化的窗口不摆放（它原来的几何留着，放回来那次 arrange 会重算），
	 * 也不参与 n —— 同列剩下的窗口因此自己撑满。 */
	struct vidow_view *fullscreen = NULL;
	struct vidow_workspace *shown_ws =
		vidow_workspace_shown_for(server, output->wlr_output);
	/* 切工作区的过渡里被推开的那块先摆：它和现在这块都占着同一块屏，谁后摆
	 * 谁留下的 col->screen_x / slot_width 就归谁用（拖动找落点读的是这两个），
	 * 这两个值该是**当前这块**工作区的。 */
	if (output->slide_from != NULL) {
		arrange_columns(&output->slide_from->columns, &work, &full_box,
			inner, output->slide_from->viewport_x,
			(int)lround(output->slide_from->slide_y), NULL);
	}
	arrange_columns(shown, &work, &full_box, inner, viewport,
		shown_ws != NULL ? (int)lround(shown_ws->slide_y) : 0,
		&fullscreen);
	vidow_float_place_all(server, output);

	/* 浮动的全屏窗口也要占满**整块屏**（和列里的全屏同一个 full_box）：以前只有
	 * 「列里的全屏」会被摆成整屏，浮动的那种「全屏」在层级上照样永远压在最上面
	 * （vidow_restack 只认 fullscreen 标志），画面上却还是它自己那一小块 ——
	 * 用户看到的就是「它一直叠在最上面，还不占满屏」。几何只进 geom
	 * （vidow_place_view 不碰 float_geom），退出全屏时照旧回原尺寸。 */
	if (fullscreen == NULL) {
		struct vidow_view *v;
		wl_list_for_each(v, &server->views, link) {
			if (v->fullscreen && v->mapped && view_shows(v) &&
					v->float_mode != VIDOW_FLOAT_NONE && !v->popup &&
					v->output == output->wlr_output &&
					vidow_workspace_view_visible(server, v)) {
				fullscreen = v;
				break;
			}
		}
		/* 只有**浮动**的全屏在这儿摆：列里那些在上面的 Pass 3 就摆完了（它们
		 * 的位置跟着条带走、尺寸给整块屏），这里再摆一次会把那一份盖掉。 */
		if (fullscreen != NULL) {
			vidow_place_view(fullscreen, full_box.x, full_box.y,
				full_box.width, full_box.height);
		}
	}

	if (fullscreen != NULL) {
		/* 压在别人上面 / 从别人上面挪开：底衬的合成结果变了，账记在它头上。 */
		vidow_backdrop_damage_view(fullscreen);
		wlr_scene_node_raise_to_top(&fullscreen->scene_tree->node);
	}

	/* 按需 VRR（niri 的 variable-refresh-rate on-demand）：这块屏现在有没有
	 * 「要 VRR 的窗口显示着」—— 有就把刷新率提上去，没有就降回来。没开按需
	 * 模式的屏在函数第一行就返回了，所以这里等于白调一次。 */
	vidow_output_vrr_on_demand_update(server, output->wlr_output);
}

void vidow_arrange(struct vidow_server *server) {
	struct vidow_output *output;
	/* 这一趟的精确损伤从零开始攒（见 fx.c 的 blur_note_damage 和 server.h 的
	 * arrange_damage）：每个窗口位置变化时 place_view 已经记过新旧位置，收口
	 * 时不必再整屏算一遍。整屏标脏（restack 真换了层级 / layer 开关 / 背景
	 * 重画）会把 arrange_force_full 置上，收口时回退整屏 —— 宁可多抓一次，
	 * 不能糊着旧的。设置 VIDOW_NO_ARRANGE_REGION=1 可强制回到整屏（A/B 对比）。 */
	server->arrange_in_progress = true;
	server->arrange_damage_any = false;
	server->arrange_force_full = false;
	/* 先把住址对齐一遍：窗口 / 列住哪块屏只能由它们所在的工作区说了算。
	 * 切工作区、规则搬屏、热插拔、拖动跨屏都会改到一半，漏一处就会让滚动
	 * 越界的窗口按几何框算到隔壁屏的工作区里。 */
	vidow_workspace_sync_outputs(server);
	/* 桌面底色跟着输出布局走（拔线 / 改位置 / 改分辨率都会走到这儿）。 */
	vidow_background_apply(server);
	/* Moving windows changes what is behind every other window.
	 *
	 * 但拖动的时候每动一下都会重摆一次（被拖的那个窗口的 geom 跟着光标走）：
	 * 这种「只有它自己在动」的重摆，每一笔都已经由 place_view / 可见性翻转 /
	 * 置顶那几处记在**它自己头上**了（vidow_backdrop_damage_view）——它自己的
	 * 私有底衬抓的时候把它藏起来了，不受影响。这里要是照样把整个世界代次推一
	 * 遍，拖一个重叠的窗口时它自己的底衬每帧都会被判过期，白重抓一遍（基准实测
	 * 每帧 2 次重抓里的一次）。
	 *
	 * 「是不是只有它」：这一趟里只要有任何一个**不是正被拖的那个**窗口报了自己
	 * 变了，标志就被清掉，退回「世界变了」—— 宁可多抓一次，不能糊着旧的。 */
	server->arrange_self_only = server->grab_view != NULL;
	wl_list_for_each(output, &server->outputs, link) {
		output_arrange(server, output);
	}
	/* 弹出物不进布局（它们的位置尺寸由客户端给死），所以放在所有输出之后单独
	 * 摆一遍 —— 也正因为不看输出，菜单能开在任意一块屏上。 */
	vidow_popup_place_all(server);

	/* 位置变了就要重判一次「谁该出现在场景里」：滚动把一列推出视口之后，它得
	 * 立刻从场景里摘掉，不然它的坐标落进隔壁屏的框里，就画到隔壁显示器上了
	 * （见 vidow_view_in_output_box）。 */
	vidow_workspace_apply_visibility(server);
	/* 全屏的时候把占位的 top layer（状态栏）藏掉：它看不见了，但输入命中
	 * 还认它，屏边上就留一条点不动的死区；退出全屏再放回来。 */
	vidow_layers_update_fullscreen(server);

	/* 摆完了收口一次层级：新窗口是「挂到最后」建出来的（在场景树里就是最上面），
	 * 浮窗得重新压回平铺之上；浮 ⇄ 平铺切换、全屏进出也都在这一趟里摆正（见
	 * vidow_restack —— 没人站错队时它一个节点都不碰）。 */
	vidow_restack(server, NULL);

	/* 收口：这一趟攒下来的精确区域已经在各自调用点记给每个底衬了
	 * （blur_note_damage），这里只负责「世界代次」和「有没有漏网的」：
	 *
	 *   force_full  这一趟里有人标过整屏（restack 真换了层级 / layer 开关 /
	 *               背景重画）—— 那一笔已经覆盖了，别重复；
	 *   有累计区域   开窗 / 关窗 / 切工作区这类「窗口位置变了」：只把真实
	 *               变过的并集推一遍，别的窗口的底衬不受影响；
	 *   什么都没有   纯配置变化（gap / 规则）：保守整屏。
	 *
	 * VIDOW_NO_ARRANGE_REGION=1 强制整屏（A/B 对比 / 出事时的逃生口）。 */
	bool use_region = !server->arrange_self_only &&
		!server->arrange_skip_world_damage &&
		getenv("VIDOW_NO_ARRANGE_REGION") == NULL;
	bool force_full = server->arrange_force_full;
	bool any_damage = server->arrange_damage_any;
	struct wlr_box region = server->arrange_damage;
	server->arrange_in_progress = false;
	server->arrange_damage_any = false;
	server->arrange_force_full = false;
	if (use_region) {
		if (force_full) {
			/* 已经有人标过整屏。 */
		} else if (any_damage) {
			vidow_backdrop_damage_region(server, &region);
		} else {
			/* **这一趟什么都没变** —— 窗口位置 / 尺寸 / 层级都没动，只是
			 * 有人顺手调了一次 arrange。
			 *
			 * 以前这里保守推整屏，于是「谁每帧调一次 arrange」就自动变成
			 * 「每帧整屏重绘」：实测静止桌面 190fps、损伤 99%/帧、layer
			 * 模糊每帧重建（它挂在 gen_scene 上），GPU 一直被占满，抓帧
			 * （vshot）抢不到资源就慢 4 倍 / 卡半秒 —— 用户报的「vshot 截
			 * 显示器还是卡」的真正来源就是这里，不是抓帧本身。
			 *
			 * 真正的配置变化（gap / 规则 / output 参数）在改动的那一处自己
			 * 标 arrange_force_full，走上面第一支；开窗 / 关窗 / 切工作区
			 * 会累计到 any_damage。所以「没变化」就该什么都不做。 */
		}
	} else if (!server->arrange_self_only &&
			!server->arrange_skip_world_damage) {
		vidow_backdrop_damage(server, false);
	}
	server->arrange_self_only = false;
}

/* ------------------------------------------------------------- 热插拔 */

/* 一块屏拔了 / 回来了：列、浮动窗口、layer surface 的「归属」记在
 * vidow_output_memory 里，这里只动「现在摆在哪块屏上」。详见 hotplug.h。 */

/* 这个窗口一定属于哪块屏（记忆）。窗口一建出来就记上，浮动窗口靠它回到原来的屏。 */
static struct vidow_output_memory *home_of(struct vidow_server *server,
		struct wlr_output *wlr_output) {
	if (wlr_output == NULL) {
		return NULL;
	}
	return vidow_output_memory_for(&server->output_memories, wlr_output->name);
}

/* 指针还活着吗 —— 记忆里存的是裸指针，可能早就被关掉了，用之前必须验一遍
 * （只比较，不解引用；只有真在 views 列表里才认）。 */
static bool view_alive(struct vidow_server *server, struct vidow_view *want) {
	if (want == NULL) {
		return false;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view == want) {
			return true;
		}
	}
	return false;
}

/* 工作区挂在哪个更新器上。配置可以给每块工作区指定「属于哪块屏」，工作区里
 * 的列就看这一条。 */
static bool ws_on_memory(struct vidow_workspace *ws,
		struct vidow_output_memory *memory) {
	if (ws->output == NULL || memory == NULL) {
		return false;
	}
	char key[VIDOW_OUTPUT_KEY_MAX];
	vidow_output_key(ws->output->name, key, sizeof(key));
	return strcmp(key, memory->key) == 0;
}

/* 把「属于 memory 这块屏」的工作区里的列整组搬到 to 上住（to 可以是 NULL =
 * 暂时没屏，列就挂着不动）。拔线时用：工作区名不变，列换了屏；插回来时按同一
 * 组名字搬回去，所以列的顺序、列宽、滚动位置都还在。
 *
 * to_memory：搬到哪块屏的**同名工作区**上（屏 A 的 "1" -> 屏 B 的 "1"；
 * 没有同名工作区就现场建一块）。这样多屏的时候布局不会挤成一团。 */
static void columns_migrate(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *to,
		struct vidow_output_memory *to_memory, bool adopt_orphans,
		size_t *out_columns, size_t *out_views) {
	*out_columns = 0;
	*out_views = 0;

	/* 先把要搬的列收集起来：搬运过程中会在工作区之间挪动链表。 */
	size_t n = 0;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special) {
			continue;
		}
		if (!(ws_on_memory(ws, memory) ||
				(adopt_orphans && ws->output == NULL))) {
			continue;
		}
		struct vidow_column *col;
		wl_list_for_each(col, &ws->columns, link) {
			n++;
		}
	}
	if (n == 0) {
		return;
	}

	struct vidow_column **cols = calloc(n, sizeof(*cols));
	if (cols == NULL) {
		return;
	}
	size_t i = 0;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special || !(ws_on_memory(ws, memory) ||
				(adopt_orphans && ws->output == NULL))) {
			continue;
		}
		struct vidow_column *col;
		wl_list_for_each(col, &ws->columns, link) {
			if (i < n) {
				cols[i++] = col;
			}
		}
	}

	for (i = 0; i < n; i++) {
		struct vidow_column *col = cols[i];
		/* 目的工作区按**名字**认：同一套名字在别的屏上是另一块工作区。 */
		struct vidow_workspace *target = NULL;
		if (to_memory != NULL) {
			wl_list_for_each(ws, &server->workspaces, link) {
				if (!ws->special &&
						ws_on_memory(ws, to_memory) &&
						strcmp(ws->name, col->ws != NULL ?
							col->ws->name : "") == 0) {
					target = ws;
					break;
				}
			}
			if (target == NULL) {
				target = vidow_workspace_create(server,
					col->ws != NULL ? col->ws->name : NULL, to);
			}
		}
		if (target == NULL) {
			continue; /* 没有接收的屏：列原地待着，屏回来再搬 */
		}
		wl_list_remove(&col->link);
		wl_list_insert(target->columns.prev, &col->link);
		col->ws = target;
		col->output = to;
		struct vidow_view *view;
		wl_list_for_each(view, &col->views, column_link) {
			/* 窗口跟着列走：工作区指针也要改。只改 output 的话 view->ws 还
			 * 指着搬走前那块工作区 —— 可见性按 ws 判，窗口在新屏上永远不显示，
			 * 而 IPC / overview 又按 output 说它在新屏，两边对不上。 */
			view->ws = target;
			view->output = to;
			(*out_views)++;
		}
		(*out_columns)++;
	}
	free(cols);
}

/* 这块屏上现在有多少列 / 多少个窗口（拔线日志要报的是这个数：工作区整块跟着
 * 列走，所以「搬了几列」在搬家之后往往已经是 0 —— 报原始数量才不会误导）。 */
static void count_output_columns(struct vidow_server *server,
		struct wlr_output *output, size_t *cols, size_t *views) {
	*cols = 0;
	*views = 0;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->special || ws->output != output) {
			continue;
		}
		struct vidow_column *col;
		wl_list_for_each(col, &ws->columns, link) {
			(*cols)++;
			*views += (size_t)wl_list_length(&col->views);
		}
	}
}

void vidow_view_output_gone(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *dead,
		struct vidow_output *fallback) {
	struct wlr_output *to = fallback != NULL ? fallback->wlr_output : NULL;

	/* overview 里每块屏各有一份状态（行表 / 遮罩 / 底衬），都挂在 vidow_output
	 * 上 —— 这块屏一拔，那份状态跟着结构体一起没了。主屏（overview.output）
	 * 没了就把整个 overview 收掉，不然下一帧在读已经释放的屏。 */
	if (server->overview.output == dead) {
		server->overview.output = NULL;
		vidow_overview_exit(server);
	}

	size_t on_screen_cols = 0, on_screen_views = 0;
	count_output_columns(server, dead, &on_screen_cols, &on_screen_views);

	/* 先把「属于这块屏」的工作区整块交给接收的屏 —— 工作区是屏上的东西，屏没了
	 * 它就跟过去（名字不变），里面的列一列都不用动。之后按名字把列补回对应的
	 * 工作区（同名工作区已经合并到接收屏上那份了）。 */
	vidow_workspace_output_gone(server, dead, to);
	size_t cols = 0, in_columns = 0;
	struct vidow_output_memory *to_memory = fallback != NULL ?
		vidow_output_memory_for(&server->output_memories,
			fallback->wlr_output->name) : NULL;
	columns_migrate(server, memory, to, to_memory, false, &cols, &in_columns);

	size_t floats = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->column != NULL) {
			continue; /* 列里的窗口跟着列走过了 */
		}
		struct vidow_output_memory *home = view->ws != NULL ?
			view->ws->home : home_of(server, view->output);
		if (view->output != dead && home != memory) {
			continue;
		}
		bool movable = !view->popup &&
			view->float_mode != VIDOW_FLOAT_FOLLOW && view->placed;
		if (movable) {
			/* 「相对自己那块屏原点」的偏移（home_dx/home_dy）。位置本身不在这里摆：
			 * 拔掉一块屏之后 wlroots 会重排剩下的输出，这一刻问到的目的屏布局位置
			 * 还是旧的，等下一次摆放（vidow_float_place_all）时再算才准。 */
			view->home_dx = view->float_geom.x - memory->last_box.x;
			view->home_dy = view->float_geom.y - memory->last_box.y;
			floats++;
		}
		view->output = to;
		/* 位置不用在这里算：float.c 的摆放会把「相对偏移」换算成新屏上的绝对
		 * 位置（拔线那一刻目的屏的布局位置还是旧的，早了会算错）。 */
	}

	wlr_log(WLR_INFO, "hotplug: 拔掉 %s（%zu 列 / %zu 个窗口 / %zu 个浮动窗口），"
		"先挪到 %s 上住，归属记着",
		memory != NULL ? memory->key : dead != NULL ? dead->name : "?",
		on_screen_cols > 0 ? on_screen_cols : cols,
		on_screen_views > 0 ? on_screen_views : in_columns,
		floats, to != NULL ? to->name : "(没有屏了)");
}

void vidow_view_output_added(struct vidow_server *server,
		struct vidow_output_memory *memory, struct vidow_output *output) {
	/* 屏回来了：按 home 把属于它的工作区整块认领回来 —— 列、列宽、列序、滚动位置
	 * 全在工作区里跟着走，不需要再按名字搬一次（那样反倒会把列序转一圈）。 */
	vidow_workspace_output_added(server, memory, output->wlr_output);
	size_t cols = 0, in_columns = 0;
	count_output_columns(server, output->wlr_output, &cols, &in_columns);

	size_t floats = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->column != NULL || view->popup) {
			continue;
		}
		struct vidow_output_memory *home = view->ws != NULL ?
			view->ws->home : home_of(server, view->output);
		if (home != memory) {
			continue;
		}
		bool movable = view->float_mode != VIDOW_FLOAT_FOLLOW && view->placed;
		if (!movable) {
			continue;
		}
		/* 号码报的是「回到这块屏的浮动窗口数」，不管它是跟着工作区回来的，
		 * 还是这一步才挪回来的。 */
		floats++;
		if (view->output == output->wlr_output) {
			continue; /* 已经跟着工作区回来了 */
		}
		view->output = output->wlr_output;
	}

	/* 滚动位置：离开时停在哪儿，回来还停在哪儿（列宽是列自己的，本来就在）。 */
	if (memory->last_box.width > 0) {
		output->viewport_x = memory->viewport_x;
	}

	vidow_arrange(server);

	/* 焦点：离开时谁在这儿，回来还是谁（前提是它还在、还映射着、真落在这块屏上）。 */
	if (view_alive(server, memory->focused) && memory->focused->mapped &&
			memory->focused->output == output->wlr_output) {
		vidow_focus_view(memory->focused);
	}

	if (cols > 0 || floats > 0) {
		wlr_log(WLR_INFO, "hotplug: %s 回来了，还原 %zu 列 / %zu 个窗口 / %zu 个"
			"浮动窗口（列宽、滚动位置都留着）", memory->key, cols,
			in_columns, floats);
	}
}

/* --------------------------------------------------------------- 层级 */

/* 场景节点 → 窗口：窗口自己那棵树的 node.data 指向它的 tag（见 vidow_view_at
 * 往上走的那一段）。不是窗口节点（关闭动画的替身、别的层的节点）返回 NULL。 */
static struct vidow_view *view_from_node(struct wlr_scene_node *node) {
	struct vidow_view_tag *tag = node != NULL ? node->data : NULL;
	if (tag == NULL || tag->magic != VIDOW_VIEW_TAG_MAGIC) {
		return NULL;
	}
	return tag->view;
}

/* 场景树里同一层的先后就是层级（wlr_scene_node_at 也是从最上面那个往下找），
 * 这里维持一条不变量：
 *
 *     平铺 < 浮动 < 全屏
 *
 * 为什么要专门收口：`wlr_scene_tree_create()` 一律把新节点挂到最后，所以新开的
 * 平铺窗口会盖住已有的浮窗；聚焦那边又是无差别 raise_to_top，点一下平铺窗口浮窗
 * 就被压下去。两处都没在管「谁该在谁上面」。
 *
 * top 非空 = 「这一个刚被抬起来」（点了它 / 聚焦到它）：它落在自己那一组的最上面
 * —— 浮窗之间就是靠这一条换层级的（点哪个哪个上来）。
 *
 * 只在真有人站错队时才动手：raise 会让那块区域重画，站对了就一个节点都不碰。
 * 全屏单列一组压在最后：它是「占满整屏」，不该被浮窗盖住（老代码在 arrange 里
 * 抬全屏，这里接着保证它一直是最上面那个）。 */
void vidow_restack(struct vidow_server *server, struct vidow_view *top) {
	if (server == NULL || server->layer_toplevels == NULL) {
		return;
	}
	/* 弹出物（X11 的菜单 / 提示）挂在 layer_popups 那棵树上：它本来就在所有窗口
	 * 之上，这里只需要把自己抬起来（老行为）。 */
	if (top != NULL && top->popup) {
		if (top->scene_tree != NULL) {
			wlr_scene_node_raise_to_top(&top->scene_tree->node);
		}
		return;
	}

	/* 第一遍：按现在的层级（底 → 顶）把两组节点收下来，顺手看有没有人站错队。
	 * 收成数组是因为第二遍要动节点：wlr_scene_node_raise_to_top() 会把当前节点
	 * 挪到链表尾，边遍历边动会在同一个链表里绕圈。 */
	struct wl_array floats; /* struct wlr_scene_node * */
	struct wl_array fulls;
	wl_array_init(&floats);
	wl_array_init(&fulls);

	struct wlr_scene_node *node;
	struct wlr_scene_node *last_tile = NULL, *last_float = NULL;
	struct wlr_scene_node *last_full = NULL;
	bool float_seen = false, broken = false;
	wl_list_for_each(node, &server->layer_toplevels->children, link) {
		struct vidow_view *view = view_from_node(node);
		if (view == NULL) {
			continue;
		}
		bool full = view->fullscreen && view->mapped;
		if (!full && view->float_mode == VIDOW_FLOAT_NONE) {
			/* 平铺：它上面既不该有浮窗，也不该有全屏 */
			if (float_seen || last_full != NULL) {
				broken = true;
			}
			last_tile = node;
			continue;
		}
		struct wlr_scene_node **slot = wl_array_add(full ? &fulls : &floats,
			sizeof(*slot));
		if (slot != NULL) {
			*slot = node;
		}
		if (full) {
			last_full = node;
		} else {
			float_seen = true;
			last_float = node;
			if (last_full != NULL) {
				broken = true; /* 浮窗压在全屏上面 */
			}
		}
	}

	struct wlr_scene_node *top_node = top != NULL && top->scene_tree != NULL ?
		&top->scene_tree->node : NULL;
	bool top_full = top != NULL && top->fullscreen && top->mapped;
	bool top_float = top != NULL && !top_full &&
		top->float_mode != VIDOW_FLOAT_NONE;

	bool need = broken;
	if (top_node != NULL) {
		if (top_full) {
			need = need || last_full != top_node;
		} else if (top_float) {
			need = need || last_float != top_node;
		} else {
			/* 平铺的那个：只要它在平铺组里最上面就行 —— 浮窗 / 全屏在它上面
			 * 是应该的，不算站错队（否则点一下平铺窗口就会白抬一遍、白记一次
			 * 底衬过期）。 */
			need = need || last_tile != top_node;
		}
	}
	if (!need) {
		wl_array_release(&floats);
		wl_array_release(&fulls);
		return;
	}

	/* 第二遍：抬的顺序就是层级 —— 平铺的那个先（它在平铺组里最上面），浮窗按
	 * 现在的顺序跟上，全屏最后。top 在自己那一组里最后抬，所以它落在组的最上面。 */
	if (top_node != NULL && !top_full && !top_float) {
		wlr_scene_node_raise_to_top(top_node);
	}
	for (size_t i = 0; i < floats.size / sizeof(struct wlr_scene_node *); i++) {
		struct wlr_scene_node *n = ((struct wlr_scene_node **)floats.data)[i];
		if (n != top_node) {
			wlr_scene_node_raise_to_top(n);
		}
	}
	if (top_node != NULL && top_float) {
		wlr_scene_node_raise_to_top(top_node);
	}
	for (size_t i = 0; i < fulls.size / sizeof(struct wlr_scene_node *); i++) {
		struct wlr_scene_node *n = ((struct wlr_scene_node **)fulls.data)[i];
		if (n != top_node) {
			wlr_scene_node_raise_to_top(n);
		}
	}
	if (top_node != NULL && top_full) {
		wlr_scene_node_raise_to_top(top_node);
	}
	wl_array_release(&floats);
	wl_array_release(&fulls);

	/* 层级动了 = 「谁在谁后面」变了：底下那些窗口的私有底衬得重抓一遍。
	 * arrange 那条路本来就会记这一笔（除非它在「只有被拖的那个在动」的省电
	 * 模式里），这里管的是不经过 arrange 的那些调用（点一下浮窗换层级）。 */
	vidow_backdrop_damage(server, false);
}

/* --------------------------------------------------------------- focus */

/* 「聚焦这个窗口会让条带滚多少像素」—— niri 的 scroll_amount_to_activate。
 * 算的是和 arrange 的 Pass 2（把焦点列滚回可见区）**同一件事**：现在视口在哪、
 * 聚焦它以后视口该在哪，两者差多少就滚多少。只是这里不真滚，只把数报出去 ——
 * focus-follows-mouse 的 max-scroll-amount 拿它跟阈值比（见 input.c）。
 *
 * 浮窗的「跟随式」子窗（FLOAT_FOLLOW）和 Pass 2 一样跟着父窗口算：光标停在
 * 对话框上时，滚动的是它爹那条列。 */
double vidow_view_scroll_amount_to_activate(struct vidow_server *server,
		struct vidow_view *view) {
	if (view == NULL) {
		return 0.0;
	}
	if (view->float_mode == VIDOW_FLOAT_FOLLOW && view->parent != NULL) {
		view = view->parent;
	}
	struct vidow_column *col = view->column;
	if (col == NULL || col->output == NULL) {
		return 0.0;
	}
	struct vidow_output *output = vidow_output_lookup(server, col->output);
	if (output == NULL) {
		return 0.0;
	}
	struct wlr_box work = vidow_work_area(server, output->wlr_output);
	if (work.width < 1) {
		return 0.0;
	}
	double viewport = output->viewport_x;
	/* 和 Pass 2 同一个算法（focus_viewport_target）—— 「聚焦它会让视口滚到
	 * 哪」和「聚焦之后视口真的在哪」必须是同一件事，不然居中策略一开，
	 * focus-follows-mouse 的 max-scroll-amount 判的就全是错的。临时把焦点列
	 * 换成它，算完还回去：这个函数只算不滚，不许改 server 的状态。 */
	struct vidow_view *saved_focus = server->focused;
	server->focused = view;
	double target = focus_viewport_target(server, output, viewport, work.width);
	server->focused = saved_focus;

	/* 和 arrange 一样夹到 0..max_viewport：不夹的话「居中的列在条带最左 /
	 * 最右」会报出一个实际滚不到的距离。 */
	int max_viewport = 0;
	struct vidow_column *c;
	wl_list_for_each(c, output_shown_columns(server, output->wlr_output), link) {
		if (!column_has_shown_views(c)) {
			continue;
		}
		max_viewport = c->offset + column_strip_width(c, work.width);
	}
	max_viewport -= work.width;
	if (max_viewport < 0) {
		max_viewport = 0;
	}
	if (target < 0.0) {
		target = 0.0;
	}
	if (target > max_viewport) {
		target = max_viewport;
	}
	return target > viewport ? target - viewport : viewport - target;
}

void vidow_focus_view(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = vidow_view_surface(view);
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;

	/* 最小化的窗口不接键盘（它没画在画面上）。真走到这儿是调用方漏了跳过，
	 * 放回来那条路会先把 minimized 清掉再进来。 */
	if (view->minimized) {
		return;
	}
	/* 小窗（mini window，S2）：**迷你 / 图标态不拿键盘焦点**。和上面那条同形状、
	 * 同一个思想 —— 调用方漏了跳过；放大那条路（T3/T4）会先把形态改成 FREE
	 * 再进来。不变量：server->focused 永远不指向 MINI / ICON 的窗口。
	 *
	 * ⚠️ 和锁屏（S3）那套**方向相反**：锁屏是「谁都不许进」（连命中测试都短路），
	 * 这里是「只有焦点不许进，命中必须进」—— vidow_view_at() 必须能命中迷你态
	 * （单击要能放大），所以那边一个字都不动。 */
	if (view->mini == VIDOW_MINI_MINI || view->mini == VIDOW_MINI_ICON) {
		return;
	}

	if (prev != surface) {
		if (prev != NULL) {
			struct vidow_view *prev_view =
				vidow_view_from_surface(server, prev);
			/* prev 可能已经不是窗口了：正在销毁的那个窗口从 views 里摘掉之后
			 * 才轮到换焦点，或者它根本是 layer / 输入法这类表面 —— 那就没有
			 * 谁需要被取消激活（以前这里会把 NULL 传下去，直接段错误）。 */
			if (prev_view != NULL) {
				vidow_view_set_activated(prev_view, false);
				/* 窗口列表客户端（waybar 任务栏、portal）也要知道谁失去了焦点 */
				vidow_portal_view_activated(prev_view, false);
			}
		}

		if (server->keyboard_layer != NULL) {
			/* An exclusive layer surface owns the keyboard: activate the
			 * view, but do not steal focus. */
		} else {
			/* surface 可能是 NULL（wine / Proton 的游戏起来时：窗口先到、
			 * 表面还没到）。vidow_seat_focus_surface 会清焦点而不是 abort。 */
			vidow_seat_focus_surface(seat, surface);
		}
	}

	/* 焦点变了，把 text-input 的 enter/leave 转给客户端输入框 */
	vidow_im_focus(server, seat->keyboard_state.focused_surface);

	/* 抬起来：浮动永远在平铺之上、全屏压一切，同一组里谁被点谁在上（层级规则
	 * 见 vidow_restack）。 */
	vidow_restack(server, view);
	vidow_view_set_activated(view, true);
	vidow_portal_view_activated(view, true);

	/* The border colour depends on focus, so repaint both windows. */
	struct vidow_view *previous = server->focused;
	server->focused = view;
	if (previous != view) {
		/* center_focused_column = "on-overflow" 要知道「上一个焦点列」是谁
		 * （见 focus_viewport_target）。换焦点时才动它。 */
		server->last_focused_col = previous != NULL ? previous->column : NULL;
		/* 换了焦点列 → 允许下一次 arrange 让视口走过渡（整条带子刚性平移）。
		 * 同一列里换窗口不置它：那不该让条子滚。 */
		if (view->column != NULL && previous != NULL &&
				previous->column != view->column) {
			struct vidow_output *o;
			wl_list_for_each(o, &server->outputs, link) {
				if (o->wlr_output == view->output) {
					o->viewport_want_slide = true;
					break;
				}
			}
		}
		/* 「现在谁拿着键盘」是排查焦点问题（抢焦点 / 顶不上来）的第一现场，
		 * 而且只在真的换人时才打一行。 */
		wlr_log(WLR_INFO, "焦点: %s -> %s",
			previous != NULL && vidow_view_title(previous) != NULL ?
				vidow_view_title(previous) : "(无)",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)");
		/* 「最近用过」的台账（Alt+Tab 切换器要用）：见 mru.c 的 debounce。 */
		vidow_mru_note_focus(server, view);
	}
	/* 工作区记着「离开时焦点是谁」，换回来的时候要还原。 */
	vidow_workspace_note_focus(view->ws, view);
	/* 焦点屏跟着**归属**走（工作区住哪块屏），不是 view->output 那个住址：
	 * 两者在切工作区 / 热插拔 / 规则搬屏的中间态里可能短暂分叉，按住址走会
	 * 把 focused_output 带到隔壁屏上 —— 之后的空点击、新窗口、切工作区全按
	 * 错的屏算。工作区归属只有真的换屏才变，和 view_home_output 同一个判据。 */
	struct wlr_output *focus_out =
		view->ws != NULL && view->ws->output != NULL ?
		view->ws->output : view->output;
	if (focus_out != NULL) {
		server->focused_output = focus_out;
	}

	vidow_arrange(server);
	/* 换焦点会 restack 一次，把新工作区的窗口抬到旧工作区上面；过渡里
	 * 旧工作区得重新压回去，否则滑动过程里看到的是新窗口盖住旧工作区。 */
	vidow_workspace_slide_raise(server);

	if (previous != view) {
		vidow_ipc_emit_focus(server, view);
	}

	if (previous != NULL && previous != view && previous->fx.active) {
		vidow_view_apply_anim(previous);
	}
}

/* 「跳到这个窗口」= 迷你 / 图标态先摊成自由小窗，再给焦点（见 server.h）。
 *
 * 为什么在这儿：MRU 切换器（Alt+Tab 提交 / 点击卡片，src/mru.c）和 IPC 的
 * focus-window（src/ipc.c）是「选中即跳过去」的同一个语义，共用这一个入口；
 * 迷你 / 图标态是列在切换器里的（mru_listable 不管形态），选中它就该能用它。
 *
 * 顺序反过来（先 vidow_focus_view）会被上面那条守卫挡回来：什么都不发生，
 * 也就是这次改动之前的行为。摊成 FREE 走的是 float.c 的形态机，过渡动画、
 * 出帧策略、宽高比那套一件不少。 */
void vidow_focus_view_expand_mini(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	if (view->mini == VIDOW_MINI_MINI || view->mini == VIDOW_MINI_ICON) {
		vidow_mini_set_state(view, VIDOW_MINI_FREE);
	}
	vidow_focus_view(view);
}

/* 「把这个窗口端到人面前」：xdg-activation（点通知 / 客户端自己顶上来）、
 * 任务栏按钮（foreign-toplevel 的 activate）、X11 的 _NET_ACTIVE_WINDOW 都走这里。
 *
 * 光调 vidow_focus_view 是不够的：窗口要是住在藏起来的工作区上，焦点就落在一个
 * 看不见的窗口上 —— 看起来就是「点了通知没反应」「切不过去」。所以先把它的工作区
 * 端出来（在别的屏上就把焦点挪过去，工作区跟着它的屏走），再给键盘焦点。 */
void vidow_view_activate(struct vidow_view *view) {
	if (view == NULL || view->server == NULL || view->popup) {
		return;
	}
	struct vidow_server *server = view->server;
	/* 事件：有人要求激活这个窗口（X11 的 _NET_ACTIVE_WINDOW、Wayland 的
	 * xdg-activation、IPC 的 activate-window、点通知预览…都走这里）。
	 * 关注点不是「谁抢了焦点」而是「应用在喊『看我』」—— 通知 / 高亮脚本要的是
	 * 这个。 */
	vidow_ipc_emit_window(server, "window-activated", view);
	const char *title = vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(无题)";

	/* 最小化的窗口被激活（点通知 / 任务栏点它 / IPC restore-window）：先放回布局，
	 * 再走下面那套「端出工作区 + 给焦点」。规则说不理激活请求的话，也照样放回 ——
	 * 「别抢焦点」不等于「别把它端出来」。 */
	if (view->minimized) {
		vidow_view_set_minimized(view, false);
	}

	/* 规则说别理它的激活请求（后台 Electron 反复把自己顶上来那类）。 */
	if (!view->focus_on_activate) {
		wlr_log(WLR_INFO, "激活: %s 不理（窗口规则 focus_on_activate = false）",
			title);
		return;
	}

	/* 还没上屏：记下来，等 map 的时候兑现（点通知常常比窗口第一次 commit 还早）。 */
	if (!view->mapped) {
		view->pending_activate = true;
		wlr_log(WLR_INFO, "激活: %s 还没上屏，等它出来再聚焦", title);
		return;
	}

	/* 窗口在哪块屏上，人就跟到哪块屏（跨屏点通知也一样）。 */
	struct wlr_output *output = view->ws != NULL && view->ws->output != NULL ?
		view->ws->output : view->output;
	if (output != NULL && output != server->focused_output) {
		wlr_log(WLR_INFO, "激活: %s 在 %s 上，焦点跟过去", title, output->name);
		server->focused_output = output;
	}

	if (view->ws != NULL) {
		if (view->ws->special) {
			/* 特殊工作区：确保它正显示在这块屏上（显示在别的屏上就搬过来）。 */
			if (!view->ws->visible || view->ws->output != output) {
				wlr_log(WLR_INFO, "激活: %s 在特殊工作区 %s 上，把它开到 %s",
					title, view->ws->name,
					output != NULL ? output->name : "聚焦的屏");
				vidow_workspace_show_special(server, view->ws, output);
			}
		} else if (output == NULL ||
				vidow_workspace_shown_for(server, output) != view->ws) {
			/* 普通工作区：它没亮着（或者亮着的是别的屏）—— 先端出来。
			 * vidow_workspace_activate 认「现在聚焦的屏」，所以上面先把
			 * focused_output 挪到了窗口那块屏上。 */
			wlr_log(WLR_INFO, "激活: %s 在藏起来的 %s 上，先把它端出来",
				title, view->ws->name);
			vidow_workspace_activate(server, view->ws);
		}
	}

	vidow_focus_view(view);
}

static void focus_nearest_column(struct vidow_server *server,
		struct wlr_output *output) {
	struct vidow_column *col;
	struct wl_list *shown = output_shown_columns(server, output);
	wl_list_for_each(col, shown, link) {
		struct vidow_view *view = column_first_shown_view(col);
		if (view != NULL) {
			vidow_focus_view(view);
			return;
		}
	}

	server->focused = NULL;
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	vidow_im_focus(server, NULL);
	vidow_arrange(server);
}

/* 按屏幕位置挑「右边最近的那个窗口」。浮窗（和「列都空了、只剩浮窗」）用这条：
 * 它们不在列里，没有列序可看，只能量位置 —— x 比它大的里面取最小的（右边最近的），
 * 右边没有了取 x 最大的（左边最近的）。平铺窗口取它所在列**最上面**那个（列里没有
 * 「当前窗口」这个概念，别处（focus_nearest_column）也是这么认的）。 */
static struct vidow_view *focus_nearest_by_x(struct vidow_server *server,
		struct vidow_view *dying, struct wlr_output *output) {
	struct vidow_workspace *ws = dying->ws;
	struct vidow_view *v;
	struct vidow_view *right = NULL;
	struct vidow_view *left = NULL;
	struct vidow_view *next;

	if (ws == NULL || vidow_workspace_shown_for(server, output) != ws) {
		return NULL;
	}

	wl_list_for_each(v, &server->views, link) {
		if (v == dying || !v->mapped || v->popup || v->ws != ws ||
				v->output != output || v->geom.width <= 0 ||
				!view_shows(v)) {
			continue;
		}
		if (v->geom.x > dying->geom.x) {
			if (right == NULL || v->geom.x < right->geom.x) {
				right = v;
			}
		} else if (left == NULL || v->geom.x > left->geom.x) {
			left = v;
		}
	}

	next = right != NULL ? right : left;
	if (next != NULL && next->column != NULL) {
		next = column_first_shown_view(next->column);
	}
	return next;
}

/* 关掉一个窗口之后焦点落谁身上（niri 的规矩：**位置不动**，谁滑到空出来的位置上
 * 就是谁）：
 *   1) 列里还有窗口 —— 关掉那个的位置由它下面那个顶上；关的本来就是最下面那个，
 *      就往上找一个（列里只剩它一个的情况不算，见 2）；
 *   2) 整列没了 —— 右边最近的那一列顶上（右边那列滑到空出来的位置上）；
 *   3) 右边没有了 —— 才往左找最近的一列（关掉最右边那列时，niri 也是落到左边那个）；
 *   4) 不在列里（浮窗）/ 这块工作区上只剩浮窗 —— 按屏幕位置找右边最近的那个窗口
 *      （见 focus_nearest_by_x）：浮窗没有列序可看，但「往右」这条规矩一样成立。
 * 以前这里只做 1 的一半（只看下面），剩下全丢给 focus_nearest_column() —— 那个是
 * 「从左往右找第一个」，于是关掉右边一个窗口（浮窗更是），焦点蹦到最左边那列去了。
 *
 * 必须**在 view_leave_column() 之前**调：列一摘空就自己 free 了，邻居得趁它还挂在
 * 工作区链表上才找得到。返回 NULL = 这块工作区上一个窗口都不剩（或者它压根没摆出来，
 * 那就不该往这儿塞焦点）—— 交给调用方走 focus_nearest_column() 兜底。 */
static struct vidow_view *focus_after_close(struct vidow_server *server,
		struct vidow_view *view, struct wlr_output *output) {
	struct vidow_column *col = view->column;
	struct vidow_column *c;
	struct vidow_view *next;
	struct wl_list *node;

	if (col == NULL) {
		/* 浮窗：不在列里，按位置挑。 */
		return focus_nearest_by_x(server, view, output);
	}

	/* 同列上下：最小化的兄弟不接键盘（它没画在画面上），跳过它继续找。 */
	for (node = view->column_link.next; node != &col->views; node = node->next) {
		next = wl_container_of(node, next, column_link);
		if (view_shows(next)) {
			return next;
		}
	}
	for (node = view->column_link.prev; node != &col->views; node = node->prev) {
		next = wl_container_of(node, next, column_link);
		if (view_shows(next)) {
			return next;
		}
	}

	/* 列里就它一个：跨列找，但只在它那块工作区正摆在这块屏上时找 —— 藏着的
	 * 工作区里的窗口不该拿到键盘（那种情况走兜底，跟以前一样挑块看得见的）。 */
	if (col->ws == NULL || vidow_workspace_shown_for(server, output) != col->ws) {
		return NULL;
	}

	for (node = col->link.next; node != &col->ws->columns; node = node->next) {
		c = wl_container_of(node, c, link);
		next = column_first_shown_view(c);
		if (next != NULL) {
			return next;
		}
	}
	for (node = col->link.prev; node != &col->ws->columns; node = node->prev) {
		c = wl_container_of(node, c, link);
		next = column_first_shown_view(c);
		if (next != NULL) {
			return next;
		}
	}

	/* 这块工作区上没有别的列了：可能还剩浮窗 —— 它们照样该接住键盘（niri 的
	 * 「平铺关完落到浮窗上」）。挑不到就返回 NULL，调用方走兜底。 */
	return focus_nearest_by_x(server, view, output);
}

/* -------------------------------------------------------- 小窗（S2）焦点交出 */

/* 一个窗口要进 MINI / ICON 了：它**不接键盘**，所以焦点得先交出去。规矩照抄
 * vidow_view_set_minimized() 里那条（已测过的路）—— 「位置不动，谁滑到空出来的
 * 位置上就是谁」，不另发明一套。
 *
 * 两件事，必须**在摘列 / 摘工作区之前**做（vidow_mini_set_state 的调用点在最前面）：
 *   1) 键盘焦点换人：focus_after_close() 要靠窗口还挂在列里才找得到邻居；
 *   2) 指针焦点清掉：光标本来压在这个窗口上，形态一变它就不该再收 enter / motion
 *      （不然进迷你态的第一下移动还会转给客户端，见 input.c 的小窗分流）。
 *
 * 为什么放在 view.c：focus_after_close / focus_nearest_column / focus_nearest_by_x
 * 都是这儿的静态函数，搬出去要暴露三个。 */
void vidow_view_yield_focus_for_mini(struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return;
	}
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;

	/* 指针焦点：光标底下就是它（或者它的子表面）才清 —— 清别人的会打断正在
	 * 操作的窗口。 */
	struct wlr_surface *pfocus = server->seat->pointer_state.focused_surface;
	if (pfocus != NULL && vidow_view_from_surface(server,
			wlr_surface_get_root_surface(pfocus)) == view) {
		wlr_seat_pointer_notify_clear_focus(server->seat);
	}

	/* 形态上它也不再是「活动窗口」了：边框 / 任务栏（foreign toplevel）跟着走。 */
	vidow_view_set_activated(view, false);
	vidow_portal_view_activated(view, false);

	if (server->focused != view) {
		return;
	}
	struct vidow_view *next = focus_after_close(server, view, output);
	server->focused = NULL;
	if (next != NULL) {
		vidow_focus_view(next);
	} else {
		focus_nearest_column(server, output);
	}
	wlr_log(WLR_INFO, "mini: 焦点交出 %s（迷你 / 图标不接键盘）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
}

/* ================================== 小窗（mini window，S5）：图标悬停预览
 *
 * 悬停图标 mini_icon_hover_ms 之后，在**朝屏内**那一侧展开一块迷你尺寸 ×0.8 的
 * **实时预览**（图标本身贴边、有一部分在屏外，朝屏外展开会被裁掉一半）。
 *
 * 三条规矩：
 *  1) **只读**：不抢 pointer focus、不进 MRU、不拿键盘焦点、不参与
 *     vidow_view_at 命中 —— 它是**信息**不是窗口（指针扫过屏幕边缘就把键盘焦点
 *     从正在打字的窗口上拽走是不能接受的）。命中和焦点这两条是**构造性**的：
 *     预览挂在 layer_toplevels 里、节点没有 view tag，vidow_view_at 走的是
 *     「节点 → view」那条映射，认不出它。
 *  2) **但吞点击**：指针底下有一块「像窗口的东西」，点下去却激活**背后的**窗口
 *     是明显的错。吞掉最省事，也不引入「预览能不能操作」这个新问题。
 *  3) **渲染用同一张表面的第二份**（wlr_scene_subsurface_tree_create，先例
 *     wincapture.c:676）—— 不另造一套抓帧 / 上屏管线，客户端出一帧预览就跟着动。
 *
 * 时间轴（用 vidow_anim_start 当定时器，不新开 timer —— 它自己会 kick watchdog
 * 保证「就算后端不给帧事件帧也照出」）：延迟 hover_ms → 出；不在图标也不在预览
 * → 离开 leave_ms → 收；**指针进到预览矩形里就把计时清掉**（这就是宽限期：
 * 从图标移到预览要跨那 8px 空隙，不给宽限会闪一下再回来）。
 */

/* ---- 小窗（S5）：图标 / 预览的出帧策略（计划 §9②） ----
 *
 * 按该窗口**有效**的 force_render 分叉：
 *   - 开 → 活节点（图标就是窗口自己那块节点，预览是表面第二份镜面）+ 客户端
 *     照常出帧（render_pump 在它被停到屏外时保活）→ 图标和预览都是**动的**；
 *   - 关 → 快照节点（`view->last_buffer` 的最后一帧）+ **真实节点停到停车位**
 *     → 客户端停帧，图标和预览都是**静止的最后一帧**。
 *
 * 两条路必须**一致**：预览不许出现「图标静止、预览还在动」—— 所以它走的是同一个
 * 判据（vidow_mini_uses_snapshot），不是各判一份。
 *
 * 为什么不算新生命周期路径：快照节点的 buffer 由 `wlr_scene_buffer_set_buffer()`
 * 自己管引用（它 ref 新的、unref 旧的），所以只要在 `view->last_buffer` 换新
 * 的那一处同步一次就够 —— 不用另建监听、不用管窗口什么时候没。
 *
 * 退化情形：`last_buffer == NULL`（客户端一帧都没交过）→ **退回活节点**，
 * 在进图标态那次打一行 INFO（见 float.c 的 vidow_mini_set_state）。
 */
bool vidow_mini_uses_snapshot(const struct vidow_view *view) {
	return view != NULL && view->mapped && view->mini == VIDOW_MINI_ICON &&
		!view->force_render && view->last_buffer != NULL;
}

/* 快照节点收掉。它是 `layer_toplevels` 的孩子，**不跟着 view->scene_tree 走** ——
 * 窗口 unmap / 销毁时留着它，屏上就是一块「窗口都没了、还冻着最后一帧」的鬼影
 * （而且攥着那帧缓冲）。下次再进图标态 mini_snapshot_place() 会重建。 */
static void mini_snapshot_free(struct vidow_view *view) {
	if (view->mini_snapshot == NULL) {
		return;
	}
	wlr_scene_node_destroy(&view->mini_snapshot->node);
	view->mini_snapshot = NULL;
}

/* 图标态的快照节点：位置尺寸跟着图标那块盒子（contain 居中，和 view_mini_scale
 * 同一条算法），只在「快照那条路」上开。 */
static void mini_snapshot_place(struct vidow_view *view) {
	if (view->scene_tree == NULL) {
		return;
	}
	if (!vidow_mini_uses_snapshot(view)) {
		if (view->mini_snapshot != NULL) {
			wlr_scene_node_set_enabled(&view->mini_snapshot->node, false);
		}
		return;
	}
	if (view->mini_snapshot == NULL) {
		if (view->server->layer_toplevels == NULL) {
			return;
		}
		view->mini_snapshot = wlr_scene_buffer_create(
			view->server->layer_toplevels, view->last_buffer);
		if (view->mini_snapshot == NULL) {
			return;
		}
		wlr_log(WLR_INFO, "mini: %s 图标态走快照（force_render 关）→ 真实节点"
			"停到停车位、客户端停帧，图标和预览都是最后一帧",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
	} else if (view->mini_snapshot->buffer != view->last_buffer) {
		wlr_scene_buffer_set_buffer(view->mini_snapshot, view->last_buffer);
	}
	int x = view->geom.x + (int)lround(view->anim_off_x);
	int y = view->geom.y + (int)lround(view->anim_off_y);
	int nw = 0, nh = 0;
	view_mini_native(view, &nw, &nh);
	int dw = view->geom.width, dh = view->geom.height;
	if (nw > 0 && nh > 0) {
		double s = (double)view->geom.width / (double)nw;
		double sy = (double)view->geom.height / (double)nh;
		if (sy < s) {
			s = sy;
		}
		dw = (int)lround((double)nw * s);
		dh = (int)lround((double)nh * s);
		if (dw < 1) {
			dw = 1;
		}
		if (dh < 1) {
			dh = 1;
		}
	}
	int px = x + (view->geom.width - dw) / 2;
	int py = y + (view->geom.height - dh) / 2;
	/* 越界裁剪（S3 修）：快照节点挂在**全局**的 layer_toplevels 上（不在窗口自己
	 * 的场景树里），而场景图是全局坐标 —— 图标按 mini_icon_overflow 刻意探出屏边
	 * 那一块会被隔壁那块屏照画（实测那条 16×64 带里 832 个非背景像素、其中 172 个
	 * 是客户端的纯红）。裁法和 fx 那边同一招：源框（缓冲像素）取可见那一块、目标框
	 * （场景尺寸）也取可见那一块 —— 缩放比一个像素不变，框外那部分不画。
	 *
	 * 判据用**自己那块屏的框**（view_clip_box 那条路对停到停车位的窗口是「不裁」：
	 * 它那边裁的是真实节点，而快照模式下真实节点本来就被停到屏外了）。 */
	struct wlr_box clip = {0};
	if (view->output != NULL) {
		wlr_output_layout_get_box(view->server->output_layout, view->output,
			&clip);
	}
	if (clip.width >= 1 && clip.height >= 1) {
		struct wlr_box full = {.x = px, .y = py, .width = dw, .height = dh};
		struct wlr_box vis;
		int bw = view->last_buffer != NULL ? view->last_buffer->width : 0;
		int bh = view->last_buffer != NULL ? view->last_buffer->height : 0;
		if (!wlr_box_intersection(&vis, &full, &clip)) {
			/* 整个身子都在自己那块屏外：一个像素都不该画（画了就是画到隔壁
			 * 屏上）。甩到所有输出框外面去 —— 和 fx 那边同一招。 */
			wlr_scene_buffer_set_source_box(view->mini_snapshot, NULL);
			px -= 1000000;
			py -= 1000000;
			dw = 1;
			dh = 1;
		} else if ((vis.width != dw || vis.height != dh) && bw > 0 && bh > 0) {
			double kx = (double)bw / (double)dw;
			double ky = (double)bh / (double)dh;
			struct wlr_fbox src = {
				.x = (double)(vis.x - px) * kx,
				.y = (double)(vis.y - py) * ky,
				.width = (double)vis.width * kx,
				.height = (double)vis.height * ky,
			};
			wlr_scene_buffer_set_source_box(view->mini_snapshot, &src);
			px = vis.x;
			py = vis.y;
			dw = vis.width;
			dh = vis.height;
		} else {
			wlr_scene_buffer_set_source_box(view->mini_snapshot, NULL);
		}
	} else {
		wlr_scene_buffer_set_source_box(view->mini_snapshot, NULL);
	}
	wlr_scene_buffer_set_dest_size(view->mini_snapshot, dw, dh);
	wlr_scene_node_set_position(&view->mini_snapshot->node, px, py);
	wlr_scene_node_set_enabled(&view->mini_snapshot->node, true);
}

void vidow_mini_frames_sync(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	struct vidow_server *server = view->server;
	if (view->mini_snapshot != NULL) {
		if (view->last_buffer == NULL) {
			/* 窗口没了 / 还没交过帧：把快照收掉（留着会攥着一张过期缓冲）。 */
			wlr_scene_node_set_enabled(&view->mini_snapshot->node, false);
		} else if (view->mini_snapshot->buffer != view->last_buffer) {
			wlr_scene_buffer_set_buffer(view->mini_snapshot, view->last_buffer);
		}
	}
	/* 预览也吃同一份快照：不能出现「图标静止、预览还在动」。 */
	if (server->mini_preview_snap != NULL &&
			server->mini_preview_view == view &&
			view->last_buffer != NULL &&
			server->mini_preview_snap->buffer != view->last_buffer) {
		wlr_scene_buffer_set_buffer(server->mini_preview_snap, view->last_buffer);
	}
	/* 进快照态那一刻 last_buffer 还是空的话，这里补摆一次（客户端下一帧就到）。 */
	vidow_mini_frames_recheck(view);
}

/* 出帧策略（快照 / 活）跟**有效** force_render 走：规则 / IPC 把它改了之后，
 * 得有人把这一面接上 —— 快照那条路要把真实节点停到停车位（客户端停帧），活
 * 那条路要把它摆回图标那块。不接的话图标会「两边都不画」（快照关了、真实节点
 * 还在停车位上，看起来就是图标凭空消失），或者反过来一直冻着。
 *
 * 谁调用：换 last_buffer 的那条路（客户端交帧）、指针一动（用户刚改完多半就在
 * 鼠标上）、每帧的补帧循环（vidow_view_render_pump）。三条都便宜，且都幂等。 */
void vidow_mini_frames_recheck(struct vidow_view *view) {
	if (view == NULL || !view->mapped) {
		return;
	}
	bool was = view->mini_snapshot != NULL && view->mini_snapshot->node.enabled;
	mini_snapshot_place(view);
	bool now = view->mini_snapshot != NULL && view->mini_snapshot->node.enabled;
	if (was != now) {
		vidow_view_apply_anim(view);
	}
}

/* 预览的内容节点 / 缓冲：活镜面那条路是树里的 scene_surface，快照那条路是
 * 一个 scene_buffer 节点 —— 位置、尺寸、透明度三个出口都走这两个取。 */
static struct wlr_scene_node *preview_content_node(struct vidow_server *server) {
	if (server->mini_preview_snap != NULL) {
		return &server->mini_preview_snap->node;
	}
	if (server->mini_preview != NULL) {
		return &server->mini_preview->node;
	}
	return NULL;
}

static struct wlr_scene_buffer *preview_content_buffer(
		struct vidow_server *server) {
	if (server->mini_preview_snap != NULL) {
		return server->mini_preview_snap;
	}
	if (server->mini_preview_surface != NULL) {
		return server->mini_preview_surface->buffer;
	}
	return NULL;
}

/* 镜面树里那层内容节点（wlr_scene_subsurface_tree_create 建的第一层就是它；
 * 子表面另外挂。和 capturemask.c 的写法一致）。 */
static struct wlr_scene_surface *preview_find_surface(
		struct wlr_scene_tree *tree) {
	if (tree == NULL) {
		return NULL;
	}
	struct wlr_scene_node *node;
	wl_list_for_each(node, &tree->children, link) {
		if (node->type != WLR_SCENE_NODE_BUFFER) {
			continue;
		}
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(
			wlr_scene_buffer_from_node(node));
		if (ss != NULL) {
			return ss;
		}
	}
	return NULL;
}

/* 预览那块矩形：迷你态尺寸 × 0.8，摆在图标的**朝屏内**那一侧（间距
 * mini_edge_margin），再夹进工作区。 */
static bool preview_box_for(struct vidow_server *server,
		struct vidow_view *view, struct wlr_box *out) {
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width < 1 || work.height < 1) {
		return false;
	}
	double wpct = 28.0, hpct = 28.0;
	const char *tw = vidow_config_get(&server->config, "mini_w");
	const char *th = vidow_config_get(&server->config, "mini_h");
	if (tw != NULL) {
		vidow_config_parse_percent(tw, &wpct);
	}
	if (th != NULL) {
		vidow_config_parse_percent(th, &hpct);
	}
	int pw = (int)lround(wpct / 100.0 * 0.8 * (double)work.width);
	int ph = (int)lround(hpct / 100.0 * 0.8 * (double)work.height);
	if (pw < 1) {
		pw = 1;
	}
	if (ph < 1) {
		ph = 1;
	}
	/* 图标「画在哪」= geom + anim_off（和 vidow_place_view 同源）。 */
	int ix = view->geom.x + (int)lround(view->anim_off_x);
	int iy = view->geom.y + (int)lround(view->anim_off_y);
	int iw = view->geom.width, ih = view->geom.height;
	int margin = server->config.mini_edge_margin;
	if (margin < 0) {
		margin = 0;
	}
	int x = 0, y = 0;
	switch (view->mini_edge) {
	case VIDOW_MINI_EDGE_LEFT:
		x = ix + iw + margin;
		y = iy + (ih - ph) / 2;
		break;
	case VIDOW_MINI_EDGE_RIGHT:
		x = ix - margin - pw;
		y = iy + (ih - ph) / 2;
		break;
	case VIDOW_MINI_EDGE_TOP:
		x = ix + (iw - pw) / 2;
		y = iy + ih + margin;
		break;
	case VIDOW_MINI_EDGE_BOTTOM:
		x = ix + (iw - pw) / 2;
		y = iy - margin - ph;
		break;
	case VIDOW_MINI_EDGE_NONE:
		x = ix + iw + margin;
		y = iy;
		break;
	}
	if (x < work.x) {
		x = work.x;
	}
	if (y < work.y) {
		y = work.y;
	}
	if (x + pw > work.x + work.width) {
		x = work.x + work.width - pw;
	}
	if (y + ph > work.y + work.height) {
		y = work.y + work.height - ph;
	}
	*out = (struct wlr_box){ .x = x, .y = y, .width = pw, .height = ph };
	return true;
}

bool vidow_mini_preview_hit(struct vidow_server *server, double lx, double ly) {
	if (server == NULL || server->mini_preview_view == NULL ||
			!server->mini_preview_shown) {
		return false;
	}
	const struct wlr_box *b = &server->mini_preview_box;
	return lx >= b->x && lx < b->x + b->width &&
		ly >= b->y && ly < b->y + b->height;
}

static void preview_apply_alpha(struct vidow_server *server, double a) {
	if (a < 0.0) {
		a = 0.0;
	} else if (a > 1.0) {
		a = 1.0;
	}
	server->mini_preview_alpha = a;
	struct wlr_scene_buffer *buf = preview_content_buffer(server);
	if (buf != NULL) {
		wlr_scene_buffer_set_opacity(buf, (float)a);
	}
}

static void preview_fade_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_server *server = data;
	preview_apply_alpha(server, server->mini_preview_alpha_from +
		(server->mini_preview_alpha_to - server->mini_preview_alpha_from) * t);
}

static void preview_fade_done(void *data, bool cancelled) {
	struct vidow_server *server = data;
	server->mini_preview_fade = NULL;
	if (cancelled) {
		return;
	}
	if (server->mini_preview_alpha_to <= 0.0) {
		/* 淡出完：收节点、忘掉 view（节点留着复用，下次换个表面 / 换个模式就重建）。 */
		struct wlr_scene_node *node = preview_content_node(server);
		if (node != NULL) {
			wlr_scene_node_set_enabled(node, false);
		}
		/* 快照那份是客户端缓冲的一张裸引用（wlr_scene_buffer 一直攥着）：收起来
		 * 时松开，别替一个已经没了的窗口把缓冲扣着。活镜面那条路是
		 * scene_surface，缓冲跟着它自己的生命周期走。 */
		if (server->mini_preview_snap != NULL) {
			wlr_scene_buffer_set_buffer(server->mini_preview_snap, NULL);
		}
		server->mini_preview_view = NULL;
		server->mini_preview_shown = false;
		server->mini_preview_box = (struct wlr_box){0};
	}
}

static void preview_fade_to(struct vidow_server *server, double to, int ms) {
	vidow_anim_cancel(server->mini_preview_fade);
	server->mini_preview_fade = NULL;
	server->mini_preview_alpha_from = server->mini_preview_alpha;
	server->mini_preview_alpha_to = to;
	if (ms <= 0) {
		preview_apply_alpha(server, to);
		preview_fade_done(server, false);
		return;
	}
	struct vidow_ease linear = { .kind = VIDOW_EASE_LINEAR };
	server->mini_preview_fade = vidow_anim_start(server, "mini-preview-fade",
		ms, linear, preview_fade_update, preview_fade_done, server);
	if (server->mini_preview_fade == NULL) {
		preview_apply_alpha(server, to);
		preview_fade_done(server, false);
	}
}

void vidow_mini_preview_hide(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	vidow_anim_cancel(server->mini_hover_timer);
	server->mini_hover_timer = NULL;
	server->mini_hover_view = NULL;
	server->mini_preview_pressed = false;
	if (!server->mini_preview_shown ||
			(server->mini_preview == NULL && server->mini_preview_snap == NULL)) {
		server->mini_preview_view = NULL;
		server->mini_preview_shown = false;
		server->mini_preview_box = (struct wlr_box){0};
		return;
	}
	wlr_log(WLR_DEBUG, "mini preview: 收起来（%dx%d @ %d,%d）",
		server->mini_preview_box.width, server->mini_preview_box.height,
		server->mini_preview_box.x, server->mini_preview_box.y);
	preview_fade_to(server, 0.0, server->config.mini_hover_fade_out_ms);
}

static void preview_show(struct vidow_server *server, struct vidow_view *view) {
	if (server == NULL || view == NULL || server->locked ||
			view->mini != VIDOW_MINI_ICON) {
		return;
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL) {
		return;
	}
	struct wlr_box box = {0};
	if (!preview_box_for(server, view, &box)) {
		return;
	}
	/* 出帧策略和图标**同一份判据**：走快照那条路时预览也是静止的最后一帧。 */
	bool snapshot = vidow_mini_uses_snapshot(view);
	if (snapshot) {
		if (server->mini_preview != NULL) {
			wlr_scene_node_destroy(&server->mini_preview->node);
			server->mini_preview = NULL;
			server->mini_preview_surface = NULL;
		}
		if (server->mini_preview_snap == NULL) {
			server->mini_preview_snap = wlr_scene_buffer_create(
				server->layer_top, view->last_buffer);
			if (server->mini_preview_snap == NULL) {
				return;
			}
		} else if (server->mini_preview_snap->buffer != view->last_buffer) {
			wlr_scene_buffer_set_buffer(server->mini_preview_snap,
				view->last_buffer);
		}
	} else {
		if (server->mini_preview_snap != NULL) {
			wlr_scene_node_destroy(&server->mini_preview_snap->node);
			server->mini_preview_snap = NULL;
		}
		/* 换了窗口（或者第一次）：重建镜面 —— 同一张表面挂第二棵树是允许的
		 * （见 wincapture.c 那段注释）。 */
		if (server->mini_preview == NULL || server->mini_preview_view != view) {
			if (server->mini_preview != NULL) {
				wlr_scene_node_destroy(&server->mini_preview->node);
				server->mini_preview = NULL;
				server->mini_preview_surface = NULL;
			}
			/* 挂 layer_top（不是 layer_toplevels）：它在窗口之上、会话锁和 overview
			 * 都会把它整棵关掉（sessionlock.c / overview.c 那两个数组），而且
			 * **不在模糊底衬抓取 / 裁剪检查的遍历范围里**（fx.c 那两处走的是
			 * layer_toplevels）—— 挂错地方的话，悬停预览那块镜面会被当成一个窗口
			 * 抓进底衬里，留下一块鬼影。 */
			server->mini_preview = wlr_scene_subsurface_tree_create(
				server->layer_top, surface);
			if (server->mini_preview == NULL) {
				return;
			}
			server->mini_preview_surface = preview_find_surface(server->mini_preview);
		}
		if (server->mini_preview_surface == NULL) {
			return;
		}
	}
	struct wlr_scene_buffer *content = preview_content_buffer(server);
	struct wlr_scene_node *node = preview_content_node(server);
	if (content == NULL || node == NULL) {
		return;
	}
	/* 尺寸：contain 塞进预览盒（和 S3 的 view_mini_scale 同一条算法 —— 不拉伸、
	 * 按内容自己的比例），居中摆。 */
	int nw = 0, nh = 0;
	view_mini_native(view, &nw, &nh);
	int dw = box.width, dh = box.height;
	if (nw > 0 && nh > 0) {
		double s = (double)box.width / (double)nw;
		double sy = (double)box.height / (double)nh;
		if (sy < s) {
			s = sy;
		}
		dw = (int)lround((double)nw * s);
		dh = (int)lround((double)nh * s);
		if (dw < 1) {
			dw = 1;
		}
		if (dh < 1) {
			dh = 1;
		}
	}
	wlr_scene_buffer_set_dest_size(content, dw, dh);
	wlr_scene_node_set_position(node,
		box.x + (box.width - dw) / 2, box.y + (box.height - dh) / 2);
	wlr_scene_node_set_enabled(node, true);
	/* 压在别的窗口之上：它可能比这些窗口后建出来（新开的窗口挂在后面）。 */
	wlr_scene_node_raise_to_top(node);
	server->mini_preview_view = view;
	server->mini_preview_box = box;
	server->mini_preview_shown = true;
	preview_apply_alpha(server, 0.0);
	preview_fade_to(server, 1.0, server->config.mini_hover_fade_in_ms);
	wlr_log(WLR_INFO, "mini preview: %s 的预览在 %d,%d %dx%d（图标 %d,%d %dx%d，"
		"贴 %s 边 → 朝屏内展开，%s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		box.x, box.y, box.width, box.height,
		view->geom.x, view->geom.y, view->geom.width, view->geom.height,
		view->mini_edge == VIDOW_MINI_EDGE_LEFT ? "左" :
			view->mini_edge == VIDOW_MINI_EDGE_RIGHT ? "右" :
			view->mini_edge == VIDOW_MINI_EDGE_TOP ? "上" :
			view->mini_edge == VIDOW_MINI_EDGE_BOTTOM ? "下" : "（无）",
		snapshot ? "快照（force_render 关，静止的最后一帧）" :
			"活镜面（force_render 开，动的）");
}

/* 指针是不是在「图标 → 预览」那条**空隙走廊**里（正朝预览走）：这时候用
 * mini_icon_hover_grace_ms（200ms）而不是离开的 300ms —— 跨那 8px 空隙本来就要
 * 一两帧「既不在图标也不在预览」，按 300ms 收会让用户看到闪一下再回来。
 * 移向别处（不在走廊里）才按 mini_icon_hover_leave_ms 收。 */
static bool preview_in_corridor(struct vidow_server *server, double lx,
		double ly) {
	if (!server->mini_preview_shown || server->mini_preview_view == NULL) {
		return false;
	}
	struct vidow_view *view = server->mini_preview_view;
	struct wlr_box prev = server->mini_preview_box;
	struct wlr_box icon = {
		.x = view->geom.x + (int)lround(view->anim_off_x),
		.y = view->geom.y + (int)lround(view->anim_off_y),
		.width = view->geom.width,
		.height = view->geom.height,
	};
	int margin = server->config.mini_edge_margin;
	if (margin < 0) {
		margin = 0;
	}
	int y1 = icon.y < prev.y ? icon.y : prev.y;
	int y2 = icon.y + icon.height > prev.y + prev.height ?
		icon.y + icon.height : prev.y + prev.height;
	struct wlr_box gap = {0};
	if (prev.x + prev.width <= icon.x) {          /* 预览在图标左边 */
		gap = (struct wlr_box){ .x = prev.x + prev.width, .y = y1,
			.width = icon.x - (prev.x + prev.width), .height = y2 - y1 };
	} else if (icon.x + icon.width <= prev.x) {   /* 预览在图标右边 */
		gap = (struct wlr_box){ .x = icon.x + icon.width, .y = y1,
			.width = prev.x - (icon.x + icon.width), .height = y2 - y1 };
	} else if (prev.y + prev.height <= icon.y) {  /* 预览在图标上方 */
		gap = (struct wlr_box){ .x = 0, .y = prev.y + prev.height,
			.width = 0, .height = icon.y - (prev.y + prev.height) };
		int x1 = icon.x < prev.x ? icon.x : prev.x;
		int x2 = icon.x + icon.width > prev.x + prev.width ?
			icon.x + icon.width : prev.x + prev.width;
		gap.x = x1;
		gap.width = x2 - x1;
	} else if (icon.y + icon.height <= prev.y) {  /* 预览在图标下方 */
		int x1 = icon.x < prev.x ? icon.x : prev.x;
		int x2 = icon.x + icon.width > prev.x + prev.width ?
			icon.x + icon.width : prev.x + prev.width;
		gap = (struct wlr_box){ .x = x1, .y = icon.y + icon.height,
			.width = x2 - x1, .height = prev.y - (icon.y + icon.height) };
	} else {
		return false; /* 两块贴在一起（没有空隙）：不存在「跨空隙」 */
	}
	/* 走廊在法线方向放宽 margin（指针抖一点也还算在走廊里），纵向也要覆盖
	 * 两块盒子合起来的范围（指针从图标往预览斜着走）。 */
	gap.x -= margin;
	gap.y -= margin;
	gap.width += margin * 2;
	gap.height += margin * 2;
	if (gap.width < 1) {
		gap.width = 1;
	}
	if (gap.height < 1) {
		gap.height = 1;
	}
	return lx >= gap.x && lx < gap.x + gap.width &&
		ly >= gap.y && ly < gap.y + gap.height;
}

static void hover_timer_done(void *data, bool cancelled) {
	struct vidow_server *server = data;
	bool leaving = server->mini_hover_leaving;
	struct vidow_view *view = server->mini_hover_view;
	server->mini_hover_timer = NULL;
	server->mini_hover_view = NULL;
	if (cancelled) {
		return;
	}
	if (leaving) {
		vidow_mini_preview_hide(server);
	} else {
		preview_show(server, view);
	}
}

static void hover_timer_start(struct vidow_server *server, int ms,
		bool leaving, struct vidow_view *view) {
	vidow_anim_cancel(server->mini_hover_timer);
	server->mini_hover_timer = NULL;
	if (ms <= 0) {
		ms = 1;
	}
	server->mini_hover_leaving = leaving;
	server->mini_hover_view = view;
	struct vidow_ease linear = { .kind = VIDOW_EASE_LINEAR };
	server->mini_hover_timer = vidow_anim_start(server, "mini-hover", ms,
		linear, NULL, hover_timer_done, server);
}

void vidow_mini_preview_motion(struct vidow_server *server,
		struct vidow_view *hit) {
	if (server == NULL || server->cursor == NULL) {
		return;
	}
	if (!server->config.mini_enabled || !server->config.mini_icon_preview) {
		vidow_mini_preview_hide(server);
		return;
	}
	struct vidow_view *icon = (hit != NULL && hit->mini == VIDOW_MINI_ICON) ?
		hit : NULL;
	/* 用户刚用 IPC / 规则把 force_render 改了的话，指针一动就把出帧策略接上
	 * （改完多半鼠标就动起来了 —— 快照态下客户端已经停帧，没有提交来触发
	 * 重新摆位，指针一动是最靠得住的那个触发点）。 */
	struct vidow_view *iv;
	wl_list_for_each(iv, &server->views, link) {
		if (iv->mini == VIDOW_MINI_ICON || iv->mini_snapshot != NULL) {
			vidow_mini_frames_recheck(iv);
		}
	}
	if (icon == NULL) {
		/* 指针在预览上（只读，但算「还在看它」）→ 清掉离开计时 = 宽限期。 */
		if (vidow_mini_preview_hit(server, server->cursor->x,
				server->cursor->y)) {
			vidow_anim_cancel(server->mini_hover_timer);
			server->mini_hover_timer = NULL;
			server->mini_hover_view = NULL;
			return;
		}
		/* 既不在图标也不在预览：
		 *   - 在「图标 → 预览」那条空隙走廊里（正朝预览走）→ 用宽限期
		 *     mini_icon_hover_grace_ms（200ms），而且**每动一下重起**：人还
		 *     在往预览走，不该被收走；
		 *   - 移向别处 → 用 mini_icon_hover_leave_ms（300ms），**已经起着就
		 *     不重起**（不然每动一下都推倒重来，永远收不掉）。 */
		bool corridor = preview_in_corridor(server, server->cursor->x,
			server->cursor->y);
		if (corridor) {
			hover_timer_start(server,
				server->config.mini_icon_hover_grace_ms, true, NULL);
			return;
		}
		if ((server->mini_preview_shown ||
				server->mini_hover_timer != NULL) &&
				(server->mini_hover_timer == NULL ||
					!server->mini_hover_leaving)) {
			hover_timer_start(server,
				server->config.mini_icon_hover_leave_ms, true, NULL);
		}
		return;
	}
	/* 在图标上：离开计时作废。 */
	if (server->mini_hover_timer != NULL && server->mini_hover_leaving) {
		vidow_anim_cancel(server->mini_hover_timer);
		server->mini_hover_timer = NULL;
	}
	if (server->mini_preview_shown && server->mini_preview_view == icon) {
		/* 已经露着 —— 但出帧策略可能刚翻面（force_render 被 IPC / 规则改了）：
		 * 那就重建一次，免得「图标活了预览还冻着」（或反过来）—— 两条路必须
		 * 一致。 */
		if ((server->mini_preview_snap != NULL) !=
				vidow_mini_uses_snapshot(icon)) {
			preview_show(server, icon);
		}
		return;
	}
	if (server->mini_hover_timer != NULL &&
			server->mini_hover_view == icon) {
		return; /* 延迟计时正跑着 */
	}
	hover_timer_start(server, server->config.mini_icon_hover_ms, false, icon);
}

/* ------------------------------------------------------------------ 最小化 */

/* 最小化 / 放回。窗口**留在原来的列里**（列宽、上下顺序、滚动位置、归属工作区和
 * 归属屏全都不用重建，放回来是瞬时的），只是不占位、不显示、不参与焦点：
 *
 *   - 布局：arrange 跳过它；整列都最小化了，那条列从条带里消失（剩下的列合拢）；
 *   - 可见性：vidow_workspace_view_visible() 说它不可见 → 场景节点关掉，跟「工作区
 *     被藏起来」是同一套；
 *   - 补帧：**规则要求保活（force_render）或者有人正在录 / 预览的窗口照样出帧** ——
 *     补帧那条路的判据是 view_on_screen()（最小化让它变假），所以它反而进了补帧分支。
 *     这里不许加「最小化就不补帧」的判断，那会把录屏录成一张冻住的画面。
 *   - 客户端：X11 要告诉它（WM_STATE=IconicState + _NET_WM_STATE_HIDDEN），Wayland
 *     回一次 configure（xdg-shell 规定状态请求必须有应答），任务栏（foreign toplevel）
 *     跟着更新状态。 */
void vidow_view_set_minimized(struct vidow_view *view, bool minimized) {
	if (view == NULL || view->popup || view->minimized == minimized) {
		return;
	}
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;
	const char *title = vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(无题)";

	if (minimized) {
		/* 全屏的先退出全屏：布局里 fullscreen 那条分支不看占位，留着它会一直
		 * 占着整块屏。 */
		vidow_view_unfullscreen(view, "最小化");
		view->minimized_seq = ++server->next_minimize_seq;
	}
	view->minimized = minimized;

	if (view->kind == VIDOW_VIEW_X11 && view->x11 != NULL) {
		vidow_x11_surface_set_minimized(view->x11, minimized);
	} else if (view->xdg_toplevel != NULL &&
			vidow_view_configure_ready(view)) {
		/* 客户端第一帧还没来（surface 没 initialized）就 set_minimized 的话，
		 * 这里排 configure 会踩 schedule_configure 里那句 assert —— 见
		 * view_request_maximize 的说明；应答由初始 configure 捎过去。 */
		wlr_xdg_surface_schedule_configure(view->xdg_toplevel->base);
	}
	if (view->ftm_handle != NULL) {
		wlr_foreign_toplevel_handle_v1_set_minimized(view->ftm_handle, minimized);
	}

	wlr_log(WLR_INFO, "最小化: %s %s（%s）", title,
		minimized ? "收起" : "放回",
		view->ws != NULL ? view->ws->name : "-");

	if (minimized && server->focused == view) {
		/* 焦点交给邻居 —— 跟关窗同一套规矩（位置不动，谁滑到空出来的位置就是谁）。
		 * 必须在把窗口摘出布局**之前**找（邻居要靠它还在列里才找得到）。 */
		struct vidow_view *next = focus_after_close(server, view, output);
		server->focused = NULL;
		if (next != NULL) {
			vidow_focus_view(next);
		} else {
			focus_nearest_column(server, output);
		}
	}

	vidow_workspace_apply_visibility(server);
	vidow_arrange(server);
	/* 显隐翻转的是它自己这一块（重排里已经按每个窗口记过账了）。 */
	{
		struct wlr_box box = vidow_view_screen_box(view);
		vidow_backdrop_damage_region(server, &box);
	}
	vidow_ipc_emit_window(server, minimized ? "window-minimized" :
		"window-unminimized", view);
}

/* 放回一个最小化的窗口 + 给焦点。跨屏 / 跨工作区都会跟过去 —— 走的是「点通知」那条
 * 路（vidow_view_activate：先把工作区端出来，再给键盘）。没最小化的窗口调它没关系
 * （activate 自己会判断该不该理）。 */
void vidow_view_restore(struct vidow_view *view) {
	if (view == NULL || !view->minimized) {
		return;
	}
	vidow_view_set_minimized(view, false);
	vidow_view_activate(view);
}

/* 放回「最近收起来的那个」：优先当前聚焦屏上的，那块屏上没有才跨屏挑。 */
bool vidow_view_restore_last(struct vidow_server *server) {
	struct vidow_view *best = NULL;      /* 聚焦屏上最新的 */
	struct vidow_view *best_any = NULL;  /* 全屏范围内最新的 */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!view->minimized) {
			continue;
		}
		if (best_any == NULL || view->minimized_seq > best_any->minimized_seq) {
			best_any = view;
		}
		if (view->output == server->focused_output &&
				(best == NULL || view->minimized_seq > best->minimized_seq)) {
			best = view;
		}
	}
	struct vidow_view *pick = best != NULL ? best : best_any;
	if (pick == NULL) {
		return false;
	}
	vidow_view_restore(pick);
	return true;
}

bool vidow_view_toggle_minimized(struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return false;
	}
	if (view->minimized) {
		vidow_view_restore(view);
	} else {
		vidow_view_set_minimized(view, true);
	}
	return true;
}

void vidow_focus_column(struct vidow_server *server, int direction) {
	struct vidow_column *col =
		server->focused != NULL ? server->focused->column : NULL;
	if (col == NULL) {
		focus_nearest_column(server, server->focused_output);
		return;
	}

	if (col->ws == NULL) {
		return;
	}
	struct wl_list *node = direction > 0 ? col->link.next : col->link.prev;
	while (node != &col->ws->columns) {
		struct vidow_column *c = wl_container_of(node, c, link);
		/* 整列都最小化的列跳过：焦点不该落进一条看不见的列。 */
		if (column_has_shown_views(c)) {
			vidow_focus_view(column_first_shown_view(c));
			return;
		}
		node = direction > 0 ? node->next : node->prev;
	}
}

void vidow_focus_window(struct vidow_server *server, int direction) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		focus_nearest_column(server, server->focused_output);
		return;
	}

	/* 同列上下：跳过最小化的（它不占位，也不接键盘）。 */
	struct wl_list *node = direction > 0 ? view->column_link.next :
		view->column_link.prev;
	while (node != &view->column->views) {
		struct vidow_view *next = wl_container_of(node, next, column_link);
		if (view_shows(next)) {
			vidow_focus_view(next);
			return;
		}
		node = direction > 0 ? node->next : node->prev;
	}

	/* Single-window column (the common case): fall through to the
	 * neighbouring column so the key still does something useful. */
	vidow_focus_column(server, direction);
}

void vidow_move_column(struct vidow_server *server, int direction) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		return;
	}
	struct vidow_column *col = view->column;
	if (col->ws == NULL) {
		return;
	}

	struct wl_list *node = direction > 0 ? col->link.next : col->link.prev;
	while (node != &col->ws->columns) {
		struct vidow_column *other = wl_container_of(node, other, link);
		wl_list_remove(&col->link);
		if (direction > 0) {
			wl_list_insert(&other->link, &col->link);
		} else {
			wl_list_insert(other->link.prev, &col->link);
		}
		vidow_arrange(server);
		return;
	}
}

static void view_move_to_column(struct vidow_view *view,
		struct vidow_column *target, bool append) {
	struct vidow_column *old = view->column;
	if (old == target) {
		return;
	}

	wl_list_remove(&view->column_link);
	view->column = target;
	if (append) {
		wl_list_insert(target->views.prev, &view->column_link);
	} else {
		wl_list_insert(&target->views, &view->column_link);
	}

	if (old != NULL && !column_has_views(old)) {
		if (old->ws != NULL) {
			wl_list_remove(&old->link);
		}
		free(old);
	}
	/* 这条窗口现在住在列里了：列里的窗口不算浮窗。 */
	vidow_workspace_track(target->ws, view, false);
}

/* 插进 `target` 这条列的第 index 位（niri 的 insert into column：一列里上下叠
 * 起来）。index 数的是「目标列里现在还占着位置的窗口（不含正被拖动 / 正搬家的这
 * 一个）的第几位」—— 和落点算法（drag.c 的 insert_pos_at）数的是同一份东西：插
 * 在第 index 个**还占位置的**窗口之前，数不够就放在最后。
 *
 * 最小化的窗口不算位置（它不占条带），所以插进去的时候也是插在它们前面 —— 它们
 * 原来在上下哪一段就留在哪一段。
 *
 * 同一条列（拖自己那条列里的上下顺序）：就地重排。
 * 别的列：从原来那条列里摘出来（列空了就拆掉），再插进目标列 —— 跨工作区的调用
 * 方负责先把 view->ws / output 换过去（见 vidow_workspace_move_view_into_column）。 */
bool vidow_view_insert_into_column(struct vidow_view *view,
		struct vidow_column *target, int index) {
	if (view == NULL || target == NULL || view->popup) {
		return false;
	}
	if (index < 0) {
		index = 0;
	}
	struct vidow_column *old = view->column;
	if (old == target) {
		wl_list_remove(&view->column_link);
	} else {
		/* 记住「并进别人列之前」自己的列宽：再拖出来时恢复它，而不是把
		 * 目标列的宽度带走。已经在别的列里（home 已记）就不覆盖。 */
		if (old != NULL && !view->column_width_home_set) {
			view->column_width_home = old->width_fraction;
			view->column_width_home_set = true;
			wlr_log(WLR_INFO, "column: %s 并进别的列，记住原列宽 %.2f",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "?",
				view->column_width_home);
		}
		vidow_view_leave_column(view);
	}

	view->column = target;
	struct wl_list *at = &target->views; /* 默认：放在最后 */
	int i = 0;
	struct vidow_view *it;
	wl_list_for_each(it, &target->views, column_link) {
		if (!view_shows(it)) {
			continue;
		}
		if (i == index) {
			at = &it->column_link;
			break;
		}
		i++;
	}
	/* wl_list_insert(pos, elm) 是插在 pos 之后 —— 插到 at 前面要写 at->prev。 */
	wl_list_insert(at->prev, &view->column_link);
	if (old != target) {
		/* 这条窗口现在住在列里了：列里的窗口不算浮窗。 */
		vidow_workspace_track(target->ws, view, false);
	}
	return true;
}

/* 把窗口从它那条列里拎出来、单独成一条新列（niri 的 expel-window-from-column）。
 * 这是「并进一列」的逆操作：并进一列有两条路（拖拽落点 / move-window-direction），
 * 取出来以前一条都没有 —— 用户报的「窗口放入列后无法取出」就是它。
 *
 * 插在 ws 的列带里 `at` 那个位置**之前**（at = &ws->columns 就是追加到末尾；
 * NULL = 插在原来那条列右边）。落点由调用方算：动作 / IPC 那条路是「原来那条列的
 * 右边 / 左边」，拖拽那条路是落点算法（drag.c 的 insert_pos_at）给的那个缝 ——
 * 两边的「第几位」数的是同一份东西。
 *
 * 列里只剩它一个的时候没什么可取的（它本来就是独立一条列），浮窗 / 弹出物本来就
 * 不在列里 —— 这两种都返回 NULL，调用方报一句人话 / 走老路。
 *
 * 宽度跟着原来那条列走（同一个 width_fraction）：取出来是为了单独挪它 / 单独调它
 * 的宽度，尺寸突变一下没有意义。**列最大化状态不带过来** —— 新列不该一出生就是
 * 满宽（用户按下「取出来」要的是「它自己一条列」）。reason 只进日志。 */
struct vidow_column *vidow_view_expel_new_column(struct vidow_view *view,
		struct vidow_workspace *ws, struct wl_list *at, const char *reason) {
	if (view == NULL || ws == NULL || view->popup ||
			view->float_mode != VIDOW_FLOAT_NONE || view->column == NULL ||
			!view_shows(view)) {
		return NULL;
	}
	struct vidow_column *col = view->column;
	if (col->ws == NULL || col->ws != ws) {
		return NULL;
	}
	/* 列里还有别的**看得见**的窗口，才算「它在一条多窗口的列里」。收起来的
	 * （最小化）不算 —— 它们在布局里不占位，取出来对它们没有影响。 */
	bool stacked = false;
	struct vidow_view *it;
	wl_list_for_each(it, &col->views, column_link) {
		if (it != view && view_shows(it)) {
			stacked = true;
			break;
		}
	}
	if (!stacked) {
		return NULL;
	}

	struct vidow_server *server = view->server;
	struct vidow_column *ncol = calloc(1, sizeof(*ncol));
	if (ncol == NULL) {
		return NULL;
	}
	ncol->server = server;
	ncol->id = ++server->next_column_id;
	ncol->output = col->output;
	ncol->ws = col->ws;
	/* 取出来时优先恢复「并进这条列之前」自己的宽度；没有记录才继承当前列宽
	 * （直接 expel 一个本来就在这条列里的窗口走这一支）。 */
	ncol->width_fraction = view->column_width_home_set ?
		view->column_width_home : col->width_fraction;
	view->column_width_home_set = false;
	/* 新列继承的是**目标**宽度（原列正过渡到一半也照目标给），新列自己不过渡。 */
	ncol->width_shown = ncol->width_fraction;
	wl_list_init(&ncol->views);
	wl_list_init(&ncol->link);

	vidow_view_leave_column(view);
	view->column = ncol;
	wl_list_insert(ncol->views.prev, &view->column_link);
	if (at == NULL) {
		at = col->link.next; /* 默认：原来那条列的右边 */
	}
	/* wl_list_insert(pos, elm) 是插在 pos 之后 —— 插到 at 前面要写 at->prev。 */
	wl_list_insert(at->prev, &ncol->link);
	/* 和 insert_into_column 一样：这条窗口现在住在列里（不是浮窗）。 */
	vidow_workspace_track(ncol->ws, view, false);

	vidow_arrange(server);
	vidow_ipc_emit_column(server, "column-expel-window", ncol);
	wlr_log(WLR_INFO, "取出窗口: %s 从列 #%llu 到新列 #%llu（%s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		(unsigned long long)col->id, (unsigned long long)ncol->id,
		reason != NULL ? reason : "取出来");
	return ncol;
}

/* 动作 / IPC 那条路：把窗口从它那条列里拎出来单独成一条新列。
 * direction > 0 = 新列插在原来那条列**右边**（niri 的默认），< 0 = 左边。
 * 取不出来（浮窗 / 列里就它一个）返回 false，调用方报一句人话。 */
bool vidow_view_expel_from_column(struct vidow_view *view, int direction) {
	if (view == NULL || view->column == NULL) {
		return false;
	}
	struct vidow_column *col = view->column;
	/* at 得在摘之前算：wl_list_insert(at->prev, …) 插在 at 前面，而
	 * col->link.next 就是「原来那条列的右边」那个位置。 */
	struct wl_list *at = direction > 0 ? col->link.next : &col->link;
	return vidow_view_expel_new_column(view, col->ws, at,
		direction > 0 ? "右边" : "左边") != NULL;
}

/* 第 index 个「还占着位置」的窗口（0 起数，越界返回 NULL）。 */
static struct vidow_view *column_shown_view_at(struct vidow_column *col,
		int index) {
	if (index < 0) {
		return NULL;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		if (view_shows(view) && index-- == 0) {
			return view;
		}
	}
	return NULL;
}

/* `view` 在它那条列里排第几（按「还占着位置」的数；它自己最小化时 -1）。 */
static int column_shown_index(struct vidow_view *view) {
	if (view == NULL || view->column == NULL || !view_shows(view)) {
		return -1;
	}
	int index = 0;
	struct vidow_view *it;
	wl_list_for_each(it, &view->column->views, column_link) {
		if (it == view) {
			return index;
		}
		if (view_shows(it)) {
			index++;
		}
	}
	return -1;
}

/* 换位置（vidow_swap_window）之前，这个窗口**要求**自己那格多宽。
 *
 * 换位置是「窗口搬进对方那一格」，宽度跟着窗口走，所以目标宽度得按窗口自己的
 * 来源重算，不能直接沿用格子里原先那个数。来源优先级和建列时一致（见
 * column_new_for 与 insert_into_column）：
 *   ① 窗口规则写了 column_width —— 最强，按 app_id / title 匹配的那个；
 *   ② 这个窗口「并进别的列之前」记住的原列宽（column_width_home）；
 *   ③ 都没有 → 维持这一格现在的宽度（返回 fallback）。
 * 列最大化（maximized）的格子不进这条判断：它的宽度是 1.0 撑满，换人进来
 * 还是撑满，vidow_column_set_width 会把 maximized 状态自己接上。 */
static double view_swap_width(const struct vidow_view *view,
		const struct vidow_column *fallback) {
	if (view == NULL) {
		return fallback != NULL ? fallback->width_fraction : 0.5;
	}
	if (view->rule_eff.set[VRA_COLUMN_WIDTH]) {
		return view->rule_eff.val[VRA_COLUMN_WIDTH].d;
	}
	if (view->column_width_home_set) {
		return view->column_width_home;
	}
	return fallback != NULL ? fallback->width_fraction : 0.5;
}

/* 和相邻那列里的窗口**换个位置**（niri 那种 swap）：两个窗口各自住进对方那一格，
 * 焦点跟着自己走 —— 列里只有一个窗口时（常见情况）就是两条列的内容对调。
 *
 * 跟谁换：目标列里「纵向同一层」那个（层数不够就取最后一个）。一列里叠了好几个
 * 窗口时，这样换完两个窗口都还在原来那一层，不会跳层。整列都最小化了的列跳过
 * （它不占条带，换过去等于凭空挪一屏）。 */
bool vidow_swap_window(struct vidow_server *server, int direction) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL ||
			view->float_mode != VIDOW_FLOAT_NONE ||
			!view_shows(view)) {
		return false;
	}
	struct vidow_column *col = view->column;
	if (col->ws == NULL || direction == 0) {
		return false;
	}
	int index = column_shown_index(view);

	struct wl_list *node = direction > 0 ? col->link.next : col->link.prev;
	while (node != &col->ws->columns) {
		struct vidow_column *other = wl_container_of(node, other, link);
		struct vidow_view *mate =
			column_shown_view_at(other, index);
		if (mate == NULL && column_has_shown_views(other)) {
			/* 对方那列层数不够：换成它最后一个（最靠下的那个）。 */
			struct vidow_view *last = NULL;
			struct vidow_view *it;
			wl_list_for_each(it, &other->views, column_link) {
				if (view_shows(it)) {
					last = it;
				}
			}
			mate = last;
		}
		if (mate == NULL || mate == view) {
			node = direction > 0 ? node->next : node->prev;
			continue;
		}
		/* 两格的前驱都得先记下来：wl_list_remove 会把节点的 prev/next 清掉。 */
		struct wl_list *a_prev = view->column_link.prev;
		struct wl_list *b_prev = mate->column_link.prev;
		wl_list_remove(&view->column_link);
		wl_list_remove(&mate->column_link);
		wl_list_insert(b_prev, &view->column_link);
		wl_list_insert(a_prev, &mate->column_link);
		view->column = other;
		mate->column = col;
		/* 宽度跟着**窗口**走（README 的语义：「两个窗口各自住进对方那一格」，
		 * 焦点也跟着窗口）。只换 wl_list 指针不够 —— 两条列的 width_fraction
		 * 各是各的，换完窗口就继承了**对方格子的宽度**，屏上看起来正是
		 * 「半宽窗口和全宽窗口交换时连宽度都交换了」（用户 2026-10-20 报的）。
		 *
		 * 两个值都得在换列**之前**算：换完 col / other 里住的是新住户，
		 * 再问「这一格原来多宽」就问不到了。 */
		double vw = view_swap_width(view, col);
		double mw = view_swap_width(mate, other);
		vidow_column_set_width(col, mw, false);
		vidow_column_set_width(other, vw, false);
		vidow_arrange(server);
		return true;
	}
	return false;
}

void vidow_move_window(struct vidow_server *server, int direction) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		return;
	}
	struct vidow_column *col = view->column;

	struct wl_list *node = direction > 0 ? view->column_link.next :
		view->column_link.prev;
	if (node != &col->views) {
		/* swap with the neighbouring window in the same column */
		wl_list_remove(&view->column_link);
		if (direction > 0) {
			wl_list_insert(node, &view->column_link);
		} else {
			wl_list_insert(node->prev, &view->column_link);
		}
		vidow_arrange(server);
		return;
	}

	/* Only window in the column: push it into the neighbouring column, which
	 * is how a column ends up stacking several windows. */
	if (col->ws == NULL) {
		return;
	}
	node = direction > 0 ? col->link.next : col->link.prev;
	while (node != &col->ws->columns) {
		struct vidow_column *other = wl_container_of(node, other, link);
		view_move_to_column(view, other, direction > 0);
		vidow_arrange(server);
		return;
	}
}

/* ---------------------------------------------------------- 列宽过渡 */

/* 这一帧该按哪个比例画。补间在跑就是补间当前值，否则就是逻辑值。 */
static double column_layout_fraction(const struct vidow_column *col) {
	return col->anim_width != NULL ? col->width_shown : col->width_fraction;
}

static void column_width_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_column *col = data;
	col->width_shown = col->anim_from + (col->anim_to - col->anim_from) * t;
	/* 重排推到下一帧的开头去做（output.c 的 arrange_dirty）。**不能在这儿直接
	 * 调 vidow_arrange**：arrange 会掐掉 / 新起别的补间，而我们此刻正站在
	 * anim.c 的遍历里，动的可能就是它手里那个 tmp 指针。 */
	col->server->arrange_dirty = true;
}

static void column_width_done(void *data, bool cancelled) {
	(void)cancelled;
	struct vidow_column *col = data;
	col->anim_width = NULL;
	if (col->server->column_anim_n > 0) {
		col->server->column_anim_n--;
	}
	col->server->arrange_dirty = true;
}

void vidow_column_set_width(struct vidow_column *col, double fraction, bool animate) {
	if (col == NULL) {
		return;
	}
	struct vidow_server *server = col->server;
	/* 起点取「现在画在屏上的那个值」，不是逻辑值 —— 过渡没跑完就再改一次时，
	 * 得从眼前这个宽度接着走，不能跳回上一次的目标。**必须在 cancel 之前取**：
	 * cancel 会把 anim_width 清掉，之后就问不出中间值了。 */
	double from = column_layout_fraction(col);

	col->width_fraction = fraction;

	int ms = server->config.column_width_ms;
	if (!animate || ms <= 0 || from == fraction) {
		vidow_anim_cancel(col->anim_width);
		col->anim_width = NULL;
		col->width_shown = fraction;
		/* **当场重排**，不能只挂个 arrange_dirty：没有过渡就没有动画在推帧
		 * （动画那条路靠看门狗保证帧），静态屏幕上下一帧可能几百毫秒之后才来
		 * —— 一次「直接到位」的改宽得当场就位。 */
		vidow_arrange(server);
		return;
	}

	vidow_anim_cancel(col->anim_width);
	col->anim_width = NULL;
	col->anim_from = from;
	col->anim_to = fraction;
	col->anim_width = vidow_anim_start(server, "column-width", ms,
		server->config.column_width_easing, column_width_update,
		column_width_done, col);
	if (col->anim_width == NULL) {
		/* 分配失败：别把宽度卡在中间，直接到位。 */
		col->width_shown = fraction;
		vidow_arrange(server);
		return;
	}
	server->column_anim_n++;
	/* 起点当场摆一次：从按下到下一帧之间不该还停在上一次的宽度上。 */
	column_width_update(col, 0.0, 0.0);
	wlr_log(WLR_DEBUG, "column-width tween: %.3f -> %.3f（%dms）", from,
		fraction, ms);
}

void vidow_cycle_column_width(struct vidow_server *server) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		return;
	}

	/* 预设表来自配置（preset_column_widths，configs/20-layout.lua）：默认
	 * 1/3、1/2、2/3，就是原来写死在这儿的那三个。表为空的话兜一个 0.5 ——
	 * 配置解析保证至少一档，这里只是不给「空表除以零」的机会。 */
	struct vidow_layout lay;
	vidow_layout_for(server, view->output, &lay);
	const double *presets = lay.preset_column_widths;
	size_t n = (size_t)lay.preset_column_widths_count;
	if (n == 0) {
		static const double fallback[] = { 0.5 };
		presets = fallback;
		n = 1;
	}
	struct vidow_column *col = view->column;

	/* 手动改宽度 = 退出「列最大化」这个状态：先把它按下来（宽度留在 1.0，
	 * 于是下面按预设找「当前是第几档」时它落在最后一档之后，循环回到 1/3 ——
	 * 也就是「满宽再循环一次回到最窄」）。 */
	col->maximized = false;

	/* 「现在是第几档」按**目标**（width_fraction）数，不按画到一半的值 ——
	 * 连按几下要一档一档往上走，不能因为动画没跑完就原地打转。 */
	size_t current = n - 1;
	for (size_t i = 0; i < n; i++) {
		if (col->width_fraction < presets[i] + 0.01) {
			current = i;
			break;
		}
	}
	vidow_column_set_width(col, presets[(current + 1) % n], true);
	vidow_arrange(server);
}

void vidow_column_set_maximized(struct vidow_column *col, bool maximized) {
	if (col == NULL || col->maximized == maximized) {
		return;
	}
	struct vidow_server *server = col->server;

	if (maximized) {
		col->maximized_saved = col->width_fraction;
		col->maximized = true;
		vidow_column_set_width(col, 1.0, true);
	} else {
		col->maximized = false;
		vidow_column_set_width(col, col->maximized_saved, true);
	}

	/* 列里每个客户端都被告知一声：它们自己也知道自己现在是不是最大化的
	 * （xdg 的 set_maximized / X11 的 _NET_WM_STATE_MAXIMIZED_*）—— 客户端
	 * 以为自己是普通尺寸、布局却当它最大化，那种「一半一半」的状态最难看。
	 * 状态没变的话 vidow_view_set_maximized 自己会挡掉（去重在它里面）。
	 *
	 * 顺带把「谁要的」那笔账理一遍：**关掉**最大化的时候作废（这一轮最大化结束
	 * 了，下次客户端再撤销就不该算在它头上）；**打开**的时候不动 —— 客户端请求
	 * 那条路正是先立标记、再调这儿（见 view_request_maximize），在这儿清掉的话
	 * 它自己的撤销请求就没人理了。 */
	struct vidow_view *view;
	wl_list_for_each(view, &col->views, column_link) {
		if (!maximized) {
			view->maximize_granted = false;
		}
		vidow_view_set_maximized(view, maximized);
	}

	vidow_arrange(server);
	vidow_ipc_emit_column(server, "column-maximize-changed", col);
}

void vidow_column_toggle_maximized(struct vidow_server *server) {
	struct vidow_view *view = server->focused;
	if (view == NULL || view->column == NULL) {
		return;
	}
	vidow_column_set_maximized(view->column, !view->column->maximized);
}

void vidow_view_toggle_fullscreen(struct vidow_view *view) {
	view_apply_fullscreen(view, !view->fullscreen, "toggle-fullscreen（Mod+F / IPC）");
}

/* ------------------------------------------------------------- events */

/* 弹出物要收键盘（菜单的方向键、Esc，dmenu 的输入），但它不改「谁是焦点窗口」：
 * 只把 seat 的键盘焦点挪到它自己的表面上，server->focused 一动不动 —— 父窗口的
 * 焦点边框因此不会灭，关掉菜单后焦点也不会被重新算到别的窗口上去。
 * Wayland 那边同理：xdg_popup 不接管焦点，客户端从 toplevel 自己转发按键。 */
static void view_popup_grab_keyboard(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL || seat->keyboard_state.focused_surface == surface) {
		return;
	}
	if (server->keyboard_layer != NULL) {
		/* 独占层拿着键盘：跟 vidow_focus_view 一样，不去抢。 */
		return;
	}
	/* 统一入口：焦点离开旧表面时补松开、清残留修饰位，新表面以干净状态
	 * 进入（见 vidow_seat_focus_surface）。 */
	vidow_seat_focus_surface(seat, surface);
}

/* 弹出物收起来了：键盘还给焦点窗口；连焦点窗口都没了就当没人要键盘。 */
static void view_popup_release_keyboard(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_surface *mine = vidow_view_surface(view);
	if (mine == NULL || seat->keyboard_state.focused_surface != mine) {
		return; /* 焦点已经不在它身上了（比如紧接着又弹了一个菜单） */
	}
	if (server->keyboard_layer != NULL) {
		return;
	}
	struct wlr_surface *back = server->focused != NULL ?
		vidow_view_surface(server->focused) : NULL;
	if (back == NULL) {
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}
	/* 统一入口：焦点离开旧表面时补松开、清残留修饰位，新表面以干净状态
	 * 进入（见 vidow_seat_focus_surface）。 */
	vidow_seat_focus_surface(seat, back);
}

/* 「表面还没提交缓冲区」的窗口：等第一帧到了再补一次归属判定。
 *
 * 抄的是 Hyprland 的 considerMap()（src/xwayland/XSurface.cpp）：map 那一刻
 * 表面可能还没有 texture，这时尺寸（current.width/height）全是 0，拿它做
 * 任何判断都是错的 —— Hyprland 干脆什么都不做，等 commit 里再调一次。
 *
 * 我们这边 view_map() 已经把 mapped 置位了，commit 不会再走一遍 view_map，
 * 所以这里补一个等价入口：条件成立时把窗口落进工作区（不进布局的那批
 * 早在 view_map 里就 return 了，has_buffer 到了也不该进来）。 */
static void view_reconsider_map(struct vidow_view *view);

/* 弹出物判据（定义见文件后半段）—— view_map 的辅助窗口判据要用它。 */
static bool x11_is_popup(const struct vidow_x11_surface *x);
static void x11_trace_view(const char *event, struct vidow_view *view,
		struct wlr_surface *surface);
void view_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, map);
	struct vidow_server *server = view->server;
	x11_trace_view("map-enter", view, NULL);
	/* XWayland reassociation may synthesize map before the real map signal
	 * arrives. Keep the lifecycle edge idempotent: duplicate map handling would
	 * reapply rules, focus, animations, and window-opened notifications. */
	if (view->mapped) {
		return;
	}

	if (vidow_flip_on()) {
		vidow_flip_note("mapped", view, true);
	}
	view->mapped = true;
	vidow_portal_view_mapped(view, true);
	/* 窗口规则先判一遍：float / workspace / output 决定它落在哪儿、怎么摆，
	 * 下面「归哪块工作区、平铺还是浮动」的判据都要看它（app_id / 标题这时
	 * 都齐了）。开窗只兑现一次的那几条在工作区落定之后（vidow_rule_placement）。 */
	vidow_rule_apply(server, view);
	/* X11 的窗口可能「状态比表面先到」：全屏那条属性在这儿先兑现一次。 */
	view_sync_x11_fullscreen(view);

	/* ---------------------------------------------------- 尺寸异常窗口
	 *
	 * 拿 Hyprland 的做法当参照（src/xwayland/XWM.cpp 的 handleMapRequest 和
	 * XSurface.cpp 的 considerMap），它**没有**「把窗口藏起来」这套机制：
	 *
	 *   1. considerMap() 只看一件事 —— **表面有没有提交过缓冲区**
	 *      （m_surface->m_current.texture）。没有就什么都不做，等下一次
	 *      commit 再来（commit 里会再调一次 considerMap）。有了就 map。
	 *   2. handleMapRequest() 里对「太小」的窗口**不隐藏，而是 configure 到
	 *      800x800** 让它变成正常窗口：
	 *          SMALL = geometry.size() < {2,2} || size < sizeHints.min
	 *          if (SMALL && !overrideRedirect) configure({pos, {800,800}});
	 *   3. override_redirect 只记状态（updateOverrideRedirect），从不藏。
	 *
	 * 也就是说 Hyprland 宁可让一个 1x1 窗口占位，也绝不把它从画面里拿掉。
	 *
	 * 我们原来那条「≤1x1 → 关场景节点 + focus_helper + return」正好反着：
	 * 关掉的节点要等 unmap 才恢复，于是**任何被误判的窗口本会话内永久不可见**。
	 * Steam 账户栏的 hover 菜单就是这么变成整块全黑的 —— 它是自绘浮层，
	 * 建窗那一下表面还没提交缓冲（current.width == 0），一头撞进「≤1x1」。
	 *
	 * 改成两条：
	 *   A. **没提交过缓冲区就先不判**（照抄 considerMap）：等 commit 里重新
	 *      走一遍 view_map 的判定，那时尺寸是真的。
	 *   B. 判定成立时**只做「不进布局」**，节点照常显示。真·辅助窗口本来
	 *      就画不出东西，多一个空节点没有任何代价；被误判的窗口从此还能
	 *      上屏 —— 用「看不见」去换「不占位」不划算。
	 *
	 * 另外判据加一道类型闸：弹出物（override-redirect / window_type）一律
	 * 不碰，菜单和提示必须显示，这是它们存在的全部意义。 */
	struct wlr_surface *helper_surface = vidow_view_surface(view);
	bool has_buffer = helper_surface != NULL &&
		helper_surface->current.buffer_width > 0 &&
		helper_surface->current.buffer_height > 0;
	bool popup_typed = view->popup ||
		(view->x11 != NULL && x11_is_popup(view->x11));
	if (!popup_typed && !has_buffer) {
		/* A：还没交缓冲，尺寸不可信。等 commit 再判（view_surface_commit
		 * 里会在有缓冲时补一次 view_map 的判定）。 */
		wlr_log(WLR_DEBUG, "map: %s 还没提交缓冲区，先不定归属（等 commit）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
	} else if (!popup_typed && helper_surface != NULL &&
			helper_surface->current.width <= 1 &&
			helper_surface->current.height <= 1) {
		/* B：只「不进布局、不占列」，**不动场景节点**。
		 *
		 * 这才是辅助窗口真正需要的效果（它只要一个能 set_selection 的键盘
		 * serial）；把它从画面上拿掉属于过度处置 —— 误判一次就是永久黑屏。 */
		view->focus_helper = true;
		/* **借出前先记下焦点在哪**（抄 Hyprland 单点存焦点的做法）。
		 *
		 * 以后归还时只认这一格，不再读 server->focused —— 借出期间任何一次
		 * 重算焦点都会改掉 server->focused，读它就是「粘贴一次、焦点跳到
		 * 下一个窗口」那条老毛病。 */
		server->focus_borrowed_from = server->focused;
		vidow_seat_focus_surface(server->seat, helper_surface);
		vidow_im_focus(server, helper_surface);
		wlr_log(WLR_INFO, "map: %s 是 1x1 辅助窗口，只借键盘、不进布局"
			"（焦点借自 %s）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			server->focus_borrowed_from != NULL ?
				(vidow_view_title(server->focus_borrowed_from) != NULL ?
					vidow_view_title(server->focus_borrowed_from) :
					"(无题)") : "(无)");
		 x11_trace_view("map-helper-return", view, helper_surface);
		return;
	}


	/* 落进现在显示着的那块工作区：新窗口跟着走，不偷偷开在藏起来的工作区里。
	 * 列的窗口在 view_ensure_column 里落工作区；浮窗落在这里。 */
	if (!view->popup) {
		if (view->column != NULL && view->column->ws != NULL) {
			view->ws = view->column->ws;
		} else if (view->output != NULL) {
			/* 新窗口落**光标那块屏**现在显示着的工作区，不是「全局焦点屏」
			 * 的那块。view->output 在创建时就是按光标选的（view_default_output），
			 * 而 server->focused_output 可能还停在上一块屏上（点空区域 / 切
			 * 工作区之后没跟上）—— 那会让窗口开在 A 屏、操作却按 B 屏算。 */
			view->ws = vidow_workspace_shown_for(server, view->output);
		} else {
			view->ws = vidow_workspace_shown(server);
		}
	}
	/* 弹出物不进工作区，它跟父窗口走：父窗口在显示着的那块工作区里，它才显示。
	 * 普通窗口不靠「关节点」藏起来（关了按窗口录屏 / 预览就是黑的），节点一直
	 * 开着，藏不藏由 scene_parked 定 —— 这一帧的 arrange 里就会算准（见
	 * workspace.c 的 vidow_workspace_apply_visibility）。 */
	bool show = view->popup ?
		(view->parent == NULL ||
			vidow_workspace_view_visible(server, view->parent)) :
		vidow_workspace_view_visible(server, view);
	if (view->popup) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, show);
	} else {
		vidow_view_set_parked(view, !show);
		wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	}
	vidow_backdrop_damage_view(view);

	struct wlr_box geo = vidow_view_geometry(view);
	struct wlr_surface *surface = vidow_view_surface(view);
	wlr_log(WLR_INFO, "map %s: surface %dx%d geometry offset %d,%d",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(untitled)",
		surface != NULL ? surface->current.width : 0,
		surface != NULL ? surface->current.height : 0, geo.x, geo.y);

	/* Re-run detection here: clients that pin a fixed size (dialogs) only
	 * send their min/max hints after the first configure round-trip.
	 * 规则写了 float 就以规则为准（vidow_float_update_mode 认 view->rule_eff）。 */
	vidow_float_update_mode(view);

	if (view->float_mode == VIDOW_FLOAT_NONE) {
		view_ensure_column(server, view);
		/* 落进一条已经最大化的列（往最大化的列里又开/搬进来一个窗口）：告诉它
		 * 一声，别让它以为自己是普通尺寸、而布局把它按整列宽度摆。xdg 那边
		 * 「抢跑的最大化请求」在 initial_commit 里已经兑过了，这里被去重挡掉。 */
		if (view->column != NULL && view->column->maximized) {
			vidow_view_set_maximized(view, true);
		}
	} else if (!view->popup) {
		vidow_workspace_track(view->ws, view, true);
	}
	/* 规则的「开出来就这样」：工作区 / 屏 / 尺寸 / 居中 / 全屏 / 最大化 / 最小化。
	 * 放在这里是因为工作区和列都已经落定（最小化收起来的窗口下面就不抢焦点了）。 */
	vidow_rule_placement(server, view);
	/* 弹出物不抢焦点：它不装饰，焦点边框长在父窗口上，父窗口的边框不该因为
	 * 弹出一个菜单就灭掉；关掉菜单后焦点也不该被重新算到别的窗口上去。
	 * 键盘照给它（X11 的菜单要收方向键、Esc），见 view_popup_grab_keyboard。 */
	if (view->popup) {
		/* 分类往往发生在表面建出来之前（那时候没东西可摆），所以这里再按 X
		 * 说的地方摆一次；顺带保证第一次上屏就落在对的位置上，不会闪一下。 */
		vidow_popup_place(view);
		/* **X11 弹出物不拿 wl_keyboard 焦点。**
		 *
		 * 原来这里调 view_popup_grab_keyboard()，理由是「菜单要收方向键 /
		 * Esc」。但 X11 的菜单根本不靠 wl_keyboard 收键：它在 X server 内部
		 * 自己 XGrabKeyboard（Xwayland 就是那个 server），键盘事件走 X 自己
		 * 的 grab 路由，合成器插不插手都一样。
		 *
		 * 代价却是实打实的：notify_enter 一改 seat 的 focused_surface，
		 * Xwayland 那边会跟着重算指针 enter/leave，于是菜单刚出来就被判成
		 * 「指针不在我身上」，自己关掉。实测日志（2026-10-27 00:16:16–21，
		 * 连按右键 5 次）每次都是这个顺序：
		 *   x11: 新窗口 (无题)（override-redirect，弹出物…）
		 *   焦点: 好友列表 -> (无题)      ← 这里
		 *   焦点: (无) -> 好友列表
		 * 即「菜单抢一次键盘 → 立刻被关 → 键盘还回去」。 */
		if (view->kind != VIDOW_VIEW_X11) {
			view_popup_grab_keyboard(view);
		}
	} else if (view->pending_activate) {
		/* 有人早就请求激活它了（点通知的时候窗口还没上屏）：现在兑现。 */
		view->pending_activate = false;
		vidow_view_activate(view);
	} else if (!view->focus_on_open && server->focused != NULL) {
		/* 规则说这个应用开窗口别抢焦点（Electron 系的通知 / 剪贴板辅助窗口
		 * 就是这样：开一个看不见的窗口把你正在打字的窗口顶掉）。窗口照常
		 * 出现、照常落在当前工作区，只是不动键盘焦点。当前一个焦点窗口都
		 * 没有时（server->focused == NULL）还是给它 —— 否则没人收键盘。 */
		wlr_log(WLR_INFO, "map: %s 不抢焦点（窗口规则 focus_on_open = false）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
	} else {
		vidow_focus_view(view);
	}
	/* fake_focus 的窗口如果这次没拿到真焦点（focus_on_open = false，或者
	 * 前面已经有焦点窗口），上面那次规则套用时可能还没 configure 好。上屏
	 * 之后再补一次 activated(true)，别让它一出生就是未激活。 */
	if (view->fake_focus && server->focused != view) {
		vidow_view_set_activated(view, true);
	}

	/* Everything above placed the window where it belongs; now play the
	 * opening effect on top of that final position. */
	vidow_view_open_anim(view);

	vidow_view_note_names(view);
	vidow_ipc_emit_window(server, "window-opened", view);
}


/* 见上面的声明：表面第一帧到了、而当时因为「还没有缓冲」没定归属的，
 * 在这儿补一次。重复调用是安全的（有工作区就不再动）。 */
static void view_reconsider_map(struct vidow_view *view) {
	if (view == NULL || view->x11 == NULL || view->server == NULL) {
		return;
	}
	struct wlr_surface *surface = vidow_view_surface(view);
	if (surface == NULL || surface->current.buffer_width <= 0 ||
			surface->current.buffer_height <= 0) {
		return; /* 还是没缓冲 */
	}
	if (view->popup || view->focus_helper) {
		return; /* 弹出物 / 已按辅助窗口处理过 */
	}
	if (view->ws != NULL) {
		return; /* 已经有归属了 */
	}
	if (surface->current.width <= 1 && surface->current.height <= 1) {
		/* 第一帧就是 1x1：现在尺寸可信了，按辅助窗口办（只拦布局，
		 * 不动场景节点 —— 见 view_map 里那段）。 */
		view->focus_helper = true;
		wlr_log(WLR_INFO, "commit: %s 第一帧确认是 1x1 辅助窗口，不进布局",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
		return;
	}
	if (view->output != NULL) {
		view->ws = vidow_workspace_shown_for(view->server, view->output);
	} else {
		view->ws = vidow_workspace_shown(view->server);
	}
	if (view->ws != NULL) {
		wlr_log(WLR_INFO, "commit: %s 第一帧到了（%dx%d），补进工作区 %s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			surface->current.width, surface->current.height,
			view->ws->name != NULL ? view->ws->name : "-");
	}
	view_ensure_column(view->server, view);
	vidow_arrange(view->server);
}

void view_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, unmap);
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;
	x11_trace_view("unmap-enter", view, NULL);

	/* **幂等**：跟 view_map 开头那道 `if (view->mapped) return;` 对称。
	 *
	 * XWayland 那条路会从好几个地方调 xwm_surface_sync()（property notify /
	 * MapNotify / UnmapNotify / 我们自己 replay 之后），而 should_be_mapped()
	 * 判的是「有没有 buffer」—— 同一个 unmap 状态可能被连着判中两次，于是
	 * view_unmap 跑两遍：portal 收两次通知、几何重置两次、动画队列里塞两笔。
	 * 状态不会错（都写同一个值），但每次都要重排 + 重画，是白烧掉的一帧。
	 * map 那边早有这道闸，unmap 这边漏了。 */
	if (!view->mapped) {
		return;
	}

	if (vidow_flip_on()) {
		vidow_flip_note("mapped", view, false);
	}
	view->mapped = false;
	/* 忘了「上一次钉过几倍」：客户端 unmap 之后可能换一个新 surface 回来
	 * （Chromium 的 widget 重建就是这样），新 surface 需要我们重新发一次
	 * preferred_buffer_scale。和 Hyprland 的 unmap 监听
	 * （m_scale = std::nullopt）同一个意思。 */
	view->pref_scale = 0;
	/* XWayland 窗口 unmap 后需重置 x11_map_replayed：不然下次 map request
	 * 会认为「已经 replay 过」直接跳过 view_map，导致 scene node 不启用 → 黑屏。
	 * Steam 主界面第二次打开黑屏就是这个原因。 */
	view->x11_map_replayed = false;
	/* 同理，configure 的去重账也得作废。`cfg_sent` 记的是「上一次发出去的
	 * 位置尺寸」，去重是为了别让客户端白重画一帧；但 unmap 之后这次协商就
	 * 结束了，下一次 map 是一次全新的协商 —— 客户端在等我们说「按这个尺寸
	 * 画」，我们却拿上一次的账认定「已经说过了」，一句话都不发。
	 *
	 * Steam 重开主窗口复用同一个 X 窗口、尺寸和上次一模一样，于是
	 * wlr_xwayland_surface_configure 一次都没被调用（日志里 x11-cfg 一条
	 * 都没有），它自己画一帧就停下来等 ConfigureNotify，永远等不到 ——
	 * 画面上就是黑的，`vidowmsg` 里 mapped / visible / placed 全是对的。
	 * 首开之所以正常，只是那时候 cfg_sent 还是 false。
	 * 拖动之所以能救回来，是因为位置变了，去重条件不成立，configure 重新发
	 * 了出去。 */
	view->cfg_sent = false;
	vidow_portal_view_mapped(view, false);
	if (view->focus_helper) {
		/* 辅助窗口走了：键盘还给原来的焦点窗口。它从来没进过 server->focused，
		 * 所以不能走下面的 focus_after_close（那会把焦点算到“下一个”）。 */
		view->focus_helper = false;
		/* **还给它借走时那个窗口**，不是 server->focused。
		 *
		 * 原来读 server->focused：借出期间任何一次重算焦点都会把这个指针
		 * 换掉，于是「粘一次，焦点跳到下一个窗口」。现在只认借出那一刻
		 * 记下的那一格（Hyprland 的 m_focusWindow 也是单点、也是同一用途）。
		 *
		 * 那个窗口可能已经关了：mapped 为假 / 表面没了就退回 server->focused，
		 * 再不行就清焦点 —— 绝不往已销毁的 view 上送。 */
		struct vidow_view *owner = server->focus_borrowed_from;
		server->focus_borrowed_from = NULL;
		if (owner != NULL && (!owner->mapped ||
				vidow_view_surface(owner) == NULL)) {
			owner = server->focused;
		} else if (owner == NULL) {
			owner = server->focused;
		}
		struct wlr_surface *back = owner != NULL ?
			vidow_view_surface(owner) : NULL;
		vidow_seat_focus_surface(server->seat, back);
		vidow_im_focus(server, back);
		wlr_log(WLR_INFO, "unmap: %s 归还键盘焦点 -> %s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			owner != NULL ? (vidow_view_title(owner) != NULL ?
				vidow_view_title(owner) : "(无题)") : "(无)");
		/* **不关场景节点**（和 view_map 那边保持一致）。
		 *
		 * 关节点曾经是「藏起辅助窗口」的第二步，但它有副作用：节点一旦关掉，
		 * 这条 view 的 scene_tree 在本会话剩余时间里就一直停成 disabled ——
		 * 客户端再来一次 map（辅助窗口经常 unmap/map 反复）时，view_map()
		 * 会提前 return（focus_helper 分支），没人再把它打开。于是窗口活着、
		 * 却永远看不见。
		 *
		 * 节点开着没有代价：真·辅助窗口本来就不画东西。 */
		return;
	}
	if (!view->popup && view->float_mode != VIDOW_FLOAT_NONE) {
		/* 浮窗离开工作区。 */
		vidow_workspace_track(view->ws, view, false);
	}
	if (!view->popup && view->float_mode == VIDOW_FLOAT_NONE &&
			view->column != NULL) {
		/* 平铺窗口自己 unmap 了，得从列里摘出去 —— 不摘的话那条列一直占着
		 * 一列宽，屏上却什么都没有。proton 游戏就会这样：它的 "Input" 输入法
		 * 辅助窗口先被平铺进一条列，随后 unmap（连接不断、surface 还在）。
		 * 用户报的正是这个：「lutris 和游戏窗口之间凭空出现了一列，什么窗口
		 * 都没有但占据了一列的空间」—— 现场 dump 里 id 59 那个窗口就是
		 * mapped=false、visible=false，可 column 还在、几何 2883,6 951x1068，
		 * 正好夹在 lutris（1926）和游戏（3834）两条列中间。
		 * 它回来时 view_map 会走 view_ensure_column 重新归位。 */
		vidow_view_leave_layout(view);
	}
	if (view->dragging) {
		/* 拖着的时候窗口没了：撤掉 grab，别让指针还挂在一个已经没了的窗口上。 */
		view->dragging = false;
		if (server->grab_view == view) {
			server->grab_mode = VIDOW_GRAB_NONE;
			server->grab_view = NULL;
			server->grab_client_press = false;
			wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
				"left_ptr");
		}
	}
	view_snapshot_close(view);
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	/* 图标态那块快照同样要收：窗口 unmap 了就不该再有它的画面（它是
	 * layer_toplevels 的孩子，scene_tree 关掉管不着它）。重新 map 时客户端一
	 * 交帧，mini_snapshot_place() 会照现状重建。 */
	mini_snapshot_free(view);
	vidow_backdrop_damage_view(view);

	if (server->mini_grab_view == view) {
		/* 小窗形态拖拽中窗口没了（客户端崩 / 被关）：撤掉，别让指针还挂在一个
		 * 已经没了的窗口上（见 input.c 的 mini_pointer_*）。 */
		server->mini_grab_view = NULL;
		server->mini_grab_dragging = false;
	}

	if (server->grab_view == view) {
		vidow_float_end_grab(server);
	}

	if (view->popup) {
		/* 弹出物从来没有成为过 server->focused，只需要把它握着的键盘还回去。 */
		view_popup_release_keyboard(view);
		/* **防御**：早先版本的 view_ensure_column 会给 popup 也建列（见那里的
		 * 评论），这些残留列得趁 unmap 摘掉。popup 正常情况下 column 为 NULL，
		 * vidow_view_leave_column 会立刻返回，无害；真挂着一条就顺手收掉，
		 * 否则它永远是 `windows: []` 但 `wl_list_empty` 为假的僵尸列。 */
		vidow_view_leave_column(view);
		vidow_workspace_cleanup(server);
		vidow_arrange(server);
		return;
	}

	if (server->focused == view) {
		/* 邻居要在摘列之前找（见 focus_after_close）。 */
		struct vidow_view *next = focus_after_close(server, view, output);
		vidow_view_leave_column(view);
		server->focused = NULL;
		if (next != NULL) {
			vidow_focus_view(next);
		} else {
			focus_nearest_column(server, output);
		}
	} else {
		vidow_view_leave_column(view);
	}

	/* 列真的摘掉了，它那块工作区才可能变空 —— 栈中间的空工作区该收掉（niri：
	 * 切走 / 关掉之后那块就没了）。放在 leave_column 之后：之前它还不算空。 */
	vidow_workspace_cleanup(server);
	vidow_arrange(server);
}

/* 客户端这一帧到底动了哪一块（布局逻辑坐标，语义见 server.h）。
 *
 * surface->buffer_damage 是**缓冲像素**坐标：wlroots 在 surface_update_damage()
 * 里已经把客户端的 surface damage + buffer damage、viewport 的 src 偏移、scale、
 * transform 全算进去了（换缓冲 / 换尺寸就是整块）。要变成布局逻辑坐标只差一个
 * 「缓冲像素 / 表面逻辑像素」的倍率和场景节点的位置。
 *
 * 只有这几件事都成立时才敢用小盒子（否则返回 WHOLE，让调用方按整块算）：
 * 没有 transform / viewport 缩放、没有 subsurface（它们的 commit 我们没在听，
 * 拿不到损伤区域）、场景节点没有被改成别的显示尺寸（overview / X11 缩放）、
 * overview 没在跑（那时候节点位置是映射过的，跟布局坐标对不上）。 */
enum vidow_surface_damage vidow_view_surface_damage(struct vidow_view *view,
		struct wlr_box *box) {
	struct wlr_surface *s = vidow_view_surface(view);
	if (s == NULL || view->scene_surface == NULL ||
			view->scene_surface->buffer == NULL) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	const struct wlr_surface_state *st = &s->current;
	if (!pixman_region32_not_empty(&s->buffer_damage)) {
		/* 没报损伤。没换缓冲 = 真的一个像素都没动（只有帧回调 /
		 * ack_configure 的那种 commit），世界代次都不用动；换了新缓冲却
		 * 没报损伤 = 内容到底变没变说不清，按整块算。 */
		return (st->committed & WLR_SURFACE_STATE_BUFFER) != 0 ?
			VIDOW_SURFACE_DAMAGE_WHOLE : VIDOW_SURFACE_DAMAGE_NONE;
	}
	if (st->transform != WL_OUTPUT_TRANSFORM_NORMAL ||
			st->viewport.has_dst || st->viewport.has_src ||
			st->width <= 0 || st->height <= 0 ||
			st->buffer_width <= 0 || st->buffer_height <= 0) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	/* 有子表面就整块算损伤，是老行为，代价极大：Steam / 所有 CEF 窗口的
	 * 主表面下都挂着子表面，于是**每一次 commit** 都被判成整块损伤 ——
	 * 整块重烘离屏、整块重抓模糊，烘完又产生新损伤，自己喂自己。实测量到
	 * 每秒 5 万次 bake（正常是 60～165 次），渲染管线直接饱和，窗口停在
	 * 第一帧的黑图上（用户报的「重开黑屏」）。
	 *
	 * 子表面自己的损伤是记在**它自己**的 state 里，不在父表面的
	 * buffer_damage 里。所以这里只看父表面这一份：它有明确损伤就用它
	 * （子表面的重画由子表面自己的 commit 各自触发），一份都没有才退回
	 * 整块。宁可偶尔多烘一帧，也不能每帧都烘。 */
	if (!wl_list_empty(&st->subsurfaces_below) ||
			!wl_list_empty(&st->subsurfaces_above)) {
		/* 上面已经确认父表面这一帧**有**损伤。只在它盖住整块表面时
		 * 才退回整块 —— 不然就用下面的精细框。 */
		pixman_box32_t *sd = pixman_region32_extents(&s->buffer_damage);
		double ssc = (double)st->buffer_width / (double)st->width;
		if ((double)sd->x1 / ssc <= 0.0 && (double)sd->y1 / ssc <= 0.0 &&
				(double)sd->x2 / ssc >= (double)st->width &&
				(double)sd->y2 / ssc >= (double)st->height) {
			return VIDOW_SURFACE_DAMAGE_WHOLE;
		}
	}
	struct wlr_scene_buffer *sb = view->scene_surface->buffer;
	if (sb->dst_width > 0 &&
			(sb->dst_width != st->width || sb->dst_height != st->height)) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	if (vidow_overview_active(view->server)) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	int nx = 0, ny = 0;
	if (!wlr_scene_node_coords(&sb->node, &nx, &ny)) {
		return VIDOW_SURFACE_DAMAGE_WHOLE;
	}
	pixman_box32_t *d = pixman_region32_extents(&s->buffer_damage);
	double cs = (double)st->buffer_width / (double)st->width;
	int x1 = (int)floor((double)d->x1 / cs);
	int x2 = (int)ceil((double)d->x2 / cs);
	int y1 = (int)floor((double)d->y1 / cs);
	int y2 = (int)ceil((double)d->y2 / cs);
	/* 只算表面里**看得见**的那一块：客户端可以声明窗口几何比表面小（GTK 的
	 * CSD 留白 —— 装饰着色器只采窗口几何那一块，见 vidow_view_fx_geo），
	 * 留白那圈 vidow 根本不画。损伤全落在留白里 = 屏幕上什么都没变。 */
	struct wlr_box g = vidow_view_geometry(view);
	if (g.width <= 0 || g.height <= 0 || g.x < 0 || g.y < 0 ||
			g.x + g.width > st->width || g.y + g.height > st->height) {
		/* 没声明几何 / 声明框落在表面外面（客户端改大小的过渡期）：当整块
		 * 表面都是可见内容，跟 vidow_view_fx_geo 同一套判断。 */
		g = (struct wlr_box){ .x = 0, .y = 0,
			.width = st->width, .height = st->height };
	}
	if (x1 < g.x) {
		x1 = g.x;
	}
	if (y1 < g.y) {
		y1 = g.y;
	}
	if (x2 > g.x + g.width) {
		x2 = g.x + g.width;
	}
	if (y2 > g.y + g.height) {
		y2 = g.y + g.height;
	}
	if (x2 <= x1 || y2 <= y1) {
		return VIDOW_SURFACE_DAMAGE_NONE; /* 只在看不见的留白里动 */
	}
	*box = (struct wlr_box){
		.x = nx + x1, .y = ny + y1,
		.width = x2 - x1, .height = y2 - y1,
	};
	return VIDOW_SURFACE_DAMAGE_BOX;
}

static void view_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, commit);
	struct wlr_surface *surface = vidow_view_surface(view);
	/* debug 每窗口帧率：每次 commit 记一笔（应用自己的渲染节奏）。 */
	vidow_debugrate_view_commit(view);
	/* 有界探针：每 60 次 commit 一行。用来判断「重开之后 XWayland 还在不在
	 * 交帧」—— 交帧停了，bake_revision 就永远不涨，装饰图停在第一张空图上。
	 *
	 * **默认关，VIDOW_COMMIT_TRACE=1 才开。**
	 *
	 * 以前是无条件打的。原神这类满速提交的 X11 游戏跑 160fps，就是每秒
	 * 2.7 行、一天二十多万行往 session log 里写 —— 排查时留着，日常是白烧
	 * IO（日志还要被 tail / 编辑器读）。 */
	if (view->x11 != NULL) {
		if (vidow_commit_trace_on() && view->commit_n % 60 == 0) {
			const char *t = vidow_view_title(view);
			wlr_log(WLR_INFO, "commit-n: %s n=%d surf=%p buf=%dx%d",
				t != NULL ? t : "(无题)", view->commit_n,
				(void *)surface,
				surface != NULL && surface->buffer != NULL ?
					surface->buffer->base.width : -1,
				surface != NULL && surface->buffer != NULL ?
					surface->buffer->base.height : -1);
		}
		view->commit_n++;
	}
	if (!view->mapped || view->focus_helper || view->scene_tree == NULL ||
			(view->x11 != NULL && view->x11->surface != NULL &&
				!view->x11->surface->mapped)) {
		x11_trace_view("commit-anomaly", view, surface);
	}

	/* 表面第一帧到了：map 那一刻可能还没缓冲（尺寸全是 0，判不了归属），
	 * 在这儿补一次。抄 Hyprland considerMap 的思路 —— 见 view_reconsider_map。 */
	view_reconsider_map(view);

	/* 统一的状态机：客户端刚交了这一帧，按「有没有 buffer」对齐一次显示
	 * 状态。这是 Hyprland 的 commit 监听（XSurface.cpp:97）那一步 ——
	 * 有 texture 且没放上屏就放上去，没 texture 却放着就收下来。
	 *
	 * 以前这里是 x11_replay_map_if_xmapped 的一长串前置条件（mapped、
	 * x11_map_replayed、scene_tree、focus_helper 都不能挡），判据和
	 * associate / map_request 两处各不相同，于是出现了「三处都说没问题、
	 * 屏幕上什么都没有」的死角。现在只有一个判据。 */
	xwm_surface_sync(view);

	/* **排帧**。这是 X11 和 Wayland 共用的交帧入口，而 vidow 是按需渲染
	 * （vidow_output_schedule_frame）—— 不在这里要一帧，客户端交上来的新内容
	 * 就要等下一次别的什么原因触发渲染才上屏。
	 *
	 * 用户报的「Electron / Chromium 应用还是卡」就是这个：Wayland 客户端那条路
	 * 由 view_surface_commit 补上了，但 X11（XWayland 上的 vivaldi / discord /
	 * telegram / Element）走的是这个 view_commit，一直没人排帧。
	 * 放在最前面：后面的记账不影响「有新内容」这个事实。 */
	if (view->server != NULL && view->output != NULL &&
			!view->server->locked) {
		/* 临时探针（VIDOW_COMMIT_WHY=1）：谁在每帧交帧。静止桌面 300fps
		 * 时每 150ms 抽一行，看 title 就知道是哪个客户端在空转重绘。 */
		if (getenv("VIDOW_COMMIT_WHY") != NULL) {
			static int64_t cw_last;
			int64_t cw_now = vidow_now_ns();
			if (cw_now - cw_last > 150000000LL) {
				cw_last = cw_now;
				wlr_log(WLR_INFO, "交帧 view=%s out=%s",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					view->output->name);
			}
		}
		vidow_output_schedule_frame(view->server, view->output,
			"客户端交帧");
		/* **X11 表面要额外多推几帧**。
		 *
		 * 原因是一条互相等待：XWayland 的渲染循环由 wl_surface 的
		 * frame callback 驱动，而 frame callback 只在 Vidow 真正画完
		 * 一帧（vidow_output_render → send_frame_done）时才发。Vidow
		 * 又是按需渲染 —— 没有损伤就一帧都不画。
		 *
		 * 于是：XWayland 交了一帧（可能是窗口刚重映射时的空白帧）→
		 * Vidow 画完、发 frame done → 之后没有损伤，Vidow 停手 →
		 * frame done 断流 → XWayland 再也不画 → 画面永久冻结在那一帧
		 * 空图上。实测 Steam 第二次开主窗口就是这样：重开后
		 * x11-probe 只出现 **1 次**（首开 2285 次），纹理 3808x2050
		 * 但内容全 0（首开是 0xff332211）。
		 *
		 * 手动切一下工作区（强制整屏重绘）画面立刻正常 —— 正好证明
		 * 「只要再发一次 frame done，XWayland 就会继续画」。
		 *
		 * 所以这里给 X11 表面多要几帧：每次 commit 再推 3 帧，把
		 * XWayland 从「等回调」里拽出来。Wayland 客户端不需要这个
		 * （它们的渲染节奏由自己的帧循环决定），只对 X11 做。 */
		if (view->x11 != NULL) {
			/* **X11 表面交帧了就排一帧。**
			 *
			 * 这里以前还有一整套「欠帧预算」：每次 commit 记 30 帧债务，
			 * 由补帧循环（vidow_view_render_pump）催着消耗，目的是把
			 * XWayland 从「等 frame callback」里拽出来（Steam 重开黑屏）。
			 *
			 * 2026-10-27 删掉的理由：真正的 frame_done 早就由
			 * vidow_views_send_x11_frame_done() 在**每次渲染收尾无条件**
			 * 发了（output.c，不看可见性、不看工作区、不看在不在屏上），
			 * 欠帧预算唯一的作用退化成「让输出别停」—— 而这件事
			 * schedule_frame 一行就做完了。
			 *
			 * 那套预算的代价却很实在：它让补帧循环绕过「在屏上就不补」
			 * 和迟滞两道闸，每 500ms 无脑续 30 帧，窗口滚出视口时把主循环
			 * 烧穿（实测按键队列延迟累积到 2078ms）。 */
			vidow_output_schedule_frame(view->server, view->output,
				"X11 表面续帧");
		}
	}
	/* X11 弹出物：内容换了（圆角 / 自带阴影的 alpha 可能跟着变），背后那张
	 * 模糊按新 mask 重烘一遍。 */
	if (view->popup && view->popup_blur != NULL) {
		vidow_popup_blur_invalidate(view->popup_blur);
	}

	/* **交帧间隔**（VIDOW_TRACE_FRAMES=1）：vidow 自己 128fps 也不慢的时候，
	 * 卡顿只能在客户端侧 —— 这个数会直接显示「它多久交一帧」。144Hz 应该
	 * ~7ms，60Hz ~16ms；上百毫秒就是客户端被饿着或者自己卡住。 */
	if (view->server->trace_timer != NULL) {
		int64_t now_ns = vidow_now_ns();
		if (view->last_commit_ns != 0 && view->mapped) {
			/* **累计**每 2 秒打一次：交帧次数 / 平均间隔 / 最坏间隔。
			 * 只记 >30ms 的那些算不出帧率（快的被滤掉了），分不清「空闲」和
			 * 「真的卡」。累计器不设阈值，所以能直接算出这个窗口的真实 fps。 */
			int64_t d = now_ns - view->last_commit_ns;
			view->commit_n++;
			view->commit_ns_sum += d;
			if (d > view->commit_ns_max) {
				view->commit_ns_max = d;
			}
			if (view->commit_report_ns == 0) {
				view->commit_report_ns = now_ns;
			} else if (now_ns - view->commit_report_ns >= 2000 * 1000000LL) {
				double span = (double)(now_ns - view->commit_report_ns) / 1e9;
				wlr_log(WLR_DEBUG,
					"交帧统计 %s: %.1f fps（%lld 帧 / %.1fs，均 %.0fms，最坏 %.0fms）",
					vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
					(double)view->commit_n / span, (long long)view->commit_n,
					span, (double)view->commit_ns_sum / 1e6 / view->commit_n,
					(double)view->commit_ns_max / 1e6);
				view->commit_n = 0;
				view->commit_ns_sum = 0;
				view->commit_ns_max = 0;
				view->commit_report_ns = now_ns;
			}
		}
		view->last_commit_ns = now_ns;
	}

	/* 客户端交帧了 —— X11 那边顺手对一下 _NET_WM_STATE（属性那条路不发信号，
	 * 见 view_sync_x11_fullscreen）。 */
	view_sync_x11_fullscreen(view);

	/* 客户端交帧了：它声明的有效源区域（wp_viewport 的 src 框）可能跟着变了，
	 * 抄一份没裁过的那一版给装饰用（裁着的时候这个字段是被 set_clip 改过的，
	 * 见 view_snapshot_content_box）。没裁的时候只是读一下，很便宜。 */
	view_snapshot_content_box(view);

	/* 帧追踪（VIDOW_TRACE_FRAMES=1）：客户端在这里交帧。只记「真画在这块屏上、
	 * 还显示着」的那些 —— 视口外的窗口本来就没人给它出帧，记了就是误报。
	 * 这块屏要是从此不再出帧，就是「客户端在画、屏幕不更新」。 */
	/* 帧追踪：客户端到底有没有在交帧（限速 150ms 一行）。「窗口停在最后一帧」
	 * 这种事，第一刀就切在这里：客户端不交帧（在等帧回调/自己卡住）和交了帧而
	 * 画面没更新，是两个完全不同的问题。 */
	if (view->server->trace_timer != NULL) {
		int64_t now_ns = vidow_now_ns();
		if (now_ns - view->trace_commit_ns > 150 * 1000000LL) {
			view->trace_commit_ns = now_ns;
			struct wlr_surface *s0 = vidow_view_surface(view);
			wlr_log(WLR_DEBUG, "trace: commit %s（缓冲 %dx%d）",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				s0 != NULL ? s0->current.buffer_width : 0,
				s0 != NULL ? s0->current.buffer_height : 0);
		}
	}
	struct vidow_output *trace_out =
		vidow_output_lookup(view->server, view->output);
	if (trace_out != NULL && view_shows(view) &&
			view_on_screen(view->server, trace_out, view)) {
		vidow_output_trace_commit(trace_out);
	}

	/* New pixels: everything that samples this window as a backdrop (a
	 * floating window on top of it) has to be re-blurred. 它自己那张私有底衬
	 * 不受影响（抓的时候它是藏着的），所以这笔账记在它头上。
	 *
	 * 记的是**客户端真动过的那一块**（surface damage），不是整块窗口：一个
	 * 1000x800 的窗口里只有一条 300x20 的进度条在动的时候，压着它的那个窗口
	 * 底下那片模糊只该因为这一条过期。说不清的时候照样整块算（见
	 * vidow_view_surface_damage）。 */
	struct wlr_box dmg;
	switch (vidow_view_surface_damage(view, &dmg)) {
	case VIDOW_SURFACE_DAMAGE_NONE:
		break; /* 一个像素都没变：世界代次都不动 */
	case VIDOW_SURFACE_DAMAGE_BOX:
		vidow_backdrop_damage_view_rect(view, &dmg);
		break;
	case VIDOW_SURFACE_DAMAGE_WHOLE:
		vidow_backdrop_damage_view(view);
		break;
	}

	if (view->kind == VIDOW_VIEW_X11) {
		/* X11：缓冲区的缩放比例要跟着 xwayland_scale 走（尺寸变了就重算），
		 * 其余和 xdg 一样 —— 浮动窗口自己定尺寸，贴图重画，最后锁一份给
		 * 关闭动画。 */
		vidow_view_x11_scale(view);
		view_dst_capture(view);
		if (view->float_mode != VIDOW_FLOAT_NONE) {
			vidow_float_on_commit(view);
		}
		if (view->mapped && (view->fx.active ||
				view->mini != VIDOW_MINI_NONE)) {
			vidow_view_apply_anim(view);
		}
		struct wlr_client_buffer *client =
			vidow_view_surface(view) != NULL ?
			vidow_view_surface(view)->buffer : NULL;
		if (client != NULL && client->source != NULL &&
				client->source != view->last_buffer) {
			wlr_buffer_unlock(view->last_buffer);
			view->last_buffer = wlr_buffer_lock(client->source);
			/* 小窗（S5）：换了最后一帧 → 快照节点（图标 + 预览）跟上。 */
			vidow_mini_frames_sync(view);
		}
		return;
	}

	/* xdg-shell: the initial commit must be answered with a configure. */
	if (view->xdg_toplevel->base->initial_commit) {
		/* 抢跑的状态请求：客户端在第一次 commit *之前* 就 set_fullscreen 的话
		 * （Qt/KDE 恢复全屏窗口会这么开窗），view_request_fullscreen 里发不了
		 * configure，只把请求留在 wlroots 那边。现在 surface initialized 了，
		 * 在 arrange 之前兑现它 —— 晚了 arrange 就按「不是全屏」排过一遍了。 */
		if (view->xdg_toplevel->requested.fullscreen && !view->fullscreen) {
			view_apply_fullscreen(view, true, "客户端开窗时就要全屏");
		}
		vidow_float_update_mode(view);
		if (view->float_mode == VIDOW_FLOAT_NONE) {
			view_ensure_column(view->server, view);
			/* 抢跑的最大化请求同理（Qt/KDE 恢复「上次是最大化」的窗口就是这么
			 * 开窗的）：view_request_maximize 里列还没建出来（列是上一行才建的），
			 * 只能在这儿兑现。放在 arrange 之前 —— 晚了就按「没最大化」排过一遍，
			 * 客户端还会先收到一个 maximized=false 的初始 configure。 */
			if (view->server->config.honor_maximize_request &&
					view->xdg_toplevel->requested.maximized &&
					view->column != NULL) {
				view->maximize_granted = true;
				vidow_column_set_maximized(view->column, true);
			}
			vidow_arrange(view->server);
		} else {
			/* 浮动窗口不进列，arrange 碰不到它，得自己回一个 configure。
			 * 尺寸 0x0 = 让客户端自己定（浮动窗口的常规语义）。
			 *
			 * 这一支不是可有可无的：xdg-foreign 会在客户端第一次 commit
			 * *之前* 就把 parent 设好（Qt/KDE 的 portal 对话框就是这样），
			 * 于是 detect_mode() 在更早的 view_set_parent 里就已经把模式
			 * 定成 FOLLOW 了，到这里 vidow_float_update_mode() 是空操作，
			 * 没人发 configure 客户端就一直等。 */
			wlr_xdg_toplevel_set_size(view->xdg_toplevel, 0, 0);
			view->cfg_w = 0;
			view->cfg_h = 0;
			view->cfg_sent = true;
		}
		view->configured = true;
		return;
	}

	if (view->float_mode != VIDOW_FLOAT_NONE) {
		/* floating windows are sized by the client */
		vidow_float_on_commit(view);
	}

	/* 客户端按几倍渲染的 —— 「清晰度」的客户端那一半。我们通过
	 * wl_surface.preferred_buffer_scale / wp_fractional_scale 告诉它按输出的
	 * 缩放画，这里核对它照做了没有：缓冲区像素数应该等于逻辑尺寸乘缩放。 */
	surface = vidow_view_surface(view);
	if (surface != NULL &&
			view->logged_client_buf_w != surface->current.buffer_width) {
		view->logged_client_buf_w = surface->current.buffer_width;
		/* state 里不留 buffer_scale，用缓冲区像素 / 逻辑尺寸反推。 */
		double client_scale = surface->current.width > 0 ?
			(double)surface->current.buffer_width /
				(double)surface->current.width : 1.0;
		view->logged_client_scale = (int)lround(client_scale);
		wlr_log(WLR_DEBUG, "surface %s: 缓冲 %dx%d 声明 %d 倍 -> 逻辑 %dx%d"
			"（输出 %.2fx，设备像素 %dx%d）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			surface->current.buffer_width, surface->current.buffer_height,
			(int)lround(surface->current.width > 0 ?
				(double)surface->current.buffer_width /
					(double)surface->current.width : 1.0),
			surface->current.width, surface->current.height,
			(double)(view->output != NULL ? view->output->scale : 1.0),
			(int)lround(surface->current.width *
				(view->output != NULL ? (double)view->output->scale : 1.0)),
			(int)lround(surface->current.height *
				(view->output != NULL ? (double)view->output->scale : 1.0)));
	}

	/* The visible pixels come from our offscreen decoration buffer now, so a
	 * client repaint has to be pushed through it again. */
	/* 小窗（S3）也走这一趟，哪怕 fx 不 active：wlroots 在每次 commit 时会把场景
	 * 节点的显示尺寸重置成表面自己的尺寸（types/scene/surface.c 的
	 * surface_reconfigure），而我们的 commit 监听器排在它后面 —— 不在这一趟里
	 * 重设一次小窗那条缩放，客户端每交一帧节点就弹回原生尺寸（画面忽大忽小、
	 * 点击范围也跟着错）。装饰开着时本来就会走到这儿，这条只多覆盖「装饰全关 +
	 * 小窗」那一种。 */
	if (view->mapped && (view->fx.active || view->mini != VIDOW_MINI_NONE)) {
		vidow_view_apply_anim(view);
	}

	/* **fractional-scale 客户端的尺寸**（Chromium / Electron 全走这条路）。
	 *
	 * 它们故意 set_buffer_scale(1)，但 buffer 按 2x 画（实测 vivaldi 在 DP-2
	 * 上：buffer 3808x2050、surface 1904x1025、scale 1）—— 协议的本意是让合成器
	 * 用 wp_viewporter 缩回去。而 wlroots 的 surface_reconfigure 拿
	 * buffer_width / buffer_scale 当节点尺寸，于是 3808 直接铺在 1904 的位置上：
	 * 画面变成 1/4，配合后面的重排又弹回 1 倍 —— 用户看到的「1/4 和 1 不停变化」，
	 * 也是「Electron 卡」的直接原因（每帧都在重排重画）。
	 *
	 * 修：每次 commit 后把客户端节点的显示尺寸钉回逻辑尺寸（surface->width /
	 * height 已经是逻辑值）。fx / 小窗那条路自己会设，不打架。 */
	if (view->scene_surface != NULL && surface != NULL &&
			surface->current.width > 0 && surface->current.height > 0) {
		wlr_scene_buffer_set_dest_size(view->scene_surface->buffer,
			surface->current.width, surface->current.height);
	}

	/* Keep a locked reference to the last frame: the close animation needs
	 * pixels, and by the time unmap fires the surface has none.
	 *
	 * Lock the client's own buffer, not the wlr_client_buffer wrapper: the
	 * wrapper goes away as soon as the surface drops it, while a lock on the
	 * source keeps the pixels alive until the animation is done. */
	struct wlr_client_buffer *client =
		view->xdg_toplevel->base->surface->buffer;
	if (client != NULL && client->source != NULL &&
			client->source != view->last_buffer) {
		wlr_buffer_unlock(view->last_buffer);
		view->last_buffer = wlr_buffer_lock(client->source);
		/* 小窗（S5）：换了最后一帧 → 快照节点（图标 + 预览）跟上。 */
		vidow_mini_frames_sync(view);
	}
}

static void view_set_parent(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, set_parent);
	vidow_float_update_mode(view);
	/* xdg-foreign 导入父窗口时走的就是这条路（wlroots 替客户端调用
	 * wlr_xdg_toplevel_set_parent），窗口列表那边也要跟上。 */
	vidow_portal_view_parent(view);
}

/* 销毁一个 view 的公共体（Wayland 的 destroy 信号、X11 的窗口永久销毁都走这里）。
 *
 * 只做「摘账 + 释放」，不读 listener —— 调用方保证 view 已经不再被任何客户端
 * 信号引用（X11 那边 vidow_view_x11_gone 已把挂在 wl_surface 上的 map/unmap/
 * commit 摘干净）。view 里的 .link 全部是 wl_list_init 成自环的（见 X11 创建
 * 处），所以下面逐个 wl_list_remove 重复调用也安全。 */
static void view_destroy_common(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	struct wlr_output *output = view->output;

	/* debug 每窗口帧率叠层是独立挂在 layer_overlay 上的，不随 scene_tree 走，
	 * 得手动收掉，否则窗口没了还留个数字在屏上。 */
	if (view->dbg_node != NULL) {
		wlr_scene_node_destroy(&view->dbg_node->node);
		view->dbg_node = NULL;
	}

	/* 悬停记录会变成悬空指针。 */
	if (server->hovered_view == view) {
		server->hovered_view = NULL;
	}
	if (server->focus_before_layer == view) {
		server->focus_before_layer = NULL;
	}

	/* 事件要带标题，而标题是客户端的资源、销毁信号来的时候已经收走了 ——
	 * 用 map / 改标题时记下的快照（见 vidow_view_note_names）。 */
	vidow_ipc_emit_window_closed(server, view->id,
		view->last_title[0] != '\0' ? view->last_title :
			vidow_view_title(view),
		view->last_app_id[0] != '\0' ? view->last_app_id :
			vidow_view_app_id(view));

	wl_list_remove(&view->map.link);
	wl_list_remove(&view->unmap.link);
	wl_list_remove(&view->commit.link);
	wl_list_remove(&view->set_title.link);
	wl_list_remove(&view->set_parent.link);
	wl_list_remove(&view->request_fullscreen.link);
	wl_list_remove(&view->request_maximize.link);
	wl_list_remove(&view->request_minimize.link);
	wl_list_remove(&view->surface_commit.link);
	wl_list_remove(&view->request_move.link);
	wl_list_remove(&view->request_resize.link);
	wl_list_remove(&view->destroy.link);
	view_capture_holds_clear(view);
	vidow_portal_view_free(view);
	vidow_mru_forget(server, view);
	/* 动画回调的 data 就是这个 view（view_move_update / view_move_done）：
	 * 窗口在动画没跑完时没了，必须先把动画取消掉，否则下一次 anim tick
	 * 会拿着已释放的 view 调 vidow_view_apply_anim() —— 那就是 2026-10-12
	 * 23:20–23:47 那 5 个 SIGSEGV 核心转储（栈：#0 野地址 <- #1
	 * vidow_output_render <- output_frame <- 帧回调）。 */
	if (view->anim_move != NULL) {
		vidow_anim_cancel(view->anim_move);
		view->anim_move = NULL;
	}
	if (view->anim_open != NULL) {
		vidow_anim_cancel(view->anim_open);
		view->anim_open = NULL;
	}
	wl_list_remove(&view->link);
	/* 果冻那个场景节点比 view 活得久，结构里又攥着 view 指针：先收掉。 */
	vidow_jelly_view_gone(view);

	/* The scene node is owned by wlroots and may outlive the view; clear the
	 * tag so hit-testing cannot reach the freed struct. */
	if (view->scene_tree != NULL) {
		view->scene_tree->node.data = NULL;
	}
	if (server->mini_grab_view == view) {
		/* 小窗形态拖拽中窗口被销毁了（客户端崩 / 直接被关，没走 unmap）：
		 * 和 view_unmap 里那处一样，撤掉指针上的挂账。 */
		server->mini_grab_view = NULL;
		server->mini_grab_dragging = false;
	}
	if (server->grab_view == view) {
		vidow_float_end_grab(server);
	}
	vidow_float_detach_children(server, view);
	/* 图标态那块快照挂在 layer_toplevels 上，不跟着 scene_tree 走：窗口没了
	 * 就得自己收（不收 = 屏上冻着一块「窗口已经没了」的鬼影）。 */
	mini_snapshot_free(view);
	wlr_buffer_unlock(view->last_buffer);
	view->last_buffer = NULL;
	vidow_fx_release(&view->fx);
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	vidow_anim_cancel(view->anim_open);
	view->anim_open = NULL;
	/* 小窗形态转换的补间（S5）data 也是这个 view。 */
	vidow_anim_cancel(view->mini_anim);
	view->mini_anim = NULL;
	/* 悬停预览（S5）挂在 server 上、指着的却是这个 view：先收掉，别留一个
	 * 攥着已释放窗口的镜面节点。 */
	if (server->mini_preview_view == view) {
		vidow_mini_preview_hide(server);
	}

	/* **先撤掉别人对我的挂账**：辅助窗口的「借出方」指向的本 view 可能马上
	 * 就被 free() 掉，留着就是一个悬空指针（unmap 时读它就崩）。 */
	if (server->focus_borrowed_from == view) {
		server->focus_borrowed_from = NULL;
	}
	if (view->focus_helper) {
		/* 辅助窗口销毁：键盘还给**它借走时那个窗口**（它没进过焦点链，
		 * 读 server->focused 会跳到「下一个」窗口 —— 见 server.h 里
		 * focus_borrowed_from 的说明）。借出方已经没了就退回 server->focused。 */
		view->focus_helper = false;
		struct vidow_view *owner = server->focus_borrowed_from;
		server->focus_borrowed_from = NULL;
		if (owner != NULL && (!owner->mapped ||
				vidow_view_surface(owner) == NULL)) {
			owner = server->focused;
		} else if (owner == NULL) {
			owner = server->focused;
		}
		if (owner == view) {
			owner = NULL; /* 自己：别往正在销毁的窗口送焦点 */
		}
		struct wlr_surface *back = owner != NULL ?
			vidow_view_surface(owner) : NULL;
		vidow_seat_focus_surface(server->seat, back);
		vidow_im_focus(server, back);
	}
	bool was_focused = server->focused == view;
	/* 邻居要在摘列（和 free(view)）之前找（见 focus_after_close）。 */
	struct vidow_view *next = was_focused ?
		focus_after_close(server, view, output) : NULL;
	if (was_focused) {
		server->focused = NULL;
	}
	vidow_view_leave_column(view);
	/* 浮窗直接 destroy（没走 unmap）时也要把 ws->floats 的账摘掉。 */
	if (!view->popup && view->float_mode != VIDOW_FLOAT_NONE) {
		vidow_workspace_track(view->ws, view, false);
	}
	vidow_workspace_cleanup(server);

	free(view);

	if (was_focused) {
		if (next != NULL) {
			vidow_focus_view(next);
		} else {
			focus_nearest_column(server, output);
		}
	} else {
		vidow_arrange(server);
	}
}

/* Wayland 的 destroy 信号入口。 */
static void view_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, destroy);
	view_destroy_common(view);
}

/* X11 窗口**永久销毁**（DestroyNotify）时由 xwm 调用。
 *
 * X11 的 view 没有 wlroots destroy 信号可挂（窗口对象是 Vidow 自己的），所以
 * 以前它永远不会被摘出 server->views、也从没 free —— 用户翻到的「state 里
 * windows 快八百个、实际只有 7 个映射」就是这堆僵尸 view。X 窗口先 unmap
 * 再重映射（同一 X id）走的是另一个入口（vidow_view_x11_gone，不销毁 view），
 * 只有 DestroyNotify 才代表这个窗口不会回来了，才走这里。
 *
 * 调用前 xwm 已经做过 vidow_view_x11_gone（摘表面、摘列、清 fx），这里只负责
 * 把 view 结构本身摘链释放。 */
void vidow_view_x11_destroy(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	/* 防御：还在被别处引用（录屏源 / 借焦点）就别硬释放 —— 那些路径自己
	 * 会调 vidow_view_x11_gone 再回到这里。这里只处理「已经没表面」的。 */
	view_destroy_common(view);
}

static void view_set_title(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, set_title);
	/* DEBUG：浏览器每换一个标签页、终端每 cd 一次都会来一行，日常日志里
	 * 不需要。规则重判 / IPC 广播照旧（它们只在该动的窗口上动）。 */
	wlr_log(WLR_DEBUG, "title %s", vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(untitled)");
	vidow_portal_view_title(view);
	/* 规则可能按标题匹配：标题变了要重新判一遍。 */
	vidow_rule_apply(view->server, view);
	vidow_view_note_names(view);
	vidow_ipc_emit_window(view->server, "window-title-changed", view);
}

static void view_request_fullscreen(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_fullscreen);
	bool want = view->xdg_toplevel->requested.fullscreen;

	if (!vidow_view_configure_ready(view)) {
		/* 第一次 commit 之前来的全屏请求：这时候既发不出 configure，也不能改
		 * 状态 —— 改了就是「布局当它全屏、客户端以为自己不是全屏」。等它的初始
		 * commit（surface initialized 之后）再兑现，见 view_commit。 */
		return;
	}

	view_apply_fullscreen(view, want, "客户端请求（xdg_toplevel.set_fullscreen）");
}

static void view_request_maximize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_maximize);

	/* 客户端可能在**第一次 commit 之前**就 set_maximized（Qt/KDE 恢复「上次是
	 * 最大化」的窗口就是这么开窗的 —— kate 一开就把合成器打死）。wlroots 收到
	 * 请求立刻发这个信号，而那时候 surface 还没 initialized：
	 * wlr_xdg_toplevel_set_maximized() 内部的 wlr_xdg_surface_schedule_configure()
	 * 第一句就是 assert(surface->initialized)，直接 abort
	 * （wlroots 0.20.2：types/xdg_shell/wlr_xdg_surface.c:168）。
	 * 所以还没 initialized 就先不碰它：等它的初始 commit（surface initialized
	 * 之后），在 view_commit 的 initial_commit 分支里兑现这个请求。 */
	if (!vidow_view_configure_ready(view)) {
		return;
	}

	/* 客户端自己发起的请求：协议上得有应答，所以把「上次发出去的是什么」这笔账
	 * 作废一次 —— 即使状态碰巧没变也要真发一个 configure 回去（去重那条路是给
	 * 布局自己变化用的，见 server.h 的 cfg_maximized_sent）。 */
	view->cfg_maximized_sent = false;

	bool want = view->xdg_toplevel->requested.maximized;
	struct vidow_column *col = view->column;

	/* 浮窗（对话框、菜单那种没有列的窗口）：照旧回一个「不最大化」——
	 * xdg-shell 要求每个状态请求都有应答，不回客户端就卡在等待里。
	 * 浮窗的尺寸是它自己定的，vidow 不替它撑满工作区。 */
	if (col == NULL) {
		view->maximize_granted = false;
		vidow_view_set_maximized(view, false);
		return;
	}

	if (want && !view->server->config.honor_maximize_request) {
		/* 配置里说不响应（默认）：协议要求每个状态请求都有应答，所以回一个
		 * 「没最大化」，但不动列宽 —— Qt/KDE/Electron 恢复「上次是最大化」
		 * 不会再让窗口一开就占满整列。 */
		view->maximize_granted = false;
		vidow_view_set_maximized(view, false);
		return;
	}
	if (want) {
		/* vidow 没有「窗口级」的最大化（列是布局的最小单位）：客户端要最大化
		 * 就把它那条列最大化，并且记下「这次是它要的」。 */
		view->maximize_granted = true;
		vidow_column_set_maximized(col, true);
		/* 列本来就是最大化的时候上面那条是空操作，应答还得自己补。 */
		vidow_view_set_maximized(view, true);
		return;
	}

	if (view->maximize_granted) {
		/* 它自己撤销：这次最大化本来就是它要的，理。 */
		view->maximize_granted = false;
		vidow_column_set_maximized(col, false);
		vidow_view_set_maximized(view, false);
		return;
	}

	/* 列是**用户**最大化的（Mod+Shift+F），客户端这时候随手发一个
	 * set_maximized(false) 不该把用户的布局撤掉 —— 照当前实际状态回话：
	 * 列最大化的照样答「最大化」，客户端和布局不会各说各话。 */
	vidow_view_set_maximized(view, col->maximized);
}

/* xdg_toplevel.set_minimized：客户端自己要求最小化（xdg-shell v6 起有，wlroots 只把
 * 请求转成信号，认不认由合成器定）。vidow 认 —— 和 IPC / 键位走同一条路；应答的那次
 * configure 由 vidow_view_set_minimized() 里发（协议要求状态请求必须有应答）。 */
static void view_request_minimize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_minimize);

	vidow_view_set_minimized(view, true);
}

/* 客户端交帧了 → 排一帧。
 *
 * vidow 是**按需渲染**：客户端自己 damage 场景之后没人踢这一脚，帧回调就只能
 * 等别的事件（光标移动 / 动画 / 补帧闹钟）碰巧到来时才发 —— 表现就是「应用
 * 内部像 60 帧，桌面本身却是 160」。这一条让「客户端想画」直接变成「排一帧」。 */
static void view_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, surface_commit);
	if (view->server == NULL || view->output == NULL || view->server->locked) {
		return;
	}
	/* 帧率计数只在 view_commit 里做（Wayland 这个 listener 和 view_commit 都挂在
	 * 同一个主 surface commit 上，两边都数会翻倍）。这里只管补帧。 */
	vidow_output_schedule_frame(view->server, view->output, "客户端交帧");
}

static void view_request_move(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_move);
	struct vidow_server *server = view->server;

	/* A CSD titlebar drag: detach a tiled window into a float and grab it.
	 * 这条路上按下是转给客户端的（它才发的 xdg_toplevel.move），from_client
	 * 让松手也转回去（见 server.h 里 grab_client_press 的说明）。 */
	if (view->float_mode == VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	/* 拖标题栏 = 平移视图（用户点名的那条）；想移动浮窗用 Mod+左键。 */
	vidow_pan_begin(server, view);
}

static void view_request_resize(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_view *view = wl_container_of(listener, view, request_resize);
	struct vidow_server *server = view->server;

	if (view->float_mode == VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	vidow_float_begin_grab(server, view, true, true);
}

/* ------------------------------------------------------- X11（XWayland） */


/* X11 的窗口类型决定它是平铺还是浮动、要不要装饰。跟 xdg 那边的
 * detect_mode() 是一个意思，只是信息源不一样。 */
static const int x11_float_types[] = {
	VIDOW_X11_WT_DIALOG,
	VIDOW_X11_WT_UTILITY,
	VIDOW_X11_WT_SPLASH,
	VIDOW_X11_WT_TOOLBAR,
	VIDOW_X11_WT_MENU,
	VIDOW_X11_WT_DROPDOWN_MENU,
	VIDOW_X11_WT_POPUP_MENU,
	VIDOW_X11_WT_TOOLTIP,
	VIDOW_X11_WT_NOTIFICATION,
	VIDOW_X11_WT_COMBO,
	VIDOW_X11_WT_DND,
	VIDOW_X11_WT_DOCK,
};

/* 这些窗口是「弹出物」：自己定位置尺寸，我们不该给它套边框。 */
static const int x11_overlay_types[] = {
	VIDOW_X11_WT_MENU,
	VIDOW_X11_WT_DROPDOWN_MENU,
	VIDOW_X11_WT_POPUP_MENU,
	VIDOW_X11_WT_TOOLTIP,
	VIDOW_X11_WT_COMBO,
	VIDOW_X11_WT_DND,
	VIDOW_X11_WT_NOTIFICATION,
};

static bool x11_has_any_type(const struct vidow_x11_surface *x,
		const int *types, size_t count) {
	for (size_t i = 0; i < count; i++) {
		if (vidow_x11_surface_has_window_type(x, types[i])) {
			return true;
		}
	}
	return false;
}

static bool x11_has_fixed_size(const struct vidow_x11_surface *x) {
	const xcb_size_hints_t *hints = x->size_hints;
	if (hints == NULL) {
		return false;
	}
	bool min_set = (hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) != 0;
	bool max_set = (hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) != 0;
	return min_set && max_set && hints->min_width == hints->max_width &&
		hints->min_height == hints->max_height;
}

/* X11 客户端声明的宽高比（WM_NORMAL_HINTS 的 PAspect）。num/den 就是 w/h。
 *
 * min_aspect 与 max_aspect 都设了、且相等，才算「一个确定的比值」—— 只设一边
 * 是「不小于 / 不大于」的区间，那不是锁定（ICCCM 的语义）。**vidow 以前完全没
 * 读这个字段**，S3b 才加上（见计划 §11.3）。
 *
 * Wayland 协议里没有对应物（xdg-shell 只有 min/max 尺寸，没有比例提示），所以
 * 非 X11 恒返回 false —— 那边的 auto 等同 free。 */
bool vidow_view_aspect_hint(const struct vidow_view *view, int *num, int *den) {
	if (view == NULL || view->kind != VIDOW_VIEW_X11 || view->x11 == NULL) {
		return false;
	}
	const xcb_size_hints_t *hints = view->x11->size_hints;
	if (hints == NULL ||
			(hints->flags & XCB_ICCCM_SIZE_HINT_P_ASPECT) == 0) {
		return false;
	}
	if (hints->min_aspect_num <= 0 || hints->min_aspect_den <= 0 ||
			hints->max_aspect_num <= 0 || hints->max_aspect_den <= 0) {
		return false;
	}
	/* 交叉相乘比：整数除法会把 16/9 和 16/10 判成同一个。 */
	if ((long)hints->min_aspect_num * hints->max_aspect_den !=
			(long)hints->max_aspect_num * hints->min_aspect_den) {
		return false;
	}
	*num = hints->min_aspect_num;
	*den = hints->min_aspect_den;
	return true;
}

/* 客户端声明的固定尺寸，逻辑单位。 */
bool vidow_view_fixed_size_hint(const struct vidow_view *view, int *w, int *h) {
	if (view->kind == VIDOW_VIEW_X11) {
		const xcb_size_hints_t *hints =
			view->x11 != NULL ? view->x11->size_hints : NULL;
		if (hints == NULL ||
				(hints->flags & XCB_ICCCM_SIZE_HINT_P_MIN_SIZE) == 0 ||
				(hints->flags & XCB_ICCCM_SIZE_HINT_P_MAX_SIZE) == 0 ||
				hints->min_width != hints->max_width ||
				hints->min_height != hints->max_height ||
				hints->min_width < 2 || hints->min_height < 2) {
			return false;
		}
		double nw = hints->min_width, nh = hints->min_height;
		vidow_xwm_from_native_size(view->server, view->output, nw, nh, &nw, &nh);
		*w = (int)lround(nw);
		*h = (int)lround(nh);
		return true;
	}
	if (view->xdg_toplevel == NULL) {
		return false;
	}
	const struct wlr_xdg_toplevel_state *st = &view->xdg_toplevel->current;
	if (st->min_width < 2 || st->min_height < 2 ||
			st->min_width != st->max_width ||
			st->min_height != st->max_height) {
		return false;
	}
	*w = st->min_width;
	*h = st->min_height;
	return true;
}

enum vidow_float_mode vidow_x11_detect_mode(struct vidow_view *view) {
	struct vidow_x11_surface *x = view->x11;
	if (x == NULL) {
		return VIDOW_FLOAT_NONE;
	}
	/* 弹出物排在最前：它自带绝对位置，当成「跟着父窗口偏移」会算错，而且它
	 * 的位置由客户端的 X 坐标说了算，压根不参与我们的布局推算。 */
	if (x11_is_popup(x)) {
		return VIDOW_FLOAT_FIXED;
	}
	/* 有父窗口就跟着父窗口（对话框）。 */
	if (x->parent != NULL) {
		return VIDOW_FLOAT_FOLLOW;
	}
	if (x->modal ||
			x11_has_any_type(x, x11_float_types,
				sizeof(x11_float_types) / sizeof(x11_float_types[0])) ||
			x11_has_fixed_size(x)) {
		return VIDOW_FLOAT_FIXED;
	}
	return VIDOW_FLOAT_NONE;
}

/* 这个 X11 窗口是不是「弹出物」——菜单 / 下拉框 / 提示气泡 / 通知。
 *
 * 两类来源：
 *   1. override-redirect：X11 的「别管我」标记，菜单和 tooltip 的基本做法；
 *   2. `_NET_WM_WINDOW_TYPE` 里那几种弹出类型（有些客户端不设 OR，只声明类型）。
 *
 * 它们在 Wayland 那边对应 xdg_popup，vidow 对 xdg_popup 做的事是：挂在父窗口的
 * 场景树上、不装饰、不跑 shader、不做动画、位置尺寸听客户端的 —— 这里一一对齐。
 *
 * 判据不看 `_MOTIF_WM_HINTS`：Wayland 没有 xdg-decoration，客户端没法要求「别
 * 装饰我」，vidow 一律给 xdg_toplevel 套装饰，X11 的普通窗口也跟它一样。
 * （`_MOTIF_WM_HINTS` 还有个坑：XWM 是在窗口刚建出来时读它的，客户端往往之后才
 * 设，读到的永远是初始值。） */
static bool x11_is_popup(const struct vidow_x11_surface *x) {
	if (x->override_redirect) {
		return true;
	}
	return x11_has_any_type(x, x11_overlay_types,
		sizeof(x11_overlay_types) / sizeof(x11_overlay_types[0]));
}

/* 从 X 报的位置找回它在哪块屏上（菜单是绝对定位的）。 */
void x11_track_output(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	if (view->x11 == NULL) {
		return;
	}
	struct wlr_output *hit = vidow_xwm_output_at_native(server,
		view->x11->x, view->x11->y);
	if (hit != NULL) {
		view->output = hit;
	} else if (view->output == NULL) {
		view->output = view_default_output(server);
	}
}

/* Client-placed X popups use the same virtual-output inverse transform
 * as normal windows. Do not clamp them to the usable area. */
bool vidow_view_x11_popup_box(struct vidow_view *view, struct wlr_box *box) {
	if (!view->popup || view->x11 == NULL) {
		return false;
	}
	if (view->x11->width <= 0 || view->x11->height <= 0) {
		return false; /* 还没定尺寸，等它自己说 */
	}
	/* Xwayland 给的 x11->x/y 是**全局 X 绝对坐标**（那个 X 屏幕横跨所有屏，
	 * 每块屏各占一段设备像素宽）。折回**场景布局坐标**要三步：
	 *   ① 减掉这块屏在 X 坐标系里的起点 dev_x；
	 *   ② 除以**这块屏自己的** scale（不是全局 xwayland_scale）；
	 *   ③ 加上这块屏在布局里的原点（右屏布局从 1920 起，不加就全挤到左屏）。
	 *
	 * 之前是「全局 X 坐标直接除以 xwayland_scale」，缺 ①③、②还用错：
	 * 单屏 / 全同倍率下碰巧对，混合 DPI 就错 —— 1x 屏上的弹窗被多除了 2
	 * 且没加布局原点，2x 屏按比例正好抵消。 */
	x11_track_output(view);
	double x = view->x11->x, y = view->x11->y;
	double w = view->x11->width, h = view->x11->height;
	vidow_xwm_from_native_point(view->server, view->output, x, y, &x, &y);
	vidow_xwm_from_native_size(view->server, view->output, w, h, &w, &h);
	/* 弹窗也要吃规则里的 x11_scale，否则场景 / 布局按规则放大、弹窗还是
	 * 按输出缩小，菜单和它的子菜单会一大一小对不上。 */
	x11_apply_rule_scale(view, &w, &h);
	box->x = (int)floor(x);
	box->y = (int)floor(y);
	box->width = (int)lround(w);
	box->height = (int)lround(h);
	if (box->width < 1) {
		box->width = 1;
	}
	if (box->height < 1) {
		box->height = 1;
	}
	return true;
}

/* 视图在「弹出物」和「普通窗口」之间改判时，装饰/动画状态得整份换掉。
 *
 * 关键在装饰开着的时候客户端表面是被隐藏的（内容由装饰着色器连同边框、圆角、
 * 阴影、模糊一起画进离屏缓冲），所以收掉装饰必须把表面的透明度还回去 ——
 * 不然弹出物会变成一块什么都没有的透明块；反过来变回普通窗口则要重新接上装饰。
 * 两个方向都由 vidow_view_apply_anim() 收尾。 */
static void view_swap_decor(struct vidow_view *view) {
	vidow_anim_cancel(view->anim_open);
	view->anim_open = NULL;
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	view->anim_alpha = 1.0;
	view->anim_off_x = 0.0;
	view->anim_off_y = 0.0;
	view->anim_scale = 1.0;
	vidow_fx_end(view->server, &view->fx);
	if (view->scene_surface != NULL && view->scene_surface->buffer != NULL) {
		wlr_scene_buffer_set_opacity(view->scene_surface->buffer, 1.0f);
	}
	if (!view->popup) {
		/* 变回普通窗口：把装饰接回来（shader 名传 NULL = 只要装饰，不要动画） */
		vidow_fx_begin(view->server, &view->fx, view->scene_tree, NULL);
	}
	vidow_view_apply_anim(view);
}

/* 「它是不是弹出物」的答案会变：X11 的属性是分成好几条消息到的 —— 窗口刚建出来
 * 时 `_NET_WM_WINDOW_TYPE` 往往还没设（客户端先 create 再 set property），
 * override-redirect 也可能后改。所以每条相关属性到来都重算一次；变了就把场景节点
 * 搬到对应的层上（弹出物要在所有托管窗口之上）、换掉装饰状态，并让布局重判一次。 */
static void x11_refresh_popup(struct vidow_view *view) {
	bool popup = view->x11 != NULL && x11_is_popup(view->x11);
	if (popup == view->popup) {
		return;
	}
	view->popup = popup;
	/* 改判成弹出物：从 foreign-toplevel 列表里摘掉。反方向（本来误判成
	 * 弹出物、后来发现是普通窗口）要补注册 —— 注册现在只在 map 时发生，
	 * 那一刻的判据可能还是错的。 */
	if (popup) {
		vidow_portal_view_drop_toplevel(view);
	}
	view_takeover_surface_scale(view);
	if (view->scene_tree != NULL) {
		/* 改判成弹出物 / 变回普通窗口：背后的模糊跟着换。场景树留着（只换
		 * 父节点），所以模糊节点得我们自己销毁。 */
		if (popup && view->popup_blur == NULL && view->x11 != NULL &&
				view->x11->surface != NULL) {
			view->popup_blur = vidow_popup_blur_register(view->server,
				view->scene_tree, &view->scene_tree->node,
				view->x11->surface, view, "x11", NULL);
		} else if (!popup && view->popup_blur != NULL) {
			vidow_popup_blur_unregister(view->popup_blur, true);
			view->popup_blur = NULL;
		}
		wlr_scene_node_reparent(&view->scene_tree->node, popup ?
			view->server->layer_popups : view->server->layer_toplevels);
		view_swap_decor(view);
		/* 搬回 layer_toplevels 的节点落在那一层的**最上面**：它要是个平铺窗口，
		 * 这会儿就压在浮窗头上了 —— 层级收口一次（见 vidow_restack）。 */
		if (!popup) {
			vidow_restack(view->server, NULL);
		}
	}
	wlr_log(WLR_INFO, "x11: %s 归类为%s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		popup ? "弹出物（不装饰、不进布局、位置听客户端的）" : "普通窗口");
}

/* One bounded lifecycle trace for X11 views.  It records state transitions and
 * anomalous commits, not every frame, so one reopen gives a complete chain
 * without turning the session log into a frame dump. */
static void x11_trace_view(const char *event, struct vidow_view *view,
		struct wlr_surface *surface) {
	if (view == NULL) {
		return;
	}
	bool xmapped = view->x11 != NULL && view->x11->surface != NULL &&
		view->x11->surface->mapped;
	wlr_log(WLR_DEBUG,
		"x11-life %s xid=0x%x xs=%p surf=%p xmapped=%d mapped=%d helper=%d "
		"tree=%p scene=%p ws=%p col=%p",
		event,
		view->x11 != NULL ? view->x11->window_id : 0,
		view->x11,
		surface != NULL ? surface :
			(view->x11 != NULL ? view->x11->surface : NULL),
		xmapped, view->mapped, view->focus_helper,
		view->scene_tree, view->scene_surface, view->ws, view->column);
}

/* 直接问 X 服务器：这个窗口现在是 viewable 吗？
 *
 * 不看 wlroots 那边的 surface->mapped —— 它要等一次 commit / 属性流才更新，
 * 而「map 请求先于关联到达」的那种时序里，我们需要的是 X 侧的**真实**状态。
 * 查不到（连接没起来 / 窗口没了）一律当不是，让调用方保持原行为。 */
/* Replay the X11 map edge when wlroots mapped the surface without a new
 * commit. This is event-driven and idempotent per association. */
static void x11_drop_surface(struct vidow_view *view);

static void x11_attach_surface(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	if (view->x11 == NULL || view->x11->surface == NULL) {
		return;
	}
	/* **场景树已经按另一块表面建好了 → 先拆掉再建。**
	 *
	 * 这里以前只看 `scene_tree != NULL` 就返回，把两种完全不同的情况当成
	 * 一种处理：
	 *
	 *   ① 同一块表面重复 attach（map 重放之类）—— 该 skip，什么都不用做。
	 *   ② **换了一块新表面**（Steam 关掉主窗口、再从托盘打开）—— 必须重建。
	 *
	 * ②被当成①之后，新表面没有 commit 监听、场景表面还指着旧的（已销毁的）
	 * 那块，于是新内容永远进不来：屏幕上停在旧的那张空图上，全黑。实测重开
	 * 之后 commit-n 卡在 8460 一动不动，源缓冲 dump 出来 1 色（首开 5061 色）。
	 *
	 * 判据用 serial 而不是指针：新表面会分在旧表面释放的那块地址上，指针
	 * 一模一样（见 x11surface.h 里 scene_serial 的说明）。 */
	if (view->scene_tree != NULL) {
		if (view->x11->scene_serial == view->x11->serial) {
			x11_trace_view("attach-skip", view, NULL);
			return;
		}
		x11_trace_view("attach-rebuild", view, view->x11->surface);
		{
			const char *t = vidow_view_title(view);
			wlr_log(WLR_INFO,
				"attach-rebuild: %s serial=%llu→%llu surf=%p",
				t != NULL ? t : "(无题)",
				(unsigned long long)view->x11->scene_serial,
				(unsigned long long)view->x11->serial,
				(void *)view->x11->surface);
		}
		/* 幂等：把旧场景树、commit/map/unmap 监听、FX 状态一起收干净。 */
		x11_drop_surface(view);
	}
	x11_trace_view("attach", view, view->x11->surface);

	/* 弹出物挂在自己的层（见 x11_refresh_popup）：X11 里 override-redirect
	 * 窗口永远在所有托管窗口之上。 */
	view->popup = x11_is_popup(view->x11);
	view->scene_tree = wlr_scene_tree_create(view->popup ?
		server->layer_popups : server->layer_toplevels);
	if (view->scene_tree == NULL) {
		return;
	}
	view->scene_tree->node.data = &view->tag;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);

	view->scene_surface = wlr_scene_surface_create(view->scene_tree,
		view->x11->surface);
	view_takeover_surface_scale(view);
	if (view->scene_surface == NULL) {
		wlr_log(WLR_ERROR, "x11: 建不出场景表面");
		return;
	}
	/* **记下这棵树是按哪一块表面建的 —— 这是上面「换没换表面」判据的唯一
	 * 依据，以前漏了这一步。**
	 *
	 * x11surface.h 把 scene_serial 定义成「0 = 还没建过场景」，上面那条判据
	 * 写的是 `scene_serial == serial → skip`。但全仓库从来没有给它赋过值，
	 * 它恒为 0：
	 *
	 *   - serial 也恰好是 0（还没收到 WL_SURFACE_SERIAL）时，判据成立，
	 *     同一块表面的重复 attach 被跳过 —— 这是想要的行为，但纯属巧合；
	 *   - serial != 0 时判据永远不成立，**每次都当「换了新表面」拆掉重建**，
	 *     包括 map 重放那种本该 skip 的重复 attach。
	 *
	 * 后者才是要修的：反复拆建场景树会让 fx 状态、commit 监听、表面缩放
	 * 钉子全部重来一遍，而 X11 客户端在 attach 期间可能正在交帧。
	 *
	 * 赋值放在场景表面建好之后：那之前的失败路径没有树，也就没有「按哪块
	 * 表面建的」可言。 */
	view->x11->scene_serial = view->x11->serial;

	view->map.notify = view_map;
	wl_signal_add(&view->x11->surface->events.map, &view->map);
	view->unmap.notify = view_unmap;
	wl_signal_add(&view->x11->surface->events.unmap, &view->unmap);
	view->commit.notify = view_commit;
	wl_signal_add(&view->x11->surface->events.commit, &view->commit);

	view->popup = x11_is_popup(view->x11);
	vidow_view_x11_scale(view);

	/* 弹出物背后的模糊：节点垫在它自己的场景树底下。表面 / 场景节点拆掉的
	 * 时候由 x11_drop_surface 显式收（树归我们，节点也归我们）。 */
	if (view->popup && view->popup_blur == NULL) {
		view->popup_blur = vidow_popup_blur_register(server,
			view->scene_tree, &view->scene_tree->node,
			view->x11->surface, view, "x11", NULL);
	}

	/* 场景树刚建好：让状态机按「有没有 buffer」对齐一次。这里以前是
	 * 「replay 一次 map」并顺手把 x11_map_replayed 清掉 —— 那套标志位
	 * 现在不再参与判定，状态机只看 buffer。 */
	view->x11_map_replayed = false;
	xwm_surface_sync(view);

	/* 重开时那个 buffer 里到底是什么颜色？
	 *
	 * 已经确认的事实：X 侧有完整画面（import -window 抓得到），而我们手上
	 * 这一帧被画成了纯黑，且它不是 unmap 之前那个 buffer 指针。剩下的唯一
	 * 可能就是这个 buffer 本身是空的 —— 那么问题在 XWayland 交上来的内容，
	 * 不在我们怎么解释它。直接读像素，别再猜。 */
	if (view->x11->surface->buffer != NULL && server->renderer != NULL) {
		struct wlr_texture *tex =
			wlr_surface_get_texture(view->x11->surface);
		if (tex != NULL) {
			uint32_t px[3 * 4] = {0};
			int tw = (int)tex->width, th = (int)tex->height;
			int sx = tw / 4, sy = th / 4;
			struct wlr_texture_read_pixels_options opt = {
				.data = px,
				.stride = 4 * sizeof(uint32_t),
				.src_box = {.x = sx, .y = sy, .width = 3, .height = 1},
				.format = WL_SHM_FORMAT_ARGB8888,
			};
			if (wlr_texture_read_pixels(tex, &opt)) {
				wlr_log(WLR_INFO, "x11-tex %s: 纹理 %dx%d 采样 %08x %08x %08x",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					tw, th, px[0], px[1], px[2]);
			} else {
				wlr_log(WLR_INFO, "x11-tex %s: 读像素失败",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)");
			}
		}
	}

	/* Xwayland can associate an already-painted surface after its last commit.
	 * wlr_scene_surface_create() installs the commit listener, but there may be
	 * no new commit after this association. The current client buffer must be
	 * copied into the new scene buffer explicitly; replaying view_commit() alone
	 * only updates Vidow bookkeeping and cannot replay wlroots' old commit. */
	if (view->x11->surface->buffer != NULL) {
		wlr_scene_buffer_set_buffer(view->scene_surface->buffer,
			&view->x11->surface->buffer->base);
	}
	if (view->x11->surface->buffer != NULL) {
		view_commit(&view->commit, NULL);
	}

}

/* wayland surface 没了（窗口 withdrawn）但 X11 窗口还在：view 留着，下次
 * associate 重新建场景节点。 */
static void x11_drop_surface(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	x11_trace_view("drop", view, NULL);
	/* 一次性日志（不受 trace 开关控制）：重开时这条到底有没有走到，是
	 * 「新 surface 的 commit 监听挂不上」那个死结的关键判据 —— 不走的话
	 * scene_tree 留着，下一次 associate 被 x11_attach_surface 的
	 * `scene_tree != NULL` 挡掉，新内容永远进不来。 */
	wlr_log(WLR_INFO, "drop-surface: %s scene_tree=%p surf=%p",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		(void *)view->scene_tree,
		(void *)(view->x11 != NULL ? view->x11->surface : NULL));
	/* A wlroots X11 surface can be dissociated and associated again without the
	 * X window being destroyed (Chromium/CEF does this while rebuilding widgets).
	 * Release Vidow-owned scene/FX state before the old scene tree disappears;
	 * otherwise the next association can retain a stale FX node and targets. */
	vidow_anim_cancel(view->anim_open);
	view->anim_open = NULL;
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;
	vidow_anim_cancel(view->mini_anim);
	view->mini_anim = NULL;
	vidow_fx_release(&view->fx);
	/* Dissociation can bypass the XWayland unmap signal. Do the helper-window
	 * focus cleanup here as well, otherwise the next association keeps the
	 * stale focus_helper gate and can never replay its map edge. */
	struct vidow_view *borrowed_owner = server->focus_borrowed_from;
	if (borrowed_owner == view) {
		borrowed_owner = NULL;
	}
	if (view->focus_helper) {
		struct vidow_view *owner = borrowed_owner;
		server->focus_borrowed_from = NULL;
		view->focus_helper = false;
		if (owner != NULL && (!owner->mapped ||
				vidow_view_surface(owner) == NULL)) {
			owner = server->focused;
		} else if (owner == NULL) {
			owner = server->focused;
		}
		struct wlr_surface *back = owner != NULL ?
			vidow_view_surface(owner) : NULL;
		vidow_seat_focus_surface(server->seat, back);
		vidow_im_focus(server, back);
	}
	if (view->popup_blur != NULL) {
		vidow_popup_blur_unregister(view->popup_blur, true);
		view->popup_blur = NULL;
	}
	/* **监听器要在 scene_tree 判断之前摘。**
	 *
	 * 这三个挂在 wl_surface 上；wlroots 销毁表面时断言 commit 监听器列表
	 * 必须为空（types/wlr_compositor.c:733）。以前 scene_tree == NULL 就
	 * 直接 return，把三个监听器留在正在销毁的表面上 → abort。
	 * 另外它们可能已经被摘过一次（surface destroy 和 X destroy 都会走到
	 * 这里），所以逐个判空再摘。 */
	if (view->map.link.next != NULL && view->map.link.next != &view->map.link) {
		wl_list_remove(&view->map.link);
		wl_list_init(&view->map.link);
	}
	if (view->unmap.link.next != NULL &&
			view->unmap.link.next != &view->unmap.link) {
		wl_list_remove(&view->unmap.link);
		wl_list_init(&view->unmap.link);
	}
	if (view->commit.link.next != NULL &&
			view->commit.link.next != &view->commit.link) {
		wl_list_remove(&view->commit.link);
		wl_list_init(&view->commit.link);
	}
	if (view->scene_tree == NULL) {
		return;
	}
	/* 场景节点归 wlroots 所有，可能比 view 活得久：先把 tag 摘掉。 */
	view->scene_tree->node.data = NULL;
	/* 表面那个 buffer 节点**不碰**：它是 view->scene_tree 的孩子，表面一没
	 * wlroots 就顺手把它销毁了（scene_surface_handle_surface_destroy），
	 * 它的 tag 跟着一起走，不会留下野指针。反过来，这个回调有可能在它之后才
	 * 跑到（退出时 wl_display_destroy_clients 先收客户端表面），那时候
	 * view->scene_surface 已经指向释放过的内存 —— 老代码在这里摘 tag，
	 * IPC quit 必段错误（wlr_scene_node_destroy 也不允许节点身上还挂着
	 * destroy 监听器或 addon，想挂钩子都不行）。 */
	view->scene_surface = NULL;
	view->scene_tree = NULL;
	x11_trace_view("drop-done", view, NULL);
	/* Dissociate is not X11 destruction. Keep the view/layout identity, but
	 * force the next association to replay map and rebuild scene placement. */
	if (vidow_flip_on()) {
		vidow_flip_note("mapped", view, false);
	}
	view->mapped = false;
	view->x11_map_replayed = false;
	/* **强制清理缓冲缩放状态**：Steam 二次打开时，pref_scale 残留导致
	 * 下次 map 时状态机不同步（scene_tree disabled）。和 view_unmap
	 * 里那处重置逻辑一致（第6460行）。*/
	view->pref_scale = 0;
	vidow_portal_view_mapped(view, false);
}

/* 读一块表面纹理的采样值，判断它是不是全空。诊断用。 */
void vidow_view_probe_x11_texture(struct vidow_view *view) {
	if (view == NULL || view->x11 == NULL || view->x11->surface == NULL ||
			view->server == NULL || view->server->renderer == NULL) {
		return;
	}
	struct wlr_surface *surf = view->x11->surface;
	if (surf->buffer == NULL) {
		wlr_log(WLR_INFO, "x11-tex %s: 没有 buffer",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)");
		return;
	}
	struct wlr_texture *tex = wlr_surface_get_texture(surf);
	if (tex == NULL) {
		wlr_log(WLR_INFO, "x11-tex %s: 取不到纹理",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)");
		return;
	}
	uint32_t px[3] = {0};
	int tw = (int)tex->width, th = (int)tex->height;
	struct wlr_texture_read_pixels_options opt = {
		.data = px,
		.stride = 4 * sizeof(uint32_t),
		.src_box = {.x = tw / 4, .y = th / 4, .width = 3, .height = 1},
		.format = WL_SHM_FORMAT_ARGB8888,
	};
	bool ok = wlr_texture_read_pixels(tex, &opt);
	wlr_log(WLR_INFO, "x11-tex %s: %dx%d %s 采样 %08x %08x %08x",
		vidow_view_title(view) != NULL ?
			vidow_view_title(view) : "(无题)",
		tw, th, ok ? "" : "(读失败)", px[0], px[1], px[2]);
}

void vidow_view_attach_x11_surface(struct vidow_view *view) {
	x11_trace_view("associate", view, view->x11 != NULL ? view->x11->surface : NULL);
	x11_attach_surface(view);
	wlr_log(WLR_DEBUG, "x11: associate %s",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");

	/* map 请求常常比这条关联先到（Steam 关掉再打开时实测就是）：那时候
	 * surface 还是 NULL，判不了，于是事件等于丢了。更早的写法是这里去问
	 * 一次 X 的 IsViewable 再补一遍 map —— 但「X 说 viewable」和「我们有
	 * 东西可画」是两件事，补出来的可能是个空壳。
	 *
	 * 现在只做一件事：让状态机按「有没有 buffer」重新对齐。surface 刚
	 * 建出来时多半还没 buffer，那就什么都不做，等 commit 来说话。 */
	xwm_surface_sync(view);
}

void vidow_view_x11_gone(struct vidow_view *view) {
	x11_trace_view("dissociate", view, NULL);
	x11_drop_surface(view);
	/* **X 窗口没了，这条列也得让它走。**
	 *
	 * 这个函数故意不销毁 view（见下面那段：X 窗口先死、view 后死是常态），
	 * 但它漏了一步 —— view 还挂在 col->views 上。于是：
	 *
	 *   1. vidow_workspace_cleanup() 用 wl_list_empty(&col->views) 判「列空
	 *      不空」，链表里还躺着这个 view → 永远不删；
	 *   2. vidow_ipc_json_column() 的 windows 只报 window_is_toplevel 的窗口
	 *      （没上屏的被排除）→ IPC 看着是 `windows: []`；
	 *   3. arrange_columns 按旧宽度继续摆这条列 → 屏上一条什么都没有的竖条，
	 *      旁边的窗口不合拢。
	 *
	 * 这就是用户 2026-10-28 反复报的「屏幕上出现一条空白带，窗口没铺满」
	 * 「所有窗口关闭都会把列留下来」。实测：开一个 X11 窗口再关掉，空列
	 * 稳定 +1，宽度停在 949（= 半屏），而列里没有任何 toplevel 窗口。
	 *
	 * 只摘列、不销毁 view：view 留着是对的（X 窗口可能重建，重新 map 时
	 * view_map 会再调 view_ensure_column 把它放回条带）。摘完顺手收尸，
	 * 让这条空列立刻从 ws->columns 上消失，再重排一次让邻居合拢。 */
	vidow_view_leave_column(view);
	vidow_workspace_cleanup(view->server);
	vidow_arrange(view->server);
	/* **X 窗口没了就把 foreign-toplevel 条目摘掉。**
	 *
	 * X 窗口先死、view 后死是常态（xwm_x11.c 的 x11_handle_destroy 里写着：
	 * 32x32 / 1x1 的 helper 窗口 create 完立刻 destroy）。这条路径只清表面，
	 * **既不销毁 view、也不碰它的 toplevel handle**，于是条目一直挂在列表里。
	 *
	 * 实测（2026-10-27，zwlr 枚举）：30 条 toplevel 对 7 个真窗口，剩下 23 条
	 * 是 `Input` / `Default IME` / `rundll32` / `OpenGL 4.5` /
	 * `Steam SDL Dummy OpenGL Window` 这类 Steam + fcitx 的辅助窗口 —— X 树里
	 * 一个都搜不到，条目却还在，顶栏就一直摆着它们。 */
	vidow_portal_view_drop_toplevel(view);
}
static void view_x11_title_changed(struct vidow_view *view) {
	/* Xwayland can emit the first property update in the same batch as
	 * association. If the association was already complete when this view was
	 * created, this is the next safe point to attach the scene surface. */
	if (view->x11 != NULL && view->scene_tree == NULL &&
			view->x11->surface != NULL) {
		x11_attach_surface(view);
	}
	/* DEBUG：同 view_set_title()，日常日志不需要每换一次标题就一行。 */
	wlr_log(WLR_DEBUG, "title %s", vidow_view_title(view) != NULL ?
		vidow_view_title(view) : "(无题)");
	vidow_portal_view_title(view);
	vidow_rule_apply(view->server, view);
	vidow_view_note_names(view);
	vidow_ipc_emit_window(view->server, "window-title-changed", view);
}

static void view_x11_class_changed(struct vidow_view *view) {
	vidow_portal_view_title(view);
	vidow_rule_apply(view->server, view);
	vidow_view_note_names(view);
}

static void view_x11_parent_changed(struct vidow_view *view) {
	vidow_float_update_mode(view);
	vidow_portal_view_parent(view);
}

static void view_x11_geometry_changed(struct vidow_view *view) {
	if (!view->popup) {
		/* 普通窗口的位置由布局决定，X 报的坐标只在浮动窗口的首次摆放里当参考。 */
		return;
	}
	/* 弹出物改位置/尺寸就是它自己的事：重摆一遍（位置从 X 读）。
	 *
	 * 尺寸变了还得**重新声明场景表面的尺寸**：菜单常以 1x1 建窗、随后才长到
	 * 真实尺寸（Steam 好友列表右键菜单、Qt 下拉框都是这样）。场景表面的 dst
	 * 只在 vidow_view_x11_scale() 里设，而它原来只挂在「commit」和「全局缩放
	 * 变化」上 —— 客户端只是 configure 长大、画面内容没变就不会重新提交，
	 * 表面永远停在 1x1：屏幕上只有一个像素，看起来就是「右键菜单出不来」。 */
	if (view->kind == VIDOW_VIEW_X11) {
		vidow_view_x11_scale(view);
	}
	vidow_arrange(view->server);
}
static void view_x11_hints_changed(struct vidow_view *view) {
	/* min/max 提示要等到这里才齐，浮动判断得重跑一遍。 */
	x11_refresh_popup(view);
	vidow_float_update_mode(view);
}

/* `_NET_WM_WINDOW_TYPE` 是**另一条**消息（wlroots 的 set_hints 是 WM_HINTS）：
 * 客户端可以在映射之后才把窗口类型设成菜单/提示/下拉框，也可能反过来。类型变了
 * 就得重新分类（弹出物 <-> 普通窗口）、甚至把场景节点搬到另一层上。 */
static void view_x11_window_type_changed(struct vidow_view *view) {
	bool was_popup = view->popup;
	x11_refresh_popup(view);
	vidow_float_update_mode(view);
	/* **改判成弹出物之后，得让状态机再对齐一次。**
	 *
	 * _NET_WM_WINDOW_TYPE 常常晚于 map 到达（Chromium / Steam 的菜单都是先
	 * 建窗口、先映射，属性过一会儿才写）。这类窗口第一次进来时还是「普通
	 * 窗口」，走的是 view_map() 那条路 —— 场景节点按布局摆好、也可能被
	 * 浮窗规则摆到别处。
	 *
	 * 属性一到、x11_refresh_popup() 把它改判成弹出物、把场景树搬到
	 * layer_popups，但**没人再跑一遍上屏状态机**：节点还是布局时代的位置，
	 * 尺寸还是布局时代的 dest，于是弹出物画在错的坐标上（实测 test-xwayland-popup.sh
	 * 的「属性晚到」那节：place xp-popup -> 540,300，而客户端说的是 500,500）。
	 *
	 * xwm_surface_sync() 对弹出物会走 xwm.c 里那条专用分支（surface map +
	 * 按客户端绝对坐标摆位 + 打开节点），正是这里需要的收尾。 */
	if (!was_popup && view->popup) {
		xwm_surface_sync(view);
	}
}

/* WM_NORMAL_HINTS（min/max 尺寸）是另一条信号，必须有自己的回调：
 * wl_container_of 是按 listener 字段的地址反推 view 的，两个信号共用一个回调
 * 会算出错的 view 指针（这里踩过一次，直接段错误）。 */
static void view_x11_size_hints_changed(struct vidow_view *view) {
	vidow_float_update_mode(view);
	/* S3b：这条信号不只带 min/max 尺寸，也带 PAspect（宽高比）—— 客户端常常
	 * **映射之后**才设它（游戏进全屏那一刻），所以锁比例必须在这儿重判一次，
	 * 而不是只在建 view 时读一遍。 */
	vidow_mini_aspect_changed(view);
}

static void view_x11_configure_request(struct vidow_view *view,
		int rx, int ry, int rw, int rh, uint32_t rmask) {
	if (rw > 1 && rh > 1) {
		/* 记下来：这个请求常常早于「我们认出它是个对话框」，见 server.h */
		view->x11_want_w = rw;
		view->x11_want_h = rh;
	}
	/* 客户端往往先报尺寸、后报窗口类型/尺寸提示，先按最新的信息重判一次模式，
	 * 免得把一个「其实该浮动」的窗口当平铺窗口把尺寸请求吞掉。 */
	vidow_float_update_mode(view);

	wlr_log(WLR_DEBUG, "x11: %s 请求 configure %d,%d %ux%u（mask 0x%x）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		rx, ry, rw, rh, rmask);

	/* overlay 窗口（菜单/下拉框）是在请求自己的位置和尺寸，照单全收：我们只管
	 * 显示它，不指挥它。override-redirect 的窗口连这里都到不了（X 服务器直接
	 * 照办，不会来问 WM），会走这里的是「声明了弹出类型但没设 OR」的那批。 */
	if (view->popup) {
		uint32_t m = rmask;
		int x = (m & XCB_CONFIG_WINDOW_X) != 0 ? rx : view->x11->x;
		int y = (m & XCB_CONFIG_WINDOW_Y) != 0 ? ry : view->x11->y;
		int w = (m & XCB_CONFIG_WINDOW_WIDTH) != 0 ?
			(int)rw : view->x11->width;
		int h = (m & XCB_CONFIG_WINDOW_HEIGHT) != 0 ?
			(int)rh : view->x11->height;
		if (w < 1) {
			w = 1;
		}
		if (h < 1) {
			h = 1;
		}
		vidow_x11_surface_configure(view->x11, (int16_t)x, (int16_t)y,
			(uint16_t)w, (uint16_t)h);
		/* 尺寸真的变了就得重新声明场景表面的尺寸 —— 和 x11_set_geometry 里
		 * 那段同一个道理：漏了它，1x1 建窗的菜单永远长不大。 */
		if (view->kind == VIDOW_VIEW_X11) {
			vidow_view_x11_scale(view);
		}
		vidow_arrange(view->server);
		return;
	}
	/* 浮动窗口自己定尺寸：客户端请求多大就给多大（X11 里这是它报名尺寸的
	 * 正常途径）。位置仍按我们的规则（居中 / 跟随父窗口）。 */
	if (view->float_mode != VIDOW_FLOAT_NONE && rw > 1 &&
			rh > 1) {
		double w = rw, h = rh;
		vidow_xwm_from_native_size(view->server, view->output, w, h, &w, &h);
		view->float_geom.width = (int)lround(w);
		view->float_geom.height = (int)lround(h);
		vidow_arrange(view->server);
		return;
	}
	/* 平铺窗口的尺寸由布局决定，客户端说了不算；照旧重排一次，等于回一个
	 * configure 告诉它最终的几何，免得它一直等。 */
		vidow_arrange(view->server);
}
static void view_x11_request_activate(struct vidow_view *view) {
	/* X11 的 _NET_ACTIVE_WINDOW（wmctrl、Electron 的通知、任务栏都在用）：
	 * 跟 Wayland 的激活请求走同一条路（先把工作区端出来，再给焦点）。 */
	vidow_view_activate(view);
}
static void view_x11_request_fullscreen(struct vidow_view *view) {
	if (view->x11 == NULL) {
		return;
	}
	view_apply_fullscreen(view, view->x11->fullscreen, "X11 请求（_NET_WM_STATE_FULLSCREEN）");
}

static void view_x11_request_maximize(struct vidow_view *view) {
	if (view->x11 == NULL) {
		return;
	}

	/* X11 的请求没有事件结构，状态就在 surface 上（和全屏那条一样：
	 * x11_request_fullscreen 读的也是 x11->fullscreen）——
	 * _NET_WM_STATE_MAXIMIZED_HORZ / VERT 只要有一个立起来就算要最大化。 */
	bool want = view->x11->maximized_horz || view->x11->maximized_vert;
	struct vidow_column *col = view->column;

	if (col == NULL) {
		/* 浮窗 / 弹出物没有列：布局不替它撑满，把 X 那边的状态掰回「不最大化」
		 * —— 只掰真的有偏差的那种，不然「我们设属性 → 客户端回写属性 → 信号
		 * 又来 → 再设一次」会转圈。 */
		if (want) {
			view->maximize_granted = false;
			vidow_view_set_maximized(view, false);
		}
		return;
	}

	/* 状态已经一致：多半是我们自己 set_maximized 之后客户端把 _NET_WM_STATE
	 * 回写过来触发的回声（X11 没有「请求-应答」，状态就是属性本身）——
	 * 什么也不做，不然就是上面说的那个圈。 */
	if (want == col->maximized) {
		return;
	}

	if (want) {
		view->maximize_granted = true;
		vidow_column_set_maximized(col, true);
		return;
	}
	if (view->maximize_granted) {
		view->maximize_granted = false;
		vidow_column_set_maximized(col, false);
		return;
	}
	/* 列是**用户**最大化的：X 客户端自己把属性撤了，得把它掰回去 —— 不然它以为
	 * 自己是普通尺寸，我们却按整列宽度摆它。这里把去重作废一次（上次发出去的就是
	 * true，状态没变的话会被挡掉）；回声由上面的 `want == col->maximized` 兜住，
	 * 不会转圈。 */
	view->cfg_maximized_sent = false;
	vidow_view_set_maximized(view, col->maximized);
}

/* X11 客户端自己要求最小化 / 放回：`WM_CHANGE_STATE` 的 IconicState / NormalState，
 * 或者 `_NET_WM_STATE_HIDDEN`（托盘图标「收起来 / 放出来」走这条）。wlroots 把两种
 * 都转成同一个信号，event->minimize 就是要不要收起。 */
static void view_x11_request_minimize(struct vidow_view *view, bool minimize) {
	vidow_view_set_minimized(view, minimize);
}
void vidow_view_new_x11(struct vidow_x11_surface *xsurface) {
	struct vidow_server *server = vidow_x11_server_owner();
	if (server == NULL || xsurface == NULL) {
		return;
	}
	struct vidow_view *view = calloc(1, sizeof(*view));
	if (view == NULL) {
		return;
	}

	view->server = server;
	view->kind = VIDOW_VIEW_X11;
	view->x11 = xsurface;
	xsurface->view = view;
	view->output = view_default_output(server);
	if (view->output == NULL && !wl_list_empty(&server->outputs)) {
		struct vidow_output *o = wl_container_of(server->outputs.next, o, link);
		view->output = o->wlr_output;
	}
	view->tag.magic = VIDOW_VIEW_TAG_MAGIC;
	view->tag.view = view;
	view->anim_alpha = 1.0;
	view->anim_scale = 1.0;
	view->popup = x11_is_popup(xsurface);
	view->focus_on_open = true;
	view->focus_on_activate = true;
	view->honor_invalid_serial = false;
	view->id = ++server->next_view_id;
	wl_list_init(&view->ws_link);
	wl_list_init(&view->capture_sources);
	/* **X11 的 view 不走 wlroots 信号，大部分 .link 从来没 insert 过。**
	 * calloc 出来全零（link.next / prev 都是 NULL），view_destroy() 会逐个
	 * wl_list_remove() —— 对 NULL 节点解引用就崩。先把它们 init 成自环，
	 * 既让 view_destroy() 能在「X 窗口永久销毁」时安全复用，也让「这个 link
	 * 挂没挂东西」的判据统一（自环 = 空）。map/unmap/commit 三个真的会挂
	 * （见 x11_attach_surface），另外 init。 */
	wl_list_init(&view->set_title.link);
	wl_list_init(&view->set_parent.link);
	wl_list_init(&view->request_fullscreen.link);
	wl_list_init(&view->request_maximize.link);
	wl_list_init(&view->request_minimize.link);
	wl_list_init(&view->surface_commit.link);
	wl_list_init(&view->request_move.link);
	wl_list_init(&view->request_resize.link);
	wl_list_init(&view->destroy.link);
	wl_list_init(&view->map.link);
	wl_list_init(&view->unmap.link);
	wl_list_init(&view->commit.link);

	wl_list_insert(&server->views, &view->link);
	vidow_portal_view_new(view);

	/* 以前这里挂 20 个 wlroots 信号的监听器。现在窗口对象是我们自己的
	 * （src/x11surface.h），X 侧事件在 xwm_x11.c 里直接调下面这些函数 ——
	 * 少一层信号转发，也少一类「信号没接上就永远不动」的故障。 */

	/* 有些窗口送来时表面就已经关联好了（还没关联的就等 x11_associate）。 */
	x11_attach_surface(view);

	vidow_float_update_mode(view);

	/* 客户端一上来就是图标化状态（托盘类程序会这么启动）：直接当最小化收起来 ——
	 * 不占位置、不显示、不抢焦点。X 那边本来就是它自己的状态，不用再告诉它一次。 */
	if (xsurface->minimized) {
		view->minimized = true;
		view->minimized_seq = ++view->server->next_minimize_seq;
		wlr_log(WLR_INFO, "x11: %s 启动时就是图标化状态，按最小化收起来",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)");
	}

	wlr_log(WLR_INFO, "x11: 新窗口 %s（%s%s，%s%s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
		vidow_view_app_id(view) != NULL ? vidow_view_app_id(view) : "?",
		xsurface->override_redirect ? "，override-redirect" : "",
		view->float_mode == VIDOW_FLOAT_NONE ? "平铺" : "浮动",
		view->popup ? "，弹出物（不装饰/不进布局/不做动画）" : "");

	/* **X11：窗口一建出来就先回一次 configure，别等它进布局。**
	 *
	 * 这是一条死锁，Steam 关掉再打开必踩（用户 2026-10-26 报的「steam
	 * 第二次窗口打不开了，黑屏」）：
	 *
	 *   Steam 建好顶层窗口 → 等一个 configure 才 XMapWindow
	 *   → 不 map 就没有 wayland surface（xwayland 那时才导出）
	 *   → 没 surface 就进不了布局（arrange 只走 ws->columns 里的 view）
	 *   → 不进布局，vidow_place_view 永远不调 set_size，configure 一条不发
	 *   → Steam 永远等不到 → 画面永远黑
	 *
	 * 实测（2026-10-26）：`counts` 里 windows=194 / windows_mapped=6 /
	 * columns=0 —— Steam 的 view 在链上、不在列里；xwininfo 是
	 * `Map State: IsUnMapped`、`WM_STATE: not found`；日志里只有
	 * `xwm: create xid=0x2000041`，一条 x11-cfg 都没有。手动
	 * `xdotool windowmap` 之后 Vidow 立刻 associate 并正常显示（截图确认）。
	 *
	 * 所以这一步必须**脱离布局**独立做：用 X 侧报上来的几何（就是客户端
	 * 自己要的尺寸，create 时从 XGetWindowAttributes 读的）原样回给它。
	 * ICCCM 允许这样做，Hyprland 在 XSurface 建好时同样会先发一次。
	 *
	 * 已经 mapped 的不发：那种窗口走正常 arrange 那条路（vidow_place_view），
	 * 这里再发一次会和布局抢几何。
	 *
	 * 尺寸 <= 0 时不发：那是「客户端自己定」的语义，回 0x0 等于命令它保持
	 * 1x1（见 vidow_view_set_size 里 w <= 0 那段注释）。 */
	if (vidow_xtrace_on()) {
		wlr_log(WLR_INFO, "x11-cfg-create: %s or=%d popup=%d %dx%d mapped=%d",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
			(int)xsurface->override_redirect, (int)view->popup,
			xsurface->width, xsurface->height, (int)xsurface->mapped);
	}
	if (!xsurface->override_redirect && !view->popup &&
			xsurface->width > 0 && xsurface->height > 0 &&
			!xsurface->mapped) {
		vidow_x11_surface_configure(xsurface,
			(int16_t)xsurface->x, (int16_t)xsurface->y,
			(uint16_t)xsurface->width, (uint16_t)xsurface->height);
		wlr_log(WLR_INFO, "x11: 新窗口先回 configure %d,%d %dx%d"
			"（不等布局，破 map/configure 死锁）",
			xsurface->x, xsurface->y,
			xsurface->width, xsurface->height);
	}
}

/* ---- 给 xwm_x11.c 的直接入口 ------------------------------------------
 *
 * 这些就是以前那些 wlroots 信号回调，去掉信号层，直接按语义调。
 * 命名和 x11surface.h 里的声明一一对应。 */







void vidow_view_x11_property_changed(struct vidow_view *view, xcb_atom_t atom) {
	if (view == NULL || view->x11 == NULL) {
		return;
	}
	struct vidow_x11_server *x11 = vidow_x11_server_get();
	if (x11 == NULL) {
		return;
	}
	/* 一条 property 变更该引起哪些连锁反应，看是哪个属性。
	 * 以前这是二十个独立信号，现在按原子分派 —— 语义完全一样。 */
	if (atom == x11->atoms.net_wm_name || atom == x11->atoms.wm_name) {
		view_x11_title_changed(view);
	} else if (atom == XCB_ATOM_WM_CLASS) {
		view_x11_class_changed(view);
	} else if (atom == XCB_ATOM_WM_TRANSIENT_FOR) {
		view_x11_parent_changed(view);
	} else if (atom == x11->atoms.net_wm_window_type) {
		/* 类型变更必须留一行：这是「先普通窗口、后改判成弹出物」那条路
		 * 唯一的入口，判断它有没有到达全靠这一行。 */
		if (vidow_xtrace_on()) {
			wlr_log(WLR_INFO, "x11-type: %s 窗口类型变更 → 重判（当前 popup=%d）",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				view->popup);
		}
		view_x11_window_type_changed(view);
	} else if (atom == x11->atoms.wm_normal_hints) {
		view_x11_size_hints_changed(view);
	} else if (atom == x11->atoms.wm_hints) {
		view_x11_hints_changed(view);
	} else if (atom == x11->atoms.net_wm_state) {
		view_x11_request_fullscreen(view);
		view_x11_request_maximize(view);
		view_x11_request_minimize(view, view->x11->minimized);
	}
}

void vidow_view_x11_geometry_changed(struct vidow_view *view,
		int x, int y, int w, int h) {
	(void)x;
	(void)y;
	(void)w;
	(void)h;
	if (view == NULL) {
		return;
	}
	view_x11_geometry_changed(view);
}

void vidow_view_x11_state_changed(struct vidow_view *view) {
	if (view == NULL || view->x11 == NULL) {
		return;
	}
	view_x11_request_fullscreen(view);
	view_x11_request_maximize(view);
}

void vidow_view_x11_activate_request(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	view_x11_request_activate(view);
}

void vidow_view_x11_configure_request(struct vidow_view *view,
		int x, int y, int w, int h) {
	if (view == NULL) {
		return;
	}
	view_x11_configure_request(view, x, y, w, h, 0);
}
static void server_new_toplevel(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, new_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct vidow_view *view = calloc(1, sizeof(*view));
	if (view == NULL) {
		return;
	}

	view->server = server;
	view->kind = VIDOW_VIEW_XDG;
	view->xdg_toplevel = xdg_toplevel;
	view->output = view_default_output(server);
	if (view->output == NULL && !wl_list_empty(&server->outputs)) {
		struct vidow_output *o = wl_container_of(server->outputs.next, o, link);
		view->output = o->wlr_output;
	}
	view->tag.magic = VIDOW_VIEW_TAG_MAGIC;
	view->tag.view = view;
	view->focus_on_open = true;
	view->focus_on_activate = true;
	view->honor_invalid_serial = false;
	view->id = ++server->next_view_id;
	wl_list_init(&view->ws_link);
	wl_list_init(&view->capture_sources);

	view->scene_tree =
		wlr_scene_xdg_surface_create(server->layer_toplevels, xdg_toplevel->base);
	if (view->scene_tree == NULL) {
		free(view);
		return;
	}
	view->scene_tree->node.data = &view->tag;
	wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	view->anim_alpha = 1.0;
	view->anim_scale = 1.0;

	/* wlr_scene_xdg_surface_create() returns a tree holding the surface. */
	view->scene_surface = find_scene_surface(&view->scene_tree->node,
		xdg_toplevel->base->surface);
	view_takeover_surface_scale(view);
	if (view->scene_surface == NULL) {
		wlr_log(WLR_ERROR, "no scene surface for toplevel %s",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	}

	wl_list_insert(&server->views, &view->link);
	vidow_portal_view_new(view);

	view->map.notify = view_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &view->map);
	view->unmap.notify = view_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &view->unmap);
	view->commit.notify = view_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &view->commit);
	view->destroy.notify = view_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &view->destroy);
	view->set_title.notify = view_set_title;
	wl_signal_add(&xdg_toplevel->events.set_title, &view->set_title);
	view->set_parent.notify = view_set_parent;
	wl_signal_add(&xdg_toplevel->events.set_parent, &view->set_parent);
	view->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &view->request_fullscreen);
	view->request_maximize.notify = view_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &view->request_maximize);
	view->request_minimize.notify = view_request_minimize;
	wl_signal_add(&xdg_toplevel->events.request_minimize, &view->request_minimize);
	view->surface_commit.notify = view_surface_commit;
	if (vidow_view_surface(view) != NULL) {
		wl_signal_add(&vidow_view_surface(view)->events.commit,
			&view->surface_commit);
	}
	view->request_move.notify = view_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &view->request_move);
	view->request_resize.notify = view_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &view->request_resize);
}

/* popup 的 unconstrain 只能等它自己第一次 commit。
 *
 * wlr_xdg_popup_unconstrain_from_box() 内部会 wlr_xdg_surface_schedule_configure()，
 * 而那第一句就是 assert(surface->initialized) —— 而 new_popup 是在客户端
 * get_popup 的那一刻发的，popup 自己的 surface 还没 commit 过，于是直接
 * SIGABRT（踩过两次：noctalia 状态栏的提示、GTK 菜单，任何 Wayland 弹出物都能
 * 打死合成器）。所以这里把这件事推到 popup 的第一次 commit 上做，那时候
 * surface 已经 initialized 了。 */
struct vidow_popup_unconstrain {
	struct wl_listener commit;
	struct wlr_xdg_popup *popup;
	struct wlr_box box;
};

static void popup_unconstrain_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_unconstrain *u =
		wl_container_of(listener, u, commit);

	/* 第一次 commit 之后 initialized 才为真；万一这个信号发在角色 commit
	 * 之前，就等下一次 commit 再来（弹窗一般还会再 commit 一次）。 */
	if (!u->popup->base->initialized) {
		return;
	}
	wl_list_remove(&u->commit.link);
	/* 父面可能是 NULL（父面已销毁 / 不是 xdg surface），unconstrain 会沿
	 * parent 链解引用它 —— 没父面就不约束。 */
	if (u->popup->parent != NULL) {
		wlr_xdg_popup_unconstrain_from_box(u->popup, &u->box);
		wlr_log(WLR_DEBUG, "popup: unconstrain 完成（父面 %p，可用区 %d,%d %dx%d）",
			(void *)u->popup->parent, u->box.x, u->box.y,
			u->box.width, u->box.height);
	} else {
		wlr_log(WLR_DEBUG, "popup: 没有父面（%p），跳过 unconstrain",
			(void *)u->popup);
	}
	free(u);
}

/* 把「输出在布局里的盒子」平移成「根 toplevel 表面自己的坐标系」里的盒子。
 *
 * wlr_xdg_popup_unconstrain_from_box() 要的是**根 toplevel 表面的局部坐标**
 * （它内部会减掉 popup 自己的 toplevel 坐标，见 wlroots 的 wlr_xdg_popup.c），
 * 而 wlr_output_layout_get_box() 给的是布局坐标。窗口不在布局原点 (0,0) 上时
 * 这两个差着一整个窗口原点的距离，客户端于是以为「可见区」在它右边一整块屏的
 * 地方，把菜单摆到屏外：微信主窗口在 DP-2（x=2880）上，右键菜单的场景节点
 * 坐标是 4800,115（= 2880 + 1920，1920 就是那块屏的 x），而屏幕只有 0..3840
 * —— 菜单画在屏幕外，看着就是「弹窗不显示」。窗口在 (0,0) 附近时这项差值为 0，
 * 所以单屏 + 窗口贴左上角测不出来（Hyprland 正常，是因为它减了 t1ParentCoords，
 * 见它的 CPopup::unconstrain）。 */
void vidow_popup_box_to_toplevel_space(const struct vidow_view *view,
		struct wlr_box *box) {
	if (view == NULL || box == NULL || view->scene_surface == NULL ||
			view->scene_surface->buffer == NULL) {
		return;
	}
	int sx = 0, sy = 0;
	wlr_scene_node_coords(&view->scene_surface->buffer->node, &sx, &sy);
	box->x -= sx;
	box->y -= sy;
}

void vidow_popup_unconstrain_on_commit(struct vidow_server *server,
		struct wlr_xdg_popup *popup, const struct wlr_box *box) {
	(void)server;
	struct vidow_popup_unconstrain *u = calloc(1, sizeof(*u));
	if (u == NULL) {
		return;
	}
	u->popup = popup;
	if (box != NULL) {
		u->box = *box;
	}
	u->commit.notify = popup_unconstrain_commit;
	wl_signal_add(&popup->base->surface->events.commit, &u->commit);
}

/* 弹出物的「commit 了什么」诊断（见 server_new_popup 里那段注释）。 */
struct vidow_popup_trace {
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wlr_xdg_popup *popup;
	struct wlr_scene_tree *tree;
};

static void popup_trace_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_trace *t = wl_container_of(listener, t, commit);
	struct wlr_surface *s = t->popup->base->surface;
	int nx = 0, ny = 0;
	bool enabled = t->tree != NULL && t->tree->node.enabled;
	if (t->tree != NULL) {
		wlr_scene_node_coords(&t->tree->node, &nx, &ny);
	}
	wlr_log(WLR_DEBUG, "popup: commit 缓冲 %dx%d 逻辑 %dx%d 节点%s 在 %d,%d"
		"（表面 mapped=%d initialized=%d）",
		s->current.buffer_width, s->current.buffer_height,
		s->current.width, s->current.height,
		enabled ? "开着" : "关着", nx, ny, s->mapped,
		t->popup->base->initialized);
}

static void popup_trace_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_trace *t = wl_container_of(listener, t, destroy);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->destroy.link);
	free(t);
}

/* 弹出物的场景树登记 / 查找（见 server.h 里 vidow_popup_scene 的说明）。
 *
 * 只有「父面是弹出物」时查找才用得着：一级弹出物挂在顶层窗口的树上是**对的**
 * （wlroots 给的坐标就是相对顶层表面的），二级弹出物的父面是**一级弹出物**，
 * 它的坐标相对一级弹出物算 —— 挂到顶层窗口的树上等于把一级弹出物在窗口里的
 * 偏移整个丢掉，二级菜单于是平移一段「一级菜单相对窗口」的距离，看着就是错位。
 * 查不到（父弹出物不是我们建的 / 登记时没内存）就退回原来的挂法。 */
static struct wlr_scene_tree *popup_scene_tree_for(
		struct vidow_server *server, struct wlr_surface *surface) {
	struct vidow_popup_scene *ps;
	wl_list_for_each(ps, &server->popup_scenes, link) {
		if (ps->surface == surface) {
			return ps->tree;
		}
	}
	return NULL;
}

static void popup_scene_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_scene *ps = wl_container_of(listener, ps, destroy);
	/* 场景树由 wlroots 自己跟着 xdg_surface 销毁（见 types/scene/xdg_shell.c），
	 * 这里只把登记项摘掉 —— 留着的话下一个 surface 可能拿到同一个地址。 */
	wl_list_remove(&ps->destroy.link);
	wl_list_remove(&ps->link);
	free(ps);
}

static void popup_scene_register(struct vidow_server *server,
		struct wlr_surface *surface, struct wlr_scene_tree *tree) {
	struct vidow_popup_scene *ps = calloc(1, sizeof(*ps));
	if (ps == NULL) {
		return;
	}
	ps->surface = surface;
	ps->tree = tree;
	ps->destroy.notify = popup_scene_destroy;
	wl_signal_add(&surface->events.destroy, &ps->destroy);
	wl_list_insert(&server->popup_scenes, &ps->link);
}

/* 建一个弹出物的场景树：父面本身是弹出物（二级菜单）就挂到**父弹出物**的树上，
 * 否则挂 fallback（顶层窗口的树 / layer 表面的树）。
 *
 * 为什么要挑父树：wlroots 把弹出物节点的坐标（popup->current.geometry）当成
 * 「相对**父表面**」的偏移（types/scene/xdg_shell.c 的
 * scene_xdg_surface_update_position）。一级弹出物的父表面是窗口 / layer 表面，
 * 挂 fallback 就对；二级弹出物的父表面是**一级弹出物**，挂 fallback 等于把
 * 「一级菜单在窗口 / 状态栏里的位置」整段丢掉 —— 子菜单整体平移
 * (父菜单.x − 窗口.x, 父菜单.y − 窗口.y)，看着就是错位。查不到父树（父弹出物
 * 不是我们建的 / 登记时没内存）就退回 fallback。
 *
 * **两条路都得走这里**：窗口的弹出物（server_new_popup）和 layer surface 的
 * 弹出物（layer.c 的 layer_surface_new_popup）。7768fe7 只在窗口那条路上挑了
 * 父树，layer 那条路（状态栏菜单）漏了，子菜单于是还是老样子 —— 一条规则两个
 * 调用点，漏一个就是这次。 */
struct wlr_scene_tree *vidow_popup_scene_create(struct vidow_server *server,
		struct wlr_scene_tree *fallback, struct wlr_xdg_popup *popup) {
	struct wlr_scene_tree *parent = fallback;
	if (popup->parent != NULL) {
		struct wlr_scene_tree *pt =
			popup_scene_tree_for(server, popup->parent);
		if (pt != NULL) {
			parent = pt;
		}
	}

	/* 同一个 xdg_popup 可能被两条路各建一次：挂在 layer 表面上的弹出物，
	 * xdg_shell 的 new_popup 先以 parent = NULL 到达（这时归 server_new_popup，
	 * 先挂在 layer_toplevels 这棵全局树里），随后 layer_shell 的 new_popup
	 * 才把 parent 补上并再走一次这里。以前第二次会再建一棵树 —— 同一个
	 * surface 两个场景节点：菜单在**两块屏上各出现一份**，其中一份坐标还是
	 * 全局的。已经有树就只把它挪到正确的父树里，不再新建。 */
	struct wlr_scene_tree *tree =
		popup_scene_tree_for(server, popup->base->surface);
	if (tree != NULL) {
		if (tree->node.parent != parent) {
			wlr_scene_node_reparent(&tree->node, parent);
		}
		return tree;
	}
	tree = wlr_scene_xdg_surface_create(parent, popup->base);
	if (tree == NULL) {
		return NULL;
	}
	/* 登记自己：下一级菜单要按 surface 找到这棵树（一级的登记也没白记 ——
	 * 客户端可以再弹一个 popup，链就是这么接下去的）。 */
	popup_scene_register(server, popup->base->surface, tree);
	/* 弹出物背后的模糊（fx.c 的 vidow_popup_blur_register）：节点垫在这棵树
	 * 底下，弹出物移动 / 改尺寸时自动跟着走。树归 wlroots，销毁时它顺手把
	 * 模糊节点一起收（见 popup_blur_node_destroy）。 */
	vidow_popup_blur_register(server, tree, &tree->node,
		popup->base->surface, NULL, "xdg",
		&popup->base->surface->events.destroy);
	/* 每个弹出物挂一条「它自己 commit 了什么」的日志（一行一条，弹出物本来
	 * 就不常 commit）。「弹窗不显示」这类问题全卡在这一步上，光看场景树看不
	 * 出是客户端没交帧还是我们没画：
	 *   没有这一行           = 客户端在等 configure，压根没交缓冲
	 *   有行但节点关着/在屏外 = 我们这边的问题（节点没使能 / 位置算错）
	 *   有行、节点开着在屏内 = 画了但被别的节点压住（层级问题） */
	struct vidow_popup_trace *t = calloc(1, sizeof(*t));
	if (t != NULL) {
		t->popup = popup;
		t->tree = tree;
		t->commit.notify = popup_trace_commit;
		wl_signal_add(&popup->base->surface->events.commit, &t->commit);
		t->destroy.notify = popup_trace_destroy;
		wl_signal_add(&popup->events.destroy, &t->destroy);
	}
	return tree;
}

/* 弹出物的「根」是不是一块 layer surface（状态栏），是就把它找出来。
 *
 * 为什么二级弹出物要走这里：wlroots 的 wlr_layer_shell_v1.c 里，挂在 layer 表面上
 * 的弹出物是**两步**建的 —— 客户端先 xdg_wm_base.get_xdg_popup(parent=NULL)，这时
 * 只发 xdg_shell 那条 new_popup（父面还是 NULL），随后 zwlr_layer_surface_v1.get_popup
 * 才把父面设成 layer 表面并只发 layer 那条 new_popup。所以：一级弹出物归 layer.c 的
 * layer_surface_new_popup 管，**二级弹出物（父面是一级弹出物）只有 xdg_shell 那条信号**
 * —— 它的根还是那块 layer 表面，但 layer.c 永远看不到它，可用区得在这里自己算。 */
static struct vidow_layer_surface *popup_root_layer_surface(
		struct vidow_server *server, const struct wlr_xdg_popup *popup) {
	struct wlr_surface *s = popup->parent;
	while (s != NULL) {
		struct wlr_layer_surface_v1 *lsls =
			wlr_layer_surface_v1_try_from_wlr_surface(s);
		if (lsls != NULL) {
			struct vidow_layer_surface *ls;
			wl_list_for_each(ls, &server->layers, link) {
				if (ls->layer_surface == lsls) {
					return ls;
				}
			}
			return NULL;
		}
		struct wlr_xdg_surface *xs =
			wlr_xdg_surface_try_from_wlr_surface(s);
		if (xs == NULL || xs->role != WLR_XDG_SURFACE_ROLE_POPUP ||
				xs->popup == NULL) {
			return NULL;
		}
		s = xs->popup->parent;
	}
	return NULL;
}

static void server_new_popup(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, new_popup);
	struct wlr_xdg_popup *popup = data;

	/* 挂在 layer surface 上的 popup（状态栏的提示 / 菜单）归 layer.c 的
	 * new_popup 管 —— 那边把节点挂在那块 layer 自己的场景树里，这样弹出物才
	 * 跟着状态栏一起动。这里再建一份就是同一个 surface 两个场景节点：屏幕
	 * 上多出一层重影，而且第一个节点没人负责销毁。 */
	if (popup->parent != NULL &&
			wlr_layer_surface_v1_try_from_wlr_surface(popup->parent) != NULL) {
		return;
	}

	struct vidow_view *view = view_from_surface(server, popup->parent);
	/* 父面本身是弹出物时（二级菜单）按登记表找父树 —— 见
	 * vidow_popup_scene_create。 */
	vidow_popup_scene_create(server,
		view != NULL ? view->scene_tree : server->layer_toplevels, popup);

	/* wlroots 0.20 不保证 popup->parent 非空（父面已经销毁、或者父面不是
	 * xdg surface 时它就是 NULL），而 unconstrain 要沿 parent 链往上算
	 * toplevel 坐标 —— wlroots 那边第一句就是解引用，没有空指针检查，
	 * 于是 SIGSEGV（noctalia 一开，状态栏弹出的提示就把合成器打死在这）。
	 * 没有父面就不约束：坐标由客户端自己的 positioner 算，最多贴边时越界
	 * 一点，不会崩。 */
	if (popup->parent == NULL) {
		wlr_log(WLR_DEBUG, "popup: 没有父面（%p），跳过 unconstrain",
			(void *)popup);
		return;
	}

	struct wlr_box box = {0};
	if (view != NULL) {
		/* 按窗口**真正落**的那块屏算可用区：view->output 是「它属于哪块屏的
		 * 工作区」，IPC 挪位置 / 跨屏拖拽之后它可能还是旧的那块（浮窗的位置
		 * 是绝对坐标，不依赖 view->output），而菜单能摆在哪儿得看它现在压在
		 * 哪块屏上。 */
		struct wlr_output *out = view->output;
		struct wlr_output *under = wlr_output_layout_output_at(
			server->output_layout,
			view->geom.x + view->geom.width / 2.0,
			view->geom.y + view->geom.height / 2.0);
		if (under != NULL) {
			out = under;
		}
		if (out != NULL) {
			wlr_output_layout_get_box(server->output_layout, out, &box);
			vidow_popup_box_to_toplevel_space(view, &box);
		}
	} else {
		/* 根不是窗口：挂在 layer surface 上（状态栏的菜单）。一级弹出物由
		 * layer.c 算这个盒子，二级弹出物只到这里（见
		 * popup_root_layer_surface）—— 不补这一手 box 就是 {0,0,0,0}，客户端
		 * 拿到「可用区是个 0x0 的盒子」，把子菜单摆到那个点上去：看着就是
		 * 「二级菜单跑到屏幕左上角」。 */
		struct vidow_layer_surface *root =
			popup_root_layer_surface(server, popup);
		vidow_popup_box_to_layer_space(server, root, &box);
	}
	vidow_popup_unconstrain_on_commit(server, popup, &box);
}

bool vidow_view_init(struct vidow_server *server) {
	server->new_toplevel.notify = server_new_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_toplevel);
	server->new_popup.notify = server_new_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_popup);
	return true;
}
