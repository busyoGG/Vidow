/* 鼠标拖窗口（niri 的 interactive move）。
 *
 * 两种窗口、同一套 grab 状态（server->grab_mode / grab_view）：
 *
 *   浮动窗口 —— float.c 里的 vidow_float_begin_grab()：拖动改的是它自己的
 *               float_geom，松手时如果光标落在**另一块屏**上，窗口就改归那块屏
 *               显示着的工作区（跨屏拖拽）；
 *   平铺窗口 —— 这里的 vidow_tile_grab_*()：Mod+左键按住，窗口跟着光标飘起来
 *               （只动场景节点，布局一行都不重排），松手时按**落点**决定去哪：
 *               光标在哪块屏上就归那块屏的工作区，光标在列带上的哪个位置就插到
 *               那一列的位置（niri 的 insert hint 语义，没画提示条而已）。
 *
 * 为什么松手才改工作区：拖动中途改工作区要连带动可见性、焦点、底部空位和清理，
 * 每帧都做一遍既贵又容易把焦点抖掉；视觉上窗口本来就跟着光标走（场景是全局的），
 * 所以「跨过去」这件事松手时一次做完就够了。 */
#include "server.h"
#include "ipc.h"

#include <stdlib.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

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

/* 光标落在这块屏列带上的第几个位置（0 = 最左边那列之前）。列的位置是 arrange
 * 算出来的：offset 从**可用区**左边算起，所以加回可用区原点才是屏幕坐标。 */
static int insert_index_at(struct vidow_server *server,
		struct vidow_workspace *ws, struct wlr_output *output, double lx,
		int skip_id) {
	if (ws == NULL || output == NULL) {
		return 0;
	}
	struct wlr_box work = vidow_work_area(server, output);
	int index = 0;
	struct vidow_column *col;
	wl_list_for_each(col, &ws->columns, link) {
		if ((int)col->id == skip_id) {
			continue;
		}
		int center = work.x + col->offset + col->width / 2;
		if (lx < center) {
			break;
		}
		index++;
	}
	return index;
}

void vidow_tile_grab_begin(struct vidow_server *server,
		struct vidow_view *view) {
	if (server == NULL || view == NULL || view->column == NULL ||
			view->ws == NULL || view->popup) {
		return;
	}
	server->grab_mode = VIDOW_GRAB_TILE;
	server->grab_view = view;
	view->dragging = true;
	/* 果冻：把窗口现在显示的装饰结果挂到弹簧网格上。做不了就算了（装饰关着
	 * 之类），拖动照旧。 */
	vidow_jelly_begin(view);
	/* 光标在窗口里的相对位置：拖动时窗口保持这个「抓点」在光标下。 */
	view->drag_dx = (int)(server->cursor->x - view->geom.x);
	view->drag_dy = (int)(server->cursor->y - view->geom.y);
	if (view->scene_tree != NULL) {
		wlr_scene_node_raise_to_top(&view->scene_tree->node);
	}
	if (view != server->focused) {
		vidow_focus_view(view);
	}
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "grabbing");
	wlr_log(WLR_INFO, "grab tile: %s（%s 上第 %d 块工作区）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		view->ws->output != NULL ? view->ws->output->name : "-",
		vidow_workspace_index(view->ws));
}

void vidow_tile_grab_update(struct vidow_server *server, double lx, double ly) {
	struct vidow_view *view = server->grab_view;
	if (view == NULL || view->scene_tree == NULL) {
		return;
	}
	/* 只改「画在哪」：布局（geom）一个字都不动，所以别的窗口不会跟着抖。 */
	view->anim_off_x = (lx - view->drag_dx) - view->geom.x;
	view->anim_off_y = (ly - view->drag_dy) - view->geom.y;
	vidow_backdrop_damage(server, false);
	vidow_view_apply_anim(view);
}

void vidow_tile_grab_end(struct vidow_server *server) {
	struct vidow_view *view = server->grab_view;
	server->grab_mode = VIDOW_GRAB_NONE;
	server->grab_view = NULL;
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

	struct wlr_output *output = output_under_cursor(server);
	struct vidow_workspace *target = shown_workspace(server, output);
	if (target == NULL || view->ws == NULL) {
		/* 拖到没有屏的地方：原样放回去。 */
		vidow_arrange(server);
		return;
	}
	struct vidow_column *col = view->column;
	int index = insert_index_at(server, target, output, server->cursor->x,
		(int)col->id);
	int from_idx = vidow_workspace_index(view->ws);

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

	/* 按落点插进目标工作区的列带（跨屏那条路是追加到末尾的，这里统一重排）。 */
	if (index < 0) {
		index = 0;
	}
	struct wl_list *at = &target->columns; /* 追加到末尾 */
	int i = 0;
	struct vidow_column *it;
	wl_list_for_each(it, &target->columns, link) {
		if (it == col) {
			continue;
		}
		if (i == index) {
			at = &it->link;
			break;
		}
		i++;
	}
	if (!wl_list_empty(&col->link)) {
		wl_list_remove(&col->link);
	}
	wl_list_init(&col->link);
	/* wl_list_insert(pos, elm) 是插在 pos 之后 —— 插到 at 前面要写 at->prev。 */
	wl_list_insert(at->prev, &col->link);

	vidow_focus_view(view);
	vidow_arrange(server);
}
