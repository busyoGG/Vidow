/* 鼠标拖窗口（niri 的 interactive move）。
 *
 * 两种窗口、同一套 grab 状态（server->grab_mode / grab_view）：
 *
 *   浮动窗口 —— float.c 里的 vidow_float_begin_grab()：拖动改的是它自己的
 *               float_geom，松手时如果光标落在**另一块屏**上，窗口就改归那块屏
 *               显示着的工作区（跨屏拖拽）；
 *   平铺窗口 —— 这里的 vidow_tile_grab_*()：Mod+左键按住，窗口跟着光标飘起来
 *               （只动场景节点，布局一行都不重排），松手时按**落点**决定去哪：
 *               光标在哪块屏上就归那块屏的工作区；落在列带上的哪个缝就新开一条
 *               列插在那儿；落在**某条列里**（离那条列里的窗口缝近）就插进那条列、
 *               一列里上下叠起来 —— niri 的那两条（NewColumn / InColumn）。
 *
 * 拖的过程里落点随时算、随时用一块半透明的预览块标出来（niri 的 insert hint，
 * 见 insert_pos_at / vidow_insert_hint_update）：新列是一张整高的「卡片」骑在那个
 * 缝上，插进已有列是那条列里的一条横条（150 / 300 高那两种，照抄 niri 的
 * insert_hint_area）。
 *
 * 为什么松手才改工作区：拖动中途改工作区要连带动可见性、焦点、底部空位和清理，
 * 每帧都做一遍既贵又容易把焦点抖掉；视觉上窗口本来就跟着光标走（场景是全局的），
 * 所以「跨过去」这件事松手时一次做完就够了。
 *
 * 一处故意跟 niri 不一样：niri 一开始拖就把那个窗口从布局里摘走（后面的列当场
 * 并拢），vidow 让它那条列的空位**留着**（松手才重排）。所以预览块是按「屏幕上
 * 现在看得见的这些位置」画的，被拖窗口原来那个空位在拖动期间还看得见。 */
#include "server.h"
#include "ipc.h"
#include "workspace.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/box.h>
#include <wlr/util/log.h>

/* 落点预览块的尺寸（照 niri 的 insert_hint_area）：新列是一张 300 宽的「卡片」、
 * 整块可用高；插进已有列是那条列里的一条横条 —— 贴最上面 / 最下面时 150 高，
 * 骑在两个窗口之间那个缝上时 300 高。 */
#define INSERT_CARD_W 300
#define INSERT_BAR_H 150
#define INSERT_BAR_MID_H 300

/* 预览块的颜色：niri 的 layout.insert-hint 默认色 #ffc87f80（橙 255,200,127、
 * alpha 128）。wlr_scene_rect 的颜色是**预乘**的（wlroots 直接把它交给渲染通道
 * 的预乘混合，见 wlr_scene.c 里 scene_rect 那段），所以这里的 rgb 已经乘过
 * alpha —— 不乘的话叠在亮色窗口上会变成加法而不是半透明（实测叠在纯红窗口上
 * 量到 (255,200,127)，预乘之后是 niri 那个 (127,100,63)）。 */
static const float INSERT_HINT_COLOR[4] = { 0.502f, 0.394f, 0.250f, 0.502f };

/* 光标底下是哪块屏（没有屏就是 NULL）。 */
static struct wlr_output *output_under_cursor(struct vidow_server *server) {
	return wlr_output_layout_output_at(server->output_layout,
		server->cursor->x, server->cursor->y);
}

/* 这块屏现在显示着的工作区（特殊工作区开着就是它）。 */
static struct vidow_workspace *shown_workspace(struct vidow_server *server,
		struct wlr_output *output) {
	if (output == NULL) {
		return NULL;
	}
	return vidow_workspace_shown_for(server, output);
}

/* 落点（niri 的 InsertPosition）：要么是「列带上的一个新列位」，要么是「某条列
 * 里的第几个窗口位置」（一列里叠起来）。 */
struct insert_pos {
	bool into_column;
	/* 新列：插到条带上的第几位；进列：插到那条列里的第几位（0 = 最上面） */
	int index;
	/* into_column 时：进哪条列 */
	struct vidow_column *column;
	/* 预览块（屏幕坐标）。width <= 0 = 这次没有落点、不画。 */
	struct wlr_box hint;
};

/* 这条列第 k 个「窗口缝」的 y：k < n 是第 k 个还占位置的窗口的上边缘，k == n 是
 * 最下面那个窗口的下边缘 + gap（niri 的 tile_offsets 末尾那个 dummy —— 它让
 * 「插到最后一个下面」也能选到）。列里只剩被拖的那一个（n == 0）时缝就是这条列
 * 自己的上下两头（top_y / bottom_y）。 */
static int tile_gap_y(struct vidow_column *col, struct vidow_view *drag, int k,
		int n, int top_y, int bottom_y) {
	if (n == 0) {
		return k <= 0 ? top_y : bottom_y;
	}
	if (k >= n) {
		return bottom_y;
	}
	int i = 0;
	struct vidow_view *v;
	wl_list_for_each(v, &col->views, column_link) {
		if (v == drag || v->minimized) {
			continue;
		}
		if (i == k) {
			return v->geom.y;
		}
		i++;
	}
	return top_y;
}

/* 算落点（niri 的 ScrollingSpace::insert_position + insert_hint_area 那一套）：
 *
 *   横向：光标离哪条列的**左边缘**最近 → 新列就插那个缝里（closest_i）；
 *   纵向：光标离同一条列里哪个**窗口缝**最近 → 就插到那条列的那一位（ti）；
 *   两个距离谁小听谁的 —— niri 就是一句 `if vert_dist <= hor_dist`：离列的左边缘
 *   近 = 新开一条列，离窗口的上下缝近 = 插进这条列（niri 的 InColumn）。
 *
 * 位置一律取**屏幕上看得见的那块**：列的 screen_x/slot_width 是 arrange 记下来的、
 * 窗口的 geom 就是现在这一块 —— 全屏那列占一整屏、整列最小化的列不占位这些规矩
 * 只有 arrange 知道，这里不重算条带数学。
 *
 * drag = 正被拖的那个窗口：它自己不算落点（条带上跳过它那条列、列里跳过它那个）
 * —— 落点说的是「剩下这些」之间的位置，插进去之后它就落在那个缝上。 */
static void insert_pos_at(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output,
		struct vidow_view *drag, struct insert_pos *out) {
	memset(out, 0, sizeof(*out));
	if (server == NULL || ws == NULL || output == NULL || drag == NULL) {
		return;
	}
	struct wlr_box work = vidow_work_area(server, output);
	int inner = vidow_gap_inner(&server->config);
	if (inner < 0) {
		inner = 0;
	}
	int half = inner / 2;
	/* niri 里光标也先各挪半个 gap：那是「瞄着缝的中心」的意思。 */
	int px = (int)server->cursor->x + half;
	int py = (int)server->cursor->y + half;
	int card_h = work.height - 2 * inner;
	if (card_h < 1) {
		card_h = 1;
	}

	/* 拿掉被拖的那个之后，它那条列里还剩几个窗口 —— 一个都不剩它就不在布局里了
	 * （niri 是先把它从布局里摘走、再算落点的）。 */
	int n_drag = 0;
	if (drag->column != NULL) {
		struct vidow_view *v;
		wl_list_for_each(v, &drag->column->views, column_link) {
			if (v != drag && !v->minimized) {
				n_drag++;
			}
		}
	}
	bool drag_stays = n_drag > 0;

	/* 条带上这些列：光标落在哪条列上（containing）、**最近的一条列缝**在哪儿
	 * （closest_i = 新列的落点）、最左边那条的左边（first_l）、最右边那条的右
	 * 边缘 + gap（niri 的 column_x(len)，它也算一条缝）。
	 *
	 * 「列缝」数的是**剩下的**那些列的左边缘（index 空间里不含被拖的那条）——
	 * niri 的 column_xs 就是这样，左右两边的缝都算：光标在哪条列的右半边时，
	 * 最近的那条缝是它右边那条列的左边缘（= 插到它们两个中间）。
	 *
	 * 被拖的那条列算不算「光标落在哪条列上」：算。它那条列的空位在 vidow 里还
	 * 留在条带上（niri 会当场并拢，vidow 松手才重排），所以落在自己那个空位上
	 * 就是「放回原处」或者「在自己这条列里挪个上下位置」—— 该按自己这条列的缝
	 * 来判。它自己也在「谁在谁边上」的名单里（还有别的窗口时 niri 里它也还在
	 * 布局里），但 index 照样按「剩下的那些」数。 */
	int m = 0;      /* 剩下的列有几条 */
	int i = 0;      /* 这一条在「剩下的那些」里的序号 */
	bool first_seen = false, last_seen = false;
	int first_l = 0, last_right = 0, after_last = 0;
	int closest_i = 0, closest_d = INT_MAX;
	struct vidow_column *containing = NULL;
	struct vidow_column *c;
	wl_list_for_each(c, &ws->columns, link) {
		if (c->slot_width <= 0) {
			continue; /* 整列最小化的：不在条带上占位置 */
		}
		bool mine = (c == drag->column);
		int l = c->screen_x;
		if (l <= px) {
			containing = c;
		}
		if (!mine || drag_stays) {
			/* 拿掉被拖的那个之后它还在布局里：它的左边缘算一条缝 */
			if (!first_seen) {
				first_l = l;
				first_seen = true;
			}
			int d = l > px ? l - px : px - l;
			if (d < closest_d) {
				closest_d = d;
				closest_i = i;
			}
			last_right = l + c->slot_width;
			last_seen = true;
		}
		if (!mine) {
			m++;
			i++;
		}
	}
	if (last_seen) {
		after_last = last_right + inner;
		int d = after_last > px ? after_last - px : px - after_last;
		if (d < closest_d) {
			closest_d = d;
			closest_i = m;
		}
	}

	/* 什么都不剩（niri 的 `columns.is_empty()` 早退）：落点只能是「最前面那条
	 * 新列位」，预览块就是那张整高的卡片、画在列带开头。 */
	if (m == 0 && n_drag == 0) {
		out->into_column = false;
		out->index = 0;
		int card_x = work.x;
		int lo = work.x - INSERT_CARD_W / 2;
		int hi = work.x + work.width - INSERT_CARD_W / 2;
		if (card_x < lo) {
			card_x = lo;
		}
		if (card_x > hi) {
			card_x = hi;
		}
		out->hint = (struct wlr_box){ card_x, work.y + inner, INSERT_CARD_W,
			card_h };
		return;
	}

	/* 光标在列带左边那一条里（外层 gap）：落在最前面（niri 的 `if x < 0 →
	 * NewColumn(0)`）—— 卡片整块摆在条带左边。 */
	if (containing == NULL || px < first_l) {
		out->into_column = false;
		out->index = 0;
		int lo = work.x - INSERT_CARD_W / 2;
		int hi = work.x + work.width - INSERT_CARD_W / 2;
		int card_x = first_l - INSERT_CARD_W - inner;
		if (card_x < lo) {
			card_x = lo;
		}
		if (card_x > hi) {
			card_x = hi;
		}
		out->hint = (struct wlr_box){ card_x, work.y + inner, INSERT_CARD_W,
			card_h };
		return;
	}

	/* 这条列里有哪些「窗口缝」：每个还占位置的窗口的上边缘（被拖的那个不算），
	 * 外加最下面那个窗口的下边缘 + gap。 */
	int n = 0, top_y = work.y, bottom_y = work.y + work.height + inner;
	struct vidow_view *v;
	wl_list_for_each(v, &containing->views, column_link) {
		if (v == drag || v->minimized) {
			continue;
		}
		if (n == 0) {
			top_y = v->geom.y;
		}
		bottom_y = v->geom.y + v->geom.height + inner;
		n++;
	}

	/* 最近的窗口缝（纵向）vs 最近的**列缝**（横向）—— niri 的 hor_dist / vert_dist：
	 * 横向那个是「到最近一条列左边缘的距离」，**左右两边的缝都算数**（光标在哪条列
	 * 的右半边时最近的就是它右边那条列的左边缘），不是「到光标底下这条列左边缘的
	 * 距离」。谁小听谁的：离列缝近 = 新开一条列，离窗口缝近 = 插进这条列。 */
	int td = INT_MAX, ti = 0;
	for (int k = 0; k <= n; k++) {
		int y = tile_gap_y(containing, drag, k, n, top_y, bottom_y);
		int d = y > py ? y - py : py - y;
		if (d < td) {
			td = d;
			ti = k;
		}
	}
	int hd = closest_d;

	/* 光标在最后一条列（别的列，被拖那条不算）右边 + gap 之外：落在最后
	 * （niri 的 `col_idx == len → NewColumn(closest_col_idx)`）。 */
	if (m > 0 && px >= after_last) {
		out->into_column = false;
		out->index = closest_i;
		int card_x = after_last;
		int lo = work.x - INSERT_CARD_W / 2;
		int hi = work.x + work.width - INSERT_CARD_W / 2;
		if (card_x < lo) {
			card_x = lo;
		}
		if (card_x > hi) {
			card_x = hi;
		}
		out->hint = (struct wlr_box){ card_x, work.y + inner, INSERT_CARD_W,
			card_h };
		return;
	}

	if (hd <= td) {
		/* 离列左边缘更近 → 新开一条列：卡片骑在 closest_i 那个缝上 —— 上一条列
		 * （在条带上排在它前面那条）的右边缘和这一条的左边缘的**中间**（niri
		 * 写的是 column_x(i) - 宽 / 2 - gap / 2，两头中间没夹着别的东西时两者
		 * 一样）。 */
		out->into_column = false;
		out->index = closest_i;
		int left_edge = work.x, right_edge = work.x;
		int k = 0;
		wl_list_for_each(c, &ws->columns, link) {
			if (c == drag->column || c->slot_width <= 0) {
				continue;
			}
			if (k == closest_i) {
				right_edge = c->screen_x;
				break;
			}
			left_edge = c->screen_x + c->slot_width;
			k++;
		}
		int card_x;
		if (m == 0) {
			/* 这块工作区上没有别的列了：新列就落在列带开头 */
			card_x = work.x;
		} else if (closest_i == 0) {
			card_x = right_edge - INSERT_CARD_W - inner;
		} else if (closest_i >= m) {
			card_x = left_edge + inner;
		} else {
			card_x = (left_edge + right_edge) / 2 - INSERT_CARD_W / 2;
		}
		/* niri：卡片至少露一半在屏上（贴着屏边的时候别整个跑出去）。 */
		int lo = work.x - INSERT_CARD_W / 2;
		int hi = work.x + work.width - INSERT_CARD_W / 2;
		if (card_x < lo) {
			card_x = lo;
		}
		if (card_x > hi) {
			card_x = hi;
		}
		out->hint = (struct wlr_box){ card_x, work.y + inner, INSERT_CARD_W,
			card_h };
		return;
	}

	/* 插进这条列：横条画在那个缝上（niri 的 InColumn 三种位置）。 */
	int gy = tile_gap_y(containing, drag, ti, n, top_y, bottom_y);
	int bar_h, bar_y;
	if (ti <= 0) {
		bar_h = INSERT_BAR_H;
		bar_y = gy;
	} else if (ti >= n) {
		bar_h = INSERT_BAR_H;
		bar_y = gy - inner - INSERT_BAR_H;
	} else {
		bar_h = INSERT_BAR_MID_H;
		bar_y = gy - half - INSERT_BAR_H;
	}
	out->into_column = true;
	out->column = containing;
	out->index = ti;
	out->hint = (struct wlr_box){ containing->screen_x, bar_y,
		containing->slot_width, bar_h };
}

/* 抬到最上面（已经是最上面了一个节点都不动 —— raise_to_top 要动链表，白动一下
 * 会多记一次损伤）。 */
static void raise_node(struct wlr_scene_node *node) {
	if (node == NULL || node->parent == NULL) {
		return;
	}
	if (node->parent->children.prev == &node->link) {
		return;
	}
	wlr_scene_node_raise_to_top(node);
}

void vidow_insert_hint_hide(struct vidow_server *server) {
	if (server != NULL && server->insert_hint != NULL) {
		wlr_scene_node_set_enabled(&server->insert_hint->node, false);
	}
}

void vidow_insert_hint_update(struct vidow_server *server) {
	if (server == NULL || server->insert_hint == NULL) {
		return;
	}
	/* 小窗（S4）落点区亮着的时候**不画**插入提示块：两个预览同时画，用户不知道
	 * 松手会得到哪个（得到的是落点区那个 = 迷你小窗）。 */
	if (server->mini_drop_active) {
		vidow_insert_hint_hide(server);
		return;
	}
	struct vidow_view *view = server->grab_mode == VIDOW_GRAB_TILE ?
		server->grab_view : NULL;
	struct wlr_output *output = output_under_cursor(server);
	struct vidow_workspace *ws = shown_workspace(server, output);
	if (view == NULL || view->column == NULL || ws == NULL) {
		vidow_insert_hint_hide(server);
		return;
	}
	struct insert_pos pos;
	insert_pos_at(server, ws, output, view, &pos);
	struct wlr_box box = pos.hint;
	if (box.width <= 0 || box.height <= 0) {
		vidow_insert_hint_hide(server);
		return;
	}
	/* 只画在光标底下那块屏里：预览块贴着屏边的时候会被裁掉一半（跟窗口一样，
	 * 见 view_clip_box），不许糊到隔壁显示器上去。 */
	struct wlr_box out_box = {0};
	struct wlr_box clipped = box;
	wlr_output_layout_get_box(server->output_layout, output, &out_box);
	if (out_box.width < 1 || out_box.height < 1 ||
			!wlr_box_intersection(&clipped, &box, &out_box) ||
			clipped.width <= 0 || clipped.height <= 0) {
		vidow_insert_hint_hide(server);
		return;
	}
	wlr_scene_rect_set_size(server->insert_hint, clipped.width,
		clipped.height);
	wlr_scene_node_set_position(&server->insert_hint->node, clipped.x,
		clipped.y);
	wlr_scene_node_set_enabled(&server->insert_hint->node, true);
	/* 块压在别的窗口之上、**被拖的那个之下**（niri 把被拖的窗口画在提示块之上）：
	 * 拖的过程里别人可能重排过层级（vidow_restack），这里每次指针动都收口一次
	 * —— 层级已经对的时候 raise_node 一个节点都不碰。 */
	raise_node(&server->insert_hint->node);
	if (view->scene_tree != NULL) {
		raise_node(&view->scene_tree->node);
	}
}

bool vidow_drag_init(struct vidow_server *server) {
	if (server == NULL || server->layer_toplevels == NULL) {
		return false;
	}
	/* 建在窗口那一层里（拖动时靠 raise 收层级：块在所有窗口之上、被拖的那个
	 * 之下），平时关着。 */
	server->insert_hint = wlr_scene_rect_create(server->layer_toplevels, 1, 1,
		(float[4]){ INSERT_HINT_COLOR[0], INSERT_HINT_COLOR[1],
			INSERT_HINT_COLOR[2], INSERT_HINT_COLOR[3] });
	if (server->insert_hint == NULL) {
		wlr_log(WLR_ERROR, "drag: 落点预览块建不出来");
		return false;
	}
	wlr_scene_node_set_enabled(&server->insert_hint->node, false);
	/* 小窗（S4）的落点区 + 悬停工具条也建在这儿（同一个层、同一种「平时关着、
	 * 用的时候摆出来」的做法）。 */
	if (!vidow_mini_ui_init(server)) {
		return false;
	}
	return true;
}

/* ============================================ 小窗（mini window，S4）：落点区
 *
 * 用户要的那条主入口：**拖窗口的时候**在每块屏右上角亮出落点区（约 15%×20%，
 * 带目标预览），松手 → 迷你小窗。
 *
 * 三条写死的规矩：
 *   1. **只在拖拽中出现** —— 光标不在落点区里，两块矩形就是关着的。常亮的话
 *      「盲拖到角落」会跟现有的平铺落点撞车（用户点名的那条）。
 *   2. 落点区亮着的时候**不画**平铺的插入提示块（见 vidow_insert_hint_update
 *      第一句）—— 两个预览同时画，用户不知道松手会得到哪个。
 *   3. 预览块 = 松手会得到的那个迷你小窗（`mini_w × mini_h`，贴右上角）。不是
 *      落点区本身的形状 —— 用户看的是「会变成多大」。
 *
 * 颜色是**预乘**的（wlr_scene_rect 直接交给预乘混合，见上面 INSERT_HINT_COLOR
 * 那段说明）：rgb 已经乘过 alpha。 */

/* 落点区底板：偏青的半透明块。预览块亮一点，好在底板里认出来。 */
static const float MINI_DROP_ZONE_COLOR[4] = { 0.06f, 0.22f, 0.18f, 0.35f };
static const float MINI_DROP_PREVIEW_COLOR[4] = { 0.09f, 0.42f, 0.32f, 0.45f };
/* 工具条：深底 + 四个按钮（按钮稍微亮一点，点得中看得见）。 */
static const float MINI_TOOLBAR_BG_COLOR[4] = { 0.04f, 0.04f, 0.05f, 0.55f };
static const float MINI_TOOLBAR_BTN_COLOR[4] = { 0.16f, 0.16f, 0.20f, 0.75f };
/* 按钮上的字形颜色（预乘，同上）。 */
static const float MINI_TOOLBAR_GLYPH_COLOR[4] = { 0.78f, 0.80f, 0.84f, 0.85f };

#define MINI_TOOLBAR_BTN_W 34
#define MINI_TOOLBAR_BTN_H 26
#define MINI_TOOLBAR_PAD 3
#define MINI_TOOLBAR_GLYPH_N 5    /* 字模 5x5 */
#define MINI_TOOLBAR_GLYPH_PX 3   /* 每个亮点 3x3 逻辑像素 → 字形 15x15 */

/* 四个按钮的**字形**（5x5 像素字模，'#' = 亮）。
 *
 * 用户报的「小窗的工具栏没有内容只有四个格子」：这四个按钮以前就是四个纯色
 * 的 wlr_scene_rect，一个字形都没有（实测每个按钮 0/884 个非底色像素）。
 * 合成器自绘的东西没有字体 / 位图可借，就用最笨也最稳的办法：每个亮点一个
 * 小方块（wlr_scene_rect），颜色统一 —— 34x26 的按钮里放 15x15 的字形，够认。
 *   [0] 放大   = 向上箭头      [1] 收成图标 = 小方块
 *   [2] 还原成列 = 三根竖条    [3] 关闭     = 叉                          */
static const char *const MINI_TOOLBAR_GLYPH[VIDOW_MINI_BTN_COUNT]
		[MINI_TOOLBAR_GLYPH_N] = {
	{ "..#..", ".###.", "#.#.#", "..#..", "..#.." },
	{ ".....", ".###.", ".###.", ".###.", "....." },
	{ ".#.#.", ".#.#.", ".#.#.", ".#.#.", ".#.#." },
	{ "#...#", ".#.#.", "..#..", ".#.#.", "#...#" },
};

/* 配置里的百分比键（"15%"）-> 比例。读不出来就用兜底值（写配置那一刻已经校验
 * 过，见 config.c 的 K_STRING 分支）。float.c 里有一份同样用途的 mini_frac()，
 * 那份是 static 的 —— 这里不为了共用一行去改它的可见性。 */
static double mini_drop_pct(const char *text, double fallback) {
	double pct = 0.0;
	if (text != NULL && text[0] != '\0' &&
			vidow_config_parse_percent(text, &pct)) {
		return pct / 100.0;
	}
	return fallback;
}

bool vidow_mini_drop_zone_box(struct vidow_server *server,
		struct wlr_output *output, struct wlr_box *zone,
		struct wlr_box *preview) {
	if (server == NULL || output == NULL) {
		return false;
	}
	const struct vidow_config *cfg = &server->config;
	if (!cfg->mini_enabled || !cfg->mini_drop_zone) {
		return false;
	}
	struct wlr_box work = vidow_work_area(server, output);
	if (work.width < 1 || work.height < 1) {
		return false;
	}
	double wf = mini_drop_pct(cfg->mini_drop_zone_w, 0.15);
	double hf = mini_drop_pct(cfg->mini_drop_zone_h, 0.20);
	if (wf <= 0.0 || hf <= 0.0) {
		return false;
	}
	int zw = (int)lround(wf * (double)work.width);
	int zh = (int)lround(hf * (double)work.height);
	if (zw < 1) {
		zw = 1;
	}
	if (zh < 1) {
		zh = 1;
	}
	/* 右上角（迷你态的默认锚点就是那儿，见 float.c 的 mini_default_anchor）。 */
	if (zone != NULL) {
		*zone = (struct wlr_box){
			.x = work.x + work.width - zw,
			.y = work.y,
			.width = zw,
			.height = zh,
		};
	}
	if (preview != NULL) {
		int mw = (int)lround(mini_drop_pct(cfg->mini_w, 0.28) *
			(double)work.width);
		int mh = (int)lround(mini_drop_pct(cfg->mini_h, 0.28) *
			(double)work.height);
		if (mw < 1) {
			mw = 1;
		}
		if (mh < 1) {
			mh = 1;
		}
		*preview = (struct wlr_box){
			.x = work.x + work.width - mw,
			.y = work.y,
			.width = mw,
			.height = mh,
		};
	}
	return true;
}

void vidow_mini_drop_zone_hide(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	server->mini_drop_active = false;
	server->mini_drop_output = NULL;
	if (server->mini_drop_zone != NULL) {
		wlr_scene_node_set_enabled(&server->mini_drop_zone->node, false);
	}
	if (server->mini_drop_preview != NULL) {
		wlr_scene_node_set_enabled(&server->mini_drop_preview->node, false);
	}
}

void vidow_mini_drop_zone_update(struct vidow_server *server) {
	if (server == NULL || server->mini_drop_zone == NULL) {
		return;
	}
	/* 只在拖拽中亮：平铺拖拽 / 浮窗拖拽 / 自由小窗拖拽（三条都走 grab_mode）。 */
	if (server->grab_mode == VIDOW_GRAB_NONE || server->grab_view == NULL) {
		vidow_mini_drop_zone_hide(server);
		return;
	}
	struct wlr_output *output = output_under_cursor(server);
	struct wlr_box zone = {0}, preview = {0};
	if (!vidow_mini_drop_zone_box(server, output, &zone, &preview)) {
		vidow_mini_drop_zone_hide(server);
		return;
	}
	if (!wlr_box_contains_point(&zone, server->cursor->x,
			server->cursor->y)) {
		/* 光标不在里面：收起来（「只在拖拽中出现」是死规矩，不是「拖拽时一直
		 * 亮着」—— 盲拖到角落松手不能命中）。 */
		vidow_mini_drop_zone_hide(server);
		return;
	}
	/* 预览块可能比落点区大（15%×20% vs 28%×28%）：裁到落点区里，不然它糊到
	 * 落点区外面，用户会以为整个预览块都是「松手区」。 */
	struct wlr_box clipped = preview;
	if (!wlr_box_intersection(&clipped, &preview, &zone) ||
			clipped.width < 1 || clipped.height < 1) {
		clipped = zone;
	}
	wlr_scene_rect_set_size(server->mini_drop_zone, zone.width, zone.height);
	wlr_scene_node_set_position(&server->mini_drop_zone->node, zone.x, zone.y);
	wlr_scene_node_set_enabled(&server->mini_drop_zone->node, true);
	wlr_scene_rect_set_size(server->mini_drop_preview, clipped.width,
		clipped.height);
	wlr_scene_node_set_position(&server->mini_drop_preview->node, clipped.x,
		clipped.y);
	wlr_scene_node_set_enabled(&server->mini_drop_preview->node, true);
	raise_node(&server->mini_drop_zone->node);
	raise_node(&server->mini_drop_preview->node);
	if (server->grab_view->scene_tree != NULL) {
		raise_node(&server->grab_view->scene_tree->node);
	}
	if (!server->mini_drop_active || server->mini_drop_output != output) {
		wlr_log(WLR_INFO, "mini: 落点区亮（%s 上 %d,%d %dx%d，松手得到 %dx%d "
			"@ %d,%d 的迷你小窗）",
			output != NULL ? output->name : "-", zone.x, zone.y, zone.width,
			zone.height, preview.width, preview.height, preview.x,
			preview.y);
	}
	server->mini_drop_active = true;
	server->mini_drop_output = output;
	/* 落点区一亮，平铺的插入提示块就得收掉（两个预览不能同时画）。 */
	vidow_insert_hint_hide(server);
}

bool vidow_mini_drop_zone_hit(struct vidow_server *server, double lx,
		double ly) {
	if (server == NULL) {
		return false;
	}
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, lx, ly);
	struct wlr_box zone = {0};
	if (!vidow_mini_drop_zone_box(server, output, &zone, NULL)) {
		return false;
	}
	return wlr_box_contains_point(&zone, lx, ly);
}

/* --------------------------------------------- 小窗（S4）：悬停工具条
 *
 * 迷你 / 图标态悬停时露出的一排按钮：放大 / 收成图标 / 还原成列 / 关闭。
 * 它**不是表面**（合成器自绘的矩形），所以命中得靠矩形判定 —— 位置记在
 * server 上（mini_toolbar_btn_box），绘制和命中用的是同一份，不会各算一套。
 *
 * 只读原则照 S2：不抢焦点、不进 MRU、不拿键盘 —— 它只是几个矩形。点中按钮是
 * 合成器自己的动作（vidow_mini_toolbar_press），不往客户端转任何东西。 */

/* 工具条摆在小窗上方（上面放不下就摆到下面），横向跟小窗左对齐，整体夹进工作区。 */
static struct wlr_box mini_toolbar_layout(struct vidow_server *server,
		struct vidow_view *view) {
	struct wlr_box work = vidow_work_area(server, view->output);
	struct wlr_box g = view->float_geom;
	int w = MINI_TOOLBAR_BTN_W * (int)VIDOW_MINI_BTN_COUNT +
		MINI_TOOLBAR_PAD * ((int)VIDOW_MINI_BTN_COUNT + 1);
	int h = MINI_TOOLBAR_BTN_H + MINI_TOOLBAR_PAD * 2;
	/* 横向**跟小窗居中对齐**（不是左对齐）：以前 x = g.x，迷你盒 353 宽、
	 * 工具条 151 宽时整条比窗口中心偏左 101px（用户报的「位置偏左」）。 */
	int x = g.x + (g.width - w) / 2;
	int y = g.y - h - 4;
	if (work.height > 0 && y < work.y) {
		y = g.y + g.height + 4; /* 上面放不下 → 摆到下面 */
	}
	if (work.width > 0) {
		if (x + w > work.x + work.width) {
			x = work.x + work.width - w;
		}
		if (x < work.x) {
			x = work.x;
		}
		if (y + h > work.y + work.height) {
			y = work.y + work.height - h;
		}
		if (y < work.y) {
			y = work.y;
		}
	}
	return (struct wlr_box){ .x = x, .y = y, .width = w, .height = h };
}

/* 把四个字形摆到按钮正中（每个亮点一个方块）。节点是 init 时照字模建好的
 * （见 vidow_mini_ui_init），顺序和这里遍历字模的顺序**必须**一致：两边都从
 * 0 数起、都是行优先，所以不需要额外存坐标表。 */
static void mini_toolbar_glyph_place(struct vidow_server *server) {
	int size = MINI_TOOLBAR_GLYPH_N * MINI_TOOLBAR_GLYPH_PX;
	for (int i = 0; i < (int)VIDOW_MINI_BTN_COUNT; i++) {
		const struct wlr_box *b = &server->mini_toolbar_btn_box[i];
		int ox = b->x + (b->width - size) / 2;
		int oy = b->y + (b->height - size) / 2;
		int g = 0;
		for (int r = 0; r < MINI_TOOLBAR_GLYPH_N; r++) {
			for (int c = 0; c < MINI_TOOLBAR_GLYPH_N; c++) {
				if (MINI_TOOLBAR_GLYPH[i][r][c] != '#') {
					continue;
				}
				if (g >= VIDOW_MINI_TOOLBAR_GLYPH_MAX) {
					continue;
				}
				struct wlr_scene_rect *node =
					server->mini_toolbar_glyph[i][g++];
				if (node == NULL) {
					continue;
				}
				wlr_scene_node_set_position(&node->node,
					ox + c * MINI_TOOLBAR_GLYPH_PX,
					oy + r * MINI_TOOLBAR_GLYPH_PX);
				wlr_scene_node_set_enabled(&node->node, true);
				raise_node(&node->node);
			}
		}
	}
}

void vidow_mini_toolbar_show(struct vidow_server *server,
		struct vidow_view *view) {
	if (server == NULL || view == NULL || server->mini_toolbar_bg == NULL) {
		return;
	}
	if (!server->config.mini_enabled || !server->config.mini_toolbar) {
		return;
	}
	struct wlr_box box = mini_toolbar_layout(server, view);
	server->mini_toolbar_box = box;
	server->mini_toolbar_view = view;
	for (int i = 0; i < (int)VIDOW_MINI_BTN_COUNT; i++) {
		int bx = box.x + MINI_TOOLBAR_PAD +
			i * (MINI_TOOLBAR_BTN_W + MINI_TOOLBAR_PAD);
		int by = box.y + MINI_TOOLBAR_PAD;
		server->mini_toolbar_btn_box[i] = (struct wlr_box){
			.x = bx, .y = by,
			.width = MINI_TOOLBAR_BTN_W,
			.height = MINI_TOOLBAR_BTN_H,
		};
	}
	wlr_scene_rect_set_size(server->mini_toolbar_bg, box.width, box.height);
	wlr_scene_node_set_position(&server->mini_toolbar_bg->node, box.x, box.y);
	wlr_scene_node_set_enabled(&server->mini_toolbar_bg->node, true);
	for (int i = 0; i < (int)VIDOW_MINI_BTN_COUNT; i++) {
		struct wlr_scene_rect *btn = server->mini_toolbar_btn[i];
		if (btn == NULL) {
			continue;
		}
		const struct wlr_box *b = &server->mini_toolbar_btn_box[i];
		wlr_scene_rect_set_size(btn, b->width, b->height);
		wlr_scene_node_set_position(&btn->node, b->x, b->y);
		wlr_scene_node_set_enabled(&btn->node, true);
		raise_node(&btn->node);
	}
	mini_toolbar_glyph_place(server);
	raise_node(&server->mini_toolbar_bg->node);
	/* 日志里把四个按钮的矩形说清楚（测试靠它注入点击；用户报「点不中」时也能
	 * 一眼看出工具条摆在哪儿）。 */
	wlr_log(WLR_INFO, "mini toolbar: %s 露出（整块 %d,%d %dx%d；放大 %d,%d "
		"收图标 %d,%d 还原成列 %d,%d 关闭 %d,%d）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		box.x, box.y, box.width, box.height,
		server->mini_toolbar_btn_box[0].x, server->mini_toolbar_btn_box[0].y,
		server->mini_toolbar_btn_box[1].x, server->mini_toolbar_btn_box[1].y,
		server->mini_toolbar_btn_box[2].x, server->mini_toolbar_btn_box[2].y,
		server->mini_toolbar_btn_box[3].x, server->mini_toolbar_btn_box[3].y);
}

void vidow_mini_toolbar_hide(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	server->mini_toolbar_view = NULL;
	if (server->mini_toolbar_bg != NULL) {
		wlr_scene_node_set_enabled(&server->mini_toolbar_bg->node, false);
	}
	for (int i = 0; i < (int)VIDOW_MINI_BTN_COUNT; i++) {
		if (server->mini_toolbar_btn[i] != NULL) {
			wlr_scene_node_set_enabled(
				&server->mini_toolbar_btn[i]->node, false);
		}
		for (int g = 0; g < VIDOW_MINI_TOOLBAR_GLYPH_MAX; g++) {
			if (server->mini_toolbar_glyph[i][g] != NULL) {
				wlr_scene_node_set_enabled(
					&server->mini_toolbar_glyph[i][g]->node, false);
			}
		}
	}
}

/* 光标在工具条**附近**吗（整块往外扩 margin 像素）。悬停路由用它「留住」工具条：
 * 工具条摆在小窗上方 4px（mini_toolbar_layout 里的 `g.y - h - 4`），指针从窗口往
 * 按钮上走**必须穿过那条缝** —— 只按按钮矩形判的话，缝里那一帧就把工具条收掉了，
 * 用户永远点不中（用户报的「整个工具条都不能交互」）。按下那一路仍然只认按钮矩形
 * （vidow_mini_toolbar_hit），不吃这条放宽。 */
bool vidow_mini_toolbar_near(struct vidow_server *server, double lx, double ly) {
	if (server == NULL || server->mini_toolbar_view == NULL) {
		return false;
	}
	const int margin = 12;
	struct wlr_box b = server->mini_toolbar_box;
	if (b.width <= 0 || b.height <= 0) {
		return false;
	}
	b.x -= margin;
	b.y -= margin;
	b.width += margin * 2;
	b.height += margin * 2;
	return wlr_box_contains_point(&b, lx, ly);
}

int vidow_mini_toolbar_hit(struct vidow_server *server,
		struct vidow_view *view, double lx, double ly) {
	if (server == NULL || view == NULL ||
			server->mini_toolbar_view != view) {
		return -1;
	}
	for (int i = 0; i < (int)VIDOW_MINI_BTN_COUNT; i++) {
		if (wlr_box_contains_point(&server->mini_toolbar_btn_box[i], lx, ly)) {
			return i;
		}
	}
	return -1;
}

bool vidow_mini_ui_init(struct vidow_server *server) {
	if (server == NULL || server->layer_toplevels == NULL) {
		return false;
	}
	server->mini_drop_zone = wlr_scene_rect_create(server->layer_toplevels,
		1, 1, (float[4]){ MINI_DROP_ZONE_COLOR[0], MINI_DROP_ZONE_COLOR[1],
			MINI_DROP_ZONE_COLOR[2], MINI_DROP_ZONE_COLOR[3] });
	server->mini_drop_preview = wlr_scene_rect_create(server->layer_toplevels,
		1, 1, (float[4]){ MINI_DROP_PREVIEW_COLOR[0],
			MINI_DROP_PREVIEW_COLOR[1], MINI_DROP_PREVIEW_COLOR[2],
			MINI_DROP_PREVIEW_COLOR[3] });
	server->mini_toolbar_bg = wlr_scene_rect_create(server->layer_toplevels,
		1, 1, (float[4]){ MINI_TOOLBAR_BG_COLOR[0], MINI_TOOLBAR_BG_COLOR[1],
			MINI_TOOLBAR_BG_COLOR[2], MINI_TOOLBAR_BG_COLOR[3] });
	if (server->mini_drop_zone == NULL || server->mini_drop_preview == NULL ||
			server->mini_toolbar_bg == NULL) {
		wlr_log(WLR_ERROR, "mini: 落点区 / 工具条的场景节点建不出来");
		return false;
	}
	for (int i = 0; i < (int)VIDOW_MINI_BTN_COUNT; i++) {
		server->mini_toolbar_btn[i] = wlr_scene_rect_create(
			server->layer_toplevels, 1, 1, (float[4]){
				MINI_TOOLBAR_BTN_COLOR[0], MINI_TOOLBAR_BTN_COLOR[1],
				MINI_TOOLBAR_BTN_COLOR[2], MINI_TOOLBAR_BTN_COLOR[3] });
		if (server->mini_toolbar_btn[i] == NULL) {
			wlr_log(WLR_ERROR, "mini: 工具条按钮建不出来");
			return false;
		}
		/* 按钮上的字形：照字模每个亮点建一个小方块（见上面那段说明）。
		 * 亮点数超过 VIDOW_MINI_TOOLBAR_GLYPH_MAX 就是字模画得太满 ——
		 * 当场报错，别悄悄少画几笔。 */
		int g = 0;
		for (int r = 0; r < MINI_TOOLBAR_GLYPH_N; r++) {
			for (int c = 0; c < MINI_TOOLBAR_GLYPH_N; c++) {
				if (MINI_TOOLBAR_GLYPH[i][r][c] != '#') {
					continue;
				}
				if (g >= VIDOW_MINI_TOOLBAR_GLYPH_MAX) {
					wlr_log(WLR_ERROR, "mini: 工具条字形 %d 的亮点超过 "
						"%d 个（改字模或加大 VIDOW_MINI_TOOLBAR_"
						"GLYPH_MAX）", i,
						VIDOW_MINI_TOOLBAR_GLYPH_MAX);
					return false;
				}
				struct wlr_scene_rect *node = wlr_scene_rect_create(
					server->layer_toplevels,
					MINI_TOOLBAR_GLYPH_PX,
					MINI_TOOLBAR_GLYPH_PX, (float[4]){
						MINI_TOOLBAR_GLYPH_COLOR[0],
						MINI_TOOLBAR_GLYPH_COLOR[1],
						MINI_TOOLBAR_GLYPH_COLOR[2],
						MINI_TOOLBAR_GLYPH_COLOR[3] });
				if (node == NULL) {
					wlr_log(WLR_ERROR, "mini: 工具条字形建不出来");
					return false;
				}
				wlr_scene_node_set_enabled(&node->node, false);
				server->mini_toolbar_glyph[i][g++] = node;
			}
		}
	}
	vidow_mini_drop_zone_hide(server);
	vidow_mini_toolbar_hide(server);
	return true;
}

void vidow_tile_grab_begin(struct vidow_server *server,
		struct vidow_view *view) {
	/* 会话锁（S3）：锁定期间不进拖拽 —— 拖的是被藏起来的窗口，而且拖拽
	 * grab 会把指针事件全吃掉（方案 §2 环节 3）。锁定瞬间已经在拖的由
	 * sessionlock.c 的 lock_cancel_input_gestures 收掉。 */
	if (server->locked) {
		return;
	}
	if (server == NULL || view == NULL || view->column == NULL ||
			view->ws == NULL || view->popup) {
		return;
	}
	server->grab_mode = VIDOW_GRAB_TILE;
	server->grab_view = view;
	/* 事件：拖动了（载荷是那个窗口）—— 和 resize 那条共用 drag-started /
	 * drag-ended，脚本不用分是「挪」还是「改宽」。 */
	vidow_ipc_emit_window(server, "drag-started", view);
	/* 平铺拖拽是 Mod+左键那条路：按下被合成器吃掉了，客户端没见过，松手也不
	 * 转给它（见 server.h 里 grab_client_press 的说明）。 */
	server->grab_client_press = false;
	server->grab_dirty = false;
	server->grab_last_box = vidow_view_screen_box(view);
	view->dragging = true;
	/* 果冻：把窗口现在显示的装饰结果挂到弹簧网格上。做不了就算了（装饰关着
	 * 之类），拖动照旧。 */
	vidow_jelly_begin(view);
	/* 光标在窗口里的相对位置：拖动时窗口保持这个「抓点」在光标下。 */
	view->drag_dx = (int)(server->cursor->x - view->geom.x);
	view->drag_dy = (int)(server->cursor->y - view->geom.y);
	if (view->scene_tree != NULL) {
		/* 飘起来的平铺窗口压住别的平铺窗口，浮窗照旧在它上面（见
		 * vidow_restack）。 */
		vidow_restack(server, view);
	}
	if (view != server->focused) {
		vidow_focus_view(view);
	}
	/* 落点预览块：起拖就摆出来（按现在这个抓点算），层级收一次口（块在所有窗口
	 * 之上、被拖的这个之下，见 vidow_insert_hint_update）。小窗（S4）的落点区
	 * 先算 —— 它亮着的话插入提示块要收掉。 */
	vidow_mini_drop_zone_update(server);
	vidow_insert_hint_update(server);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "grabbing");
	wlr_log(WLR_INFO, "grab tile: %s（%s 上第 %d 块工作区）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		view->ws->output != NULL ? view->ws->output->name : "-",
		vidow_workspace_index(view->ws));
}

void vidow_tile_grab_update(struct vidow_server *server, double lx, double ly) {
	/* 会话锁（S3）：锁定期间不进拖拽 —— 拖的是被藏起来的窗口，而且拖拽
	 * grab 会把指针事件全吃掉（方案 §2 环节 3）。锁定瞬间已经在拖的由
	 * sessionlock.c 的 lock_cancel_input_gestures 收掉。 */
	if (server->locked) {
		return;
	}
	struct vidow_view *view = server->grab_view;
	if (view == NULL || view->scene_tree == NULL) {
		return;
	}
	/* 只改「画在哪」：布局（geom）一个字都不动，所以别的窗口不会跟着抖。 */
	view->anim_off_x = (lx - view->drag_dx) - view->geom.x;
	view->anim_off_y = (ly - view->drag_dy) - view->geom.y;
	/* 真正重画 / 记损伤推迟到这一帧要出帧之前（见 output.c 里那段）：指针比刷新
	 * 快的时候一帧会来好几个移动事件，中间那些位置屏幕上从来没出现过 —— 以前
	 * 每个事件都重画一遍这个窗口的装饰（整块窗口那么大的离屏渲染），实测每个事件
	 * 66µs（1000Hz 鼠标 = 每帧 0.4ms，8000Hz = 每帧 3.3ms）。这里只记「还得摆一次」
	 * 并要一帧。 */
	server->grab_dirty = true;
	server->grab_ev_ns = vidow_now_ns(); /* 探针：量「事件 -> 出帧」延迟 */
	/* 位置立刻摆（便宜：场景节点一动就自带损伤）。要帧那一下是**幂等**的
	 * （wlr_output_schedule_frame 里有 frame_pending 挡着），所以一帧最多催出
	 * 一次渲染，不会变成「每个事件渲染一遍」。 */
	vidow_view_apply_anim_place(view);
	vidow_output_schedule_frame(server, view->output, "拖动跟手");
	/* 落点预览块跟着一起更新（很便宜：就是几个整形比较 + 摆一次节点）。
	 * 小窗（S4）的落点区同理 —— 顺序不能反：落点区先算，插入提示块才知道
	 * 要不要让位。 */
	vidow_mini_drop_zone_update(server);
	vidow_insert_hint_update(server);
	/* 贴边自动滚条带：光标压进工作区左右边缘那条带里就按秒速往那边推视口。
	 * 停着不动也要继续滚，所以这里只记「往哪边、多快」，推进在每帧
	 * （vidow_tile_drag_scroll_step，output.c 的渲染里调）。niri 的 Dnd 就是这个
	 * 手感：条带比屏宽时，屏幕外那些列（和它们之间的落点）光靠推光标永远够不到。 */
	vidow_tile_drag_autoscroll(server, lx);
	/* 顺手把保帧看门狗武装上：要帧那一下可能被「后端已经欠着一帧」挡掉
	 * （嵌套在别的合成器里时很常见），没有看门狗就没人再推了 —— 拖动会冻住。 */
	vidow_output_kick_watchdog(server);
}

/* 边缘带多宽（逻辑像素）：光标进到离工作区左右边这么近就开始滚。 */
#define DRAG_SCROLL_EDGE 40.0
/* 压在最边上时的速度（逻辑像素/秒）：条带一屏 1920 的话，最边上大约 1.2 秒滚
 * 过一屏 —— 够快，又不至于一下冲过头。 */
#define DRAG_SCROLL_MAX 1600.0

void vidow_tile_drag_autoscroll(struct vidow_server *server, double lx) {
	struct vidow_view *view = server->grab_view;
	if (view == NULL) {
		return;
	}
	/* 用**光标底下**那块屏（和落点判定同一个来源）：跨屏拖的时候窗口的
	 * view->output 还挂在老屏上，照它算就会去滚老屏的条带。光标不在任何屏上
	 * 才退回窗口自己那块。 */
	struct wlr_output *wlr_out = output_under_cursor(server);
	struct vidow_output *out = wlr_out != NULL ?
		vidow_output_lookup(server, wlr_out) : NULL;
	if (out == NULL) {
		out = vidow_output_lookup(server, view->output);
	}
	if (out == NULL) {
		return;
	}
	/* 只有光标底下这一块屏接管视口：跨屏拖时刚离开的那块要还回去（不然它的
	 * 视口就永久锁在拖拽手里了）。 */
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o != out && o->drag_scroll) {
			o->drag_scroll = false;
			o->drag_scroll_speed = 0.0;
			o->drag_scroll_ns = 0;
			server->arrange_dirty = true;
		}
	}
	/* 视口这期间归拖拽：不滚的时候也要接管（否则 arrange 的「把焦点列滚回可见
	 * 区」会在拖拽途中自己滚一下）。接管那一刻取当前视口当起点。 */
	if (!out->drag_scroll) {
		out->drag_scroll = true;
		out->drag_viewport = out->viewport_x;
	}
	struct wlr_box work = vidow_work_area(server, out->wlr_output);
	if (work.width < 1) {
		out->drag_scroll_speed = 0.0;
		return;
	}
	double lo = (double)work.x + DRAG_SCROLL_EDGE;
	double hi = (double)(work.x + work.width) - DRAG_SCROLL_EDGE;
	double speed = 0.0;
	if (lx < lo) {
		double t = (lo - lx) / DRAG_SCROLL_EDGE;
		if (t > 1.0) {
			t = 1.0;
		}
		speed = -t * DRAG_SCROLL_MAX;
	} else if (lx > hi) {
		double t = (lx - hi) / DRAG_SCROLL_EDGE;
		if (t > 1.0) {
			t = 1.0;
		}
		speed = t * DRAG_SCROLL_MAX;
	}
	out->drag_scroll_speed = speed;
}

bool vidow_tile_drag_scroll_step(struct vidow_server *server) {
	struct vidow_view *view = server->grab_view;
	bool active = server->grab_mode == VIDOW_GRAB_TILE && view != NULL;
	if (!active) {
		/* 拖拽没了（松手 / 窗口半路没了）：接管到此为止，视口交回 arrange。
		 * 顺手把所有屏清一遍 —— 这条自愈是为了「哪条退出路径漏了清」不会把
		 * 视口永久锁在拖拽手里（正常路径在 vidow_tile_grab_end 里清）。 */
		struct vidow_output *o;
		wl_list_for_each(o, &server->outputs, link) {
			if (o->drag_scroll) {
				o->drag_scroll = false;
				o->drag_scroll_speed = 0.0;
				o->drag_scroll_ns = 0;
				server->arrange_dirty = true;
			}
		}
		return false;
	}
	/* 谁在接管视口就推谁（跨屏拖的时候接管的那块未必是窗口的 view->output，
	 * 见 vidow_tile_drag_autoscroll）。同一时刻只有一块屏接管。 */
	struct vidow_output *out = NULL;
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->drag_scroll) {
			out = o;
			break;
		}
	}
	if (out == NULL) {
		return false;
	}
	int64_t now = vidow_now_ns();
	double dt = out->drag_scroll_ns > 0 ?
		(double)(now - out->drag_scroll_ns) / 1e9 : 0.0;
	out->drag_scroll_ns = now;
	if (out->drag_scroll_speed == 0.0 || dt <= 0.0) {
		return false;
	}
	if (dt > 0.1) {
		dt = 0.1;   /* 卡了一帧不要跳一大段 */
	}
	out->drag_viewport += out->drag_scroll_speed * dt;
	/* 视口一变，列的 screen_x 全变：重排一次（arrange 会把视口夹回
	 * [0, max_viewport]，所以滚到头自然停住），落点提示跟着重算。被拖的窗口
	 * 自己不用动 —— 它跟的是光标。 */
	vidow_arrange(server);
	vidow_insert_hint_update(server);
	/* arrange 会把视口夹回 [0, max_viewport]：把夹过的值抄回来。不抄的话滚到
	 * 头之后还接着累加，等光标移到另一边要先「空转」掉这一段才开始动。 */
	out->drag_viewport = out->viewport_x;
	vidow_output_schedule_frame(server, out->wlr_output, "拖拽贴边自动滚");
	return true;
}

void vidow_tile_grab_end(struct vidow_server *server) {
	struct vidow_view *view = server->grab_view;
	/* 还没摆出来的那个位置屏幕上没出现过，不用管；**已经画出来**的那个位置得记
	 * 一笔 —— 松手之后窗口就离开那儿了，别人底衬里那一块得重抓。 */
	if (view != NULL) {
		vidow_backdrop_damage_box(server, &server->grab_last_box);
	}
	/* 预览块收掉（落点算不算得出来都得收）。小窗（S4）落点区同理 —— 松手那一刻
	 * 光标在不在里面，先记下来再收（收掉之后 rect 就关了）。 */
	bool drop_zone = vidow_mini_drop_zone_hit(server, server->cursor->x,
		server->cursor->y);
	vidow_insert_hint_hide(server);
	vidow_mini_drop_zone_hide(server);
	if (view != NULL) {
		vidow_ipc_emit_window(server, "drag-ended", view);
	}
	server->grab_dirty = false;
	server->grab_mode = VIDOW_GRAB_NONE;
	server->grab_view = NULL;
	/* 贴边自动滚的接管到此为止：视口交回 arrange（按新的焦点列重算 = 滚到刚
	 * 落下的那个窗口）。放在下面那些提前返回之前 —— 哪条路走不通都不能把视口
	 * 留在拖拽手里。 */
	if (view != NULL) {
		struct vidow_output *out = vidow_output_lookup(server, view->output);
		if (out != NULL) {
			out->drag_scroll = false;
			out->drag_scroll_speed = 0.0;
			out->drag_scroll_ns = 0;
		}
	}
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
	/* 松手：解掉抓点，网格自己晃回去（位置照旧归下面的布局逻辑管）。
	 * 放在所有提前返回之前 —— 不管这条路走不走得通，果冻都得收手。 */
	vidow_jelly_release(view);
	if (view == NULL || view->column == NULL) {
		return;
	}
	view->dragging = false;
	view->anim_off_x = 0.0;
	view->anim_off_y = 0.0;

	/* 小窗（S4）主入口：松手时光标落在**落点区**里 → 变成迷你小窗（用户点名的
	 * 那条「拖到右上角松手 = 迷你小窗」，不是自由小窗）。
	 *
	 * 「只在拖拽中出现」这条规矩保证不会误触：光标不在落点区里时它根本没亮
	 * （见 vidow_mini_drop_zone_update），所以这里判的「在不在里面」和用户看到
	 * 的是同一件事 —— 盲拖到角落松手走的是下面的正常平铺落点。 */
	if (drop_zone) {
		wlr_log(WLR_INFO, "mini: 拖拽落进落点区 → %s 变迷你小窗",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "?");
		vidow_mini_set_state(view, VIDOW_MINI_MINI);
		return;
	}

	struct wlr_output *output = output_under_cursor(server);
	struct vidow_workspace *target = shown_workspace(server, output);
	if (target == NULL || view->ws == NULL) {
		/* 拖到没有屏的地方：原样放回去。 */
		vidow_arrange(server);
		return;
	}
	struct vidow_column *col = view->column;
	struct insert_pos pos;
	insert_pos_at(server, target, output, view, &pos);
	int from_idx = vidow_workspace_index(view->ws);

	if (pos.into_column) {
		/* 落进**某条列里**：一列里上下叠起来（niri 的 InColumn）。跨工作区 /
		 * 跨屏时搬的是窗口自己（niri 拖的一直是窗口），不是整条列 —— 原来那
		 * 条列里还有别人就留在原处。 */
		const char *title = vidow_view_title(view) != NULL ?
			vidow_view_title(view) : "?";
		int want = pos.index;
		if (pos.column->ws != view->ws) {
			if (!vidow_workspace_move_view_into_column(server, view,
					pos.column, want)) {
				vidow_arrange(server);
				return;
			}
			wlr_log(WLR_INFO,
				"grab: %s 落到 %s 上第 %d 块工作区的一条列里（第 %d 位）%s",
				title, output->name,
				vidow_workspace_index(pos.column->ws), want + 1,
				output != view->output ? "（跨屏）" : "");
		} else if (vidow_view_insert_into_column(view, pos.column, want)) {
			wlr_log(WLR_INFO, "grab: %s 插进 %s 的一条列里的第 %d 位",
				title, output->name, want + 1);
		} else {
			vidow_arrange(server);
			return;
		}
		vidow_focus_view(view);
		vidow_arrange(server);
		return;
	}

	int index = pos.index;
	if (index < 0) {
		index = 0;
	}
	/* 落点按「条带上还剩的那些列」数（被拖的那条不算、整列最小化的不算）——
	 * 和 insert_pos_at 数的是同一份。at = 新列该插在哪个位置**之前**。 */
	struct wl_list *at = &target->columns; /* 追加到末尾 */
	int i = 0;
	struct vidow_column *it;
	wl_list_for_each(it, &target->columns, link) {
		if (it == col || it->slot_width <= 0) {
			continue;
		}
		if (i == index) {
			at = &it->link;
			break;
		}
		i++;
	}

	/* 落点是「新开一条列」（niri 的 NewColumn）：这条窗口在一条**多窗口**的列里
	 * 的时候，把**它自己**取出来成新列（niri 拖的一直是窗口），原来那条列连同别的
	 * 窗口留在原处。用户报的「拖到右边 / 拖到空白处也取不出来」就是这儿 —— 以前
	 * 落点算成新列位就整条列搬走，窗口永远留在列里。
	 * 列里就它一个（或者它是浮窗）→ 取不出来，走老路：整条列搬过去。 */
	if (target == view->ws && vidow_view_expel_new_column(view, target, at,
			"拖出列") != NULL) {
		wlr_log(WLR_INFO,
			"grab: %s 从列里取出来 → %s 上第 %d 块工作区的第 %d 位",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			output->name, vidow_workspace_index(target), index + 1);
		vidow_focus_view(view);
		return; /* 取出来那条路自己 arrange 过了 */
	}

	if (target != view->ws) {
		/* 跨屏（或跨工作区）：整条列搬过去。 */
		if (!vidow_workspace_move_view(server, view, target)) {
			vidow_arrange(server);
			return;
		}
		wlr_log(WLR_INFO, "grab: %s 拖到 %s 上 → 归到第 %d 块工作区%s",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "?",
			output->name, vidow_workspace_index(target),
			output != view->output ? "（跨屏）" : "");
	} else {
		wlr_log(WLR_INFO, "grab: %s 在 %s 的第 %d 块工作区里挪到第 %d 位",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "?",
			output->name, from_idx, index + 1);
	}

	/* 按落点把整条列插进目标工作区的列带（跨屏那条路是搬过来再重排，这里统一
	 * 按上面算好的 at 插）。 */
	if (!wl_list_empty(&col->link)) {
		wl_list_remove(&col->link);
	}
	wl_list_init(&col->link);
	/* wl_list_insert(pos, elm) 是插在 pos 之后 —— 插到 at 前面要写 at->prev。 */
	wl_list_insert(at->prev, &col->link);

	vidow_focus_view(view);
	vidow_arrange(server);
}

/* --------------------------------------------- 平铺改宽（Mod+右键拖）
 *
 * niri 的 interactive resize 那条：按住 Mod 用右键拖平铺窗口，改的是**这一列的
 * 宽度**（列宽的右边缘跟着光标走）。拖的过程里实时改 width_fraction + 重排，窗口
 * 跟着手走。
 *
 * 为什么只横向：vidow 的平铺尺寸模型里窗口宽 = 列宽（比例分的是连 gap 一起算的
 * 那块区域，窗口 = 那块区域 - 一个 gap，见 server.h 的 vidow_column_width_px）、
 * 高永远是整块可用高 —— 没有「单个窗口的高度」这个概念（只有浮窗有自己的几何）。
 * 纵向拖动不改任何东西，松手也不留副作用。
 *
 * 改宽按**增量**算（新宽 = 起拖宽 + 光标位移），不是「右边缘 = 光标 x」：列一变宽
 * 视口就会跟着滚（focused 那列要整个可见），按绝对位置算的话下一帧光标底下的右边缘
 * 就跑了，手会抖。 */

void vidow_tile_resize_begin(struct vidow_server *server,
		struct vidow_view *view) {
	/* 会话锁（S3）：锁定期间不进拖拽 —— 拖的是被藏起来的窗口，而且拖拽
	 * grab 会把指针事件全吃掉（方案 §2 环节 3）。锁定瞬间已经在拖的由
	 * sessionlock.c 的 lock_cancel_input_gestures 收掉。 */
	if (server->locked) {
		return;
	}
	if (server == NULL || view == NULL || view->column == NULL ||
			view->popup || view->float_mode != VIDOW_FLOAT_NONE) {
		return;
	}
	struct wlr_box work = vidow_work_area(server, view->output);
	int width = view->column->width > 0 ? view->column->width :
		vidow_column_width_px(&server->config, work.width,
			view->column->width_fraction);
	if (width < 1) {
		return;
	}
	server->grab_mode = VIDOW_GRAB_RESIZE_TILE;
	server->grab_view = view;
	/* 按下被合成器吃掉了（没转给客户端），松手也一起吃掉。 */
	server->grab_client_press = false;
	server->grab_dirty = false;
	server->resize_from_x = server->cursor->x;
	server->resize_from_width = width;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "ew-resize");
	wlr_log(WLR_INFO, "grab resize: %s 起拖（列宽 %d 像素，比例 %.3f）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		width, view->column->width_fraction);
	/* 事件：起拖 / 松手（载荷是那个窗口）—— 录屏脚本靠它打「在拖窗口」的标记 */
	vidow_ipc_emit_window(server, "drag-started", view);
}

void vidow_tile_resize_update(struct vidow_server *server, double lx, double ly) {
	/* 会话锁（S3）：锁定期间不进拖拽 —— 拖的是被藏起来的窗口，而且拖拽
	 * grab 会把指针事件全吃掉（方案 §2 环节 3）。锁定瞬间已经在拖的由
	 * sessionlock.c 的 lock_cancel_input_gestures 收掉。 */
	if (server->locked) {
		return;
	}
	(void)ly;
	struct vidow_view *view = server->grab_view;
	struct vidow_column *col = view != NULL ? view->column : NULL;
	if (col == NULL || view->output == NULL) {
		return;
	}
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width <= 0) {
		return;
	}
	double want = (double)server->resize_from_width +
		(lx - server->resize_from_x);
	/* 像素 → 比例：和 arrange 里那条（vidow_column_width_px）互逆，所以拖到
	 * 哪儿右边缘就停在哪儿，不会因为 gap 而差一格。 */
	double fraction = vidow_column_fraction_of_px(&server->config, work.width,
		want);
	if (fraction < 0.05) {
		fraction = 0.05;
	}
	if (fraction > 1.0) {
		fraction = 1.0;
	}
	/* 动一下不到 0.2%（小于一个像素）就不重排：手停住的时候一个节点都不碰。 */
	double diff = fraction - col->width_fraction;
	if (diff < 0.002 && diff > -0.002) {
		return;
	}
	/* animate = false：拖动改宽必须跟手（重排也由它当场做掉，这里不再单独调一次
	 * —— 指针事件比帧密，每个事件两次重排就是白算一遍）。顺带把可能还在跑的
	 * 列宽过渡掐掉：不然过渡会接着往旧目标走，跟手的那一下被它拽回去。 */
	vidow_column_set_width(col, fraction, false);
	/* 手动给了宽度 = 退出「列最大化」这个状态（见 server.h 的 maximized）。 */
	col->maximized = false;
	/* 拖动时指针比刷新快：一帧里会来好几个移动事件，中间那些宽度屏幕上从来没
	 * 出现过。重排是便宜的（列宽变了本来就要重排），这里只要保证「要一帧」，
	 * 让这一帧把最终宽度画出来（同 vidow_tile_grab_update）。 */
	server->grab_dirty = true;
	vidow_output_schedule_frame(server, view->output, "改列宽跟手");
	vidow_output_kick_watchdog(server);
}

void vidow_tile_resize_end(struct vidow_server *server) {
	struct vidow_view *view = server->grab_view;
	struct vidow_column *col = view != NULL ? view->column : NULL;
	if (col != NULL) {
		wlr_log(WLR_INFO, "grab resize: %s 松手（列宽 %d 像素，比例 %.3f）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			col->width, col->width_fraction);
		vidow_arrange(server);
	}
	if (view != NULL) {
		vidow_ipc_emit_window(server, "drag-ended", view);
	}
	server->grab_mode = VIDOW_GRAB_NONE;
	server->grab_view = NULL;
	server->grab_dirty = false;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
}

/* ---------------------------------------------------------------- 平移视图
 *
 * 拖 CSD 标题栏 = **平移视图**，窗口不动（用户：「按住标题栏拖 → 视图平移、
 * 窗口不动」）。视口本来就有（arrange_columns 按 output->viewport_x 算列 x），
 * 所以这里只需要把光标位移喂进 viewport_x —— 不新建任何平移逻辑。 */
static double pan_cursor_x;
static double pan_base;
static struct vidow_output *pan_output;

void vidow_pan_begin(struct vidow_server *server, struct vidow_view *view) {
	pan_output = NULL;
	if (server == NULL || view == NULL || view->output == NULL) {
		return;
	}
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == view->output) {
			pan_output = o;
			break;
		}
	}
	if (pan_output == NULL) {
		return;
	}
	server->grab_mode = VIDOW_GRAB_PAN;
	server->grab_view = NULL;
	/* 这条和老的浮动抓取一样：按下是**转给客户端**的（它才发的 move 请求），
	 * 所以松手也必须转回去 —— 不然客户端以为左键一直按着，拖完再动鼠标窗口
	 * 还会跟着走，下一次点击被当成「按下」。 */
	server->grab_client_press = true;
	pan_cursor_x = server->cursor->x;
	pan_base = pan_output->viewport_x;
	wlr_log(WLR_DEBUG, "pan: 开始（viewport_x=%.0f）", pan_base);
}

void vidow_pan_motion(struct vidow_server *server) {
	if (pan_output == NULL || server == NULL) {
		return;
	}
	/* 往右拖 = 视图往左滑（手指拖纸的直觉，和触控板手势一致）。 */
	pan_output->viewport_x = pan_base - (server->cursor->x - pan_cursor_x);
	wlr_log(WLR_DEBUG, "pan: motion cursor=%.0f viewport_x=%.0f",
		server->cursor->x, pan_output->viewport_x);
	vidow_arrange(server);
}

void vidow_pan_end(struct vidow_server *server) {
	if (pan_output != NULL) {
		wlr_log(WLR_DEBUG, "pan: 结束（viewport_x=%.0f）",
			pan_output->viewport_x);
	}
	/* 松手要把指针**交还**给光标底下的客户端：平移期间我们没动 pointer focus，
	 * 但 move 请求那一下是客户端主动发的，收尾时补一个 enter，否则它可能一直
	 * 停在「被 grab」的状态里，后续 motion 收不到（test-titlebar-drag 的
	 * 「松手之后客户端一条指针位置都收不到」）。 */
	if (server != NULL) {
		double px = 0, py = 0;
		struct wlr_surface *psurf = NULL;
		vidow_view_at(server, server->cursor->x, server->cursor->y,
			&psurf, &px, &py);
		if (psurf != NULL) {
			wlr_seat_pointer_notify_enter(server->seat, psurf, px, py);
		}
	}
	if (server != NULL && server->grab_mode == VIDOW_GRAB_PAN) {
		server->grab_mode = VIDOW_GRAB_NONE;
		/* 和按下时配对的复位：不清的话松手之后客户端的指针位置还会一直被吃掉
		 * （test-titlebar-drag 的「松手之后客户端一条指针位置都收不到」）。 */
		server->grab_client_press = false;
	}
	pan_output = NULL;
}
