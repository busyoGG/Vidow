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
	/* 顺手把保帧看门狗武装上：要帧那一下可能被「后端已经欠着一帧」挡掉
	 * （嵌套在别的合成器里时很常见），没有看门狗就没人再推了 —— 拖动会冻住。 */
	vidow_output_kick_watchdog(server);
}

void vidow_tile_grab_end(struct vidow_server *server) {
	struct vidow_view *view = server->grab_view;
	/* 还没摆出来的那个位置屏幕上没出现过，不用管；**已经画出来**的那个位置得记
	 * 一笔 —— 松手之后窗口就离开那儿了，别人底衬里那一块得重抓。 */
	if (view != NULL) {
		vidow_backdrop_damage_box(server, &server->grab_last_box);
	}
	server->grab_dirty = false;
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

/* --------------------------------------------- 平铺改宽（Mod+右键拖）
 *
 * niri 的 interactive resize 那条：按住 Mod 用右键拖平铺窗口，改的是**这一列的
 * 宽度**（列宽的右边缘跟着光标走）。拖的过程里实时改 width_fraction + 重排，窗口
 * 跟着手走。
 *
 * 为什么只横向：vidow 的平铺尺寸模型里窗口宽 = 列宽（width_fraction × 可用宽）、
 * 高永远是整块可用高 —— 没有「单个窗口的高度」这个概念（只有浮窗有自己的几何）。
 * 纵向拖动不改任何东西，松手也不留副作用。
 *
 * 改宽按**增量**算（新宽 = 起拖宽 + 光标位移），不是「右边缘 = 光标 x」：列一变宽
 * 视口就会跟着滚（focused 那列要整个可见），按绝对位置算的话下一帧光标底下的右边缘
 * 就跑了，手会抖。 */

void vidow_tile_resize_begin(struct vidow_server *server,
		struct vidow_view *view) {
	if (server == NULL || view == NULL || view->column == NULL ||
			view->popup || view->float_mode != VIDOW_FLOAT_NONE) {
		return;
	}
	struct wlr_box work = vidow_work_area(server, view->output);
	int width = view->column->width > 0 ? view->column->width :
		(int)(work.width * view->column->width_fraction);
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
}

void vidow_tile_resize_update(struct vidow_server *server, double lx, double ly) {
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
	double fraction = want / (double)work.width;
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
	col->width_fraction = fraction;
	/* 手动给了宽度 = 退出「列最大化」这个状态（见 server.h 的 maximized）。 */
	col->maximized = false;
	vidow_arrange(server);
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
	server->grab_mode = VIDOW_GRAB_NONE;
	server->grab_view = NULL;
	server->grab_dirty = false;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
}
