#include "server.h"
#include "ipc.h"

#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

#define FLOAT_MIN_SIZE 64
#define FLOAT_MAX_DEPTH 8

/* ------------------------------------------------------------ detection */

static bool has_fixed_size(struct vidow_view *view) {
	const struct wlr_xdg_toplevel_state *st = &view->xdg_toplevel->current;
	return st->min_width > 0 && st->min_width == st->max_width &&
		st->min_height > 0 && st->min_height == st->max_height;
}

/* 父窗口按「根表面」找：xdg 给的是 toplevel，X11 给的是另一个 X 窗口，
 * 两边都只有一个表面能对应。 */
static struct vidow_view *resolve_parent(struct vidow_server *server,
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

/* Auto-detection:
 *  - a toplevel with a parent is a dialog for that window -> follow it
 *  - a toplevel that pins min == max is a fixed-size dialog -> float on its own
 *  - everything else is tiled
 */
static enum vidow_float_mode detect_mode(struct vidow_view *view) {
	if (view->kind == VIDOW_VIEW_X11) {
		return vidow_x11_detect_mode(view);
	}
	if (view->xdg_toplevel->parent != NULL) {
		return VIDOW_FLOAT_FOLLOW;
	}
	if (has_fixed_size(view)) {
		return VIDOW_FLOAT_FIXED;
	}
	return VIDOW_FLOAT_NONE;
}

/* --------------------------------------------------------------- modes */

void vidow_float_set_mode(struct vidow_view *view,
		enum vidow_float_mode mode) {
	struct vidow_server *server = view->server;

	/* 换模式（平铺 <-> 浮动）之后，「我要它多大」这件事重新交给客户端。 */
	view->float_want_w = 0;
	view->float_want_h = 0;

	if (mode == view->float_mode) {
		if (mode == VIDOW_FLOAT_FOLLOW) {
			/* the parent may have changed under us */
			struct vidow_view *parent =
				resolve_parent(server, vidow_view_parent_surface(view));
			if (parent != NULL) {
				view->parent = parent;
			}
		}
		return;
	}

	wlr_log(WLR_INFO, "float mode %s: %d -> %d",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		view->float_mode, mode);

	if (mode == VIDOW_FLOAT_NONE) {
		view->float_mode = VIDOW_FLOAT_NONE;
		view->parent = NULL;
		memset(&view->float_geom, 0, sizeof(view->float_geom));
		vidow_view_join_layout(server, view);
		vidow_arrange(server);
		vidow_ipc_emit_window(server, "window-floating-changed", view);
		return;
	}

	/* Seed the box from the current tile so a toggled window stays put until
	 * the client answers the configure. A window that has not been mapped yet
	 * has no tile: leave the box empty and let the first commit size it.
	 * 弹出物例外：位置尺寸由客户端给死，不需要种子，arrange 时直接读 X。 */
	if (view->popup) {
		memset(&view->float_geom, 0, sizeof(view->float_geom));
	} else if (view->mapped && view->geom.width > 0 && view->geom.height > 0) {
		view->float_geom = view->geom;
		/* the tile may be scrolled off-screen: pull the float back in */
		struct wlr_box work = vidow_work_area(server, view->output);
		if (work.width > 0 && work.height > 0) {
			if (view->float_geom.x < work.x ||
					view->float_geom.x + view->float_geom.width > work.x + work.width) {
				view->float_geom.x = work.x + (work.width - view->float_geom.width) / 2;
			}
			if (view->float_geom.y < work.y ||
					view->float_geom.y + view->float_geom.height > work.y + work.height) {
				view->float_geom.y = work.y + (work.height - view->float_geom.height) / 2;
			}
		}
	} else {
		memset(&view->float_geom, 0, sizeof(view->float_geom));
	}

	view->float_mode = mode;
	vidow_view_leave_layout(view);

	if (mode == VIDOW_FLOAT_FOLLOW) {
		view->parent = resolve_parent(server, vidow_view_parent_surface(view));
		if (view->parent == NULL) {
			/* unknown parent: fall back to an independent float */
			view->float_mode = VIDOW_FLOAT_FIXED;
		} else if (view->float_geom.width > 0) {
			/* turn the absolute box into an offset from the parent tile */
			view->float_geom.x -= view->parent->geom.x;
			view->float_geom.y -= view->parent->geom.y;
		}
	}

	/* Floating windows pick their own size (0x0 = client's choice). */
	vidow_view_set_size(view, 0, 0);
	vidow_arrange(server);
	vidow_ipc_emit_window(server, "window-floating-changed", view);
}

void vidow_float_update_mode(struct vidow_view *view) {
	vidow_float_set_mode(view, detect_mode(view));
}

/* ------------------------------------------- 运行时改浮动窗口（IPC 用）

 * 位置记的是「相对自己那块屏原点」的偏移（home_dx/home_dy），跟着屏走 ——
 * 所以改位置时要同时更新偏移和绝对盒，只改绝对盒会在下一次 arrange 时被
 * float_place_recursive() 用偏移覆盖回去。 */

void vidow_float_move_to(struct vidow_view *view, int x, int y) {
	if (view == NULL || view->float_mode == VIDOW_FLOAT_NONE) {
		return;
	}
	view->float_geom.x = x;
	view->float_geom.y = y;
	struct wlr_box box = {0};
	if (view->output != NULL) {
		wlr_output_layout_get_box(view->server->output_layout, view->output,
			&box);
	}
	if (box.width > 0) {
		view->home_dx = x - box.x;
		view->home_dy = y - box.y;
	}
	vidow_arrange(view->server);
}

void vidow_float_set_size(struct vidow_view *view, int w, int h) {
	if (view == NULL || view->float_mode == VIDOW_FLOAT_NONE) {
		return;
	}
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}
	view->float_geom.width = w;
	view->float_geom.height = h;
	/* 尺寸是客户端的事：先按它要的 configure 一次。记下「我要它多大」——
	 * 它要是先为别的原因提交一次旧尺寸，vidow_float_on_commit() 不能拿旧尺寸
	 * 把这盒改回去（那样这条命令会被一个迟到的提交吃掉）。 */
	view->float_want_w = w;
	view->float_want_h = h;
	vidow_view_set_size(view, w, h);
	vidow_arrange(view->server);
}

void vidow_float_center(struct vidow_view *view) {
	if (view == NULL || view->float_mode == VIDOW_FLOAT_NONE) {
		return;
	}
	struct wlr_box work = vidow_work_area(view->server, view->output);
	if (work.width < 1 || work.height < 1) {
		return;
	}
	int w = view->float_geom.width > 0 ? view->float_geom.width :
		view->geom.width;
	int h = view->float_geom.height > 0 ? view->float_geom.height :
		view->geom.height;
	vidow_float_move_to(view, work.x + (work.width - w) / 2,
		work.y + (work.height - h) / 2);
}

void vidow_float_toggle(struct vidow_view *view) {
	if (view->float_mode != VIDOW_FLOAT_NONE) {
		vidow_float_set_mode(view, VIDOW_FLOAT_NONE);
		return;
	}
	vidow_float_set_mode(view, vidow_view_parent_surface(view) != NULL ?
		VIDOW_FLOAT_FOLLOW : VIDOW_FLOAT_FIXED);
}

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

	/* 客户端自己要的尺寸，逻辑单位（X11 的像素在这里已经除掉了 xwayland_scale，
	 * 否则浮动窗口在 X11 下会大一倍）。 */
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
			int scale = view->server->xwayland_scale > 0 ?
				(int)lround(view->server->xwayland_scale) : 1;
			w = view->x11_want_w / scale;
			h = view->x11_want_h / scale;
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

static void float_place_recursive(struct vidow_view *view, int depth) {
	if (depth > FLOAT_MAX_DEPTH) {
		return;
	}

	if (view->float_mode == VIDOW_FLOAT_FOLLOW) {
		if (view->parent != NULL) {
			if (view->parent->float_mode != VIDOW_FLOAT_NONE) {
				/* make sure the parent has its current box first */
				float_place_recursive(view->parent, depth + 1);
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
		vidow_place_view(view, view->float_geom.x, view->float_geom.y,
			view->float_geom.width, view->float_geom.height);
		if (box.width > 0) {
			view->home_dx = view->float_geom.x - box.x;
			view->home_dy = view->float_geom.y - box.y;
		}
	}
}

void vidow_float_place_all(struct vidow_server *server,
		struct vidow_output *output) {
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
		/* 藏起来的工作区（或者正常屏上的特殊工作区）里的浮窗不摆：它的场景
		 * 节点是关着的，摆了也看不见，还会把「这块屏摆过什么」的账算乱。 */
		if (!vidow_workspace_view_visible(server, view)) {
			continue;
		}
		float_place_recursive(view, 0);
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
		struct vidow_view *view, bool resize) {
	server->grab_mode = resize ? VIDOW_GRAB_RESIZE : VIDOW_GRAB_MOVE;
	server->grab_view = view;
	server->grab_x = server->cursor->x;
	server->grab_y = server->cursor->y;
	server->grab_geom = view->float_geom;

	wlr_log(WLR_INFO, "grab %s: %s",
		resize ? "resize" : "move",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
		resize ? "se-resize" : "grabbing");
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

		if (view->float_mode == VIDOW_FLOAT_FIXED) {
			/* keep an independent float inside the work area */
			struct wlr_box work = vidow_work_area(server, view->output);
			if (work.width > 0 && work.height > 0) {
				if (x + server->grab_geom.width > work.x + work.width) {
					x = work.x + work.width - server->grab_geom.width;
				}
				if (y + server->grab_geom.height > work.y + work.height) {
					y = work.y + work.height - server->grab_geom.height;
				}
				if (x < work.x) {
					x = work.x;
				}
				if (y < work.y) {
					y = work.y;
				}
			}
		}

		view->float_geom.x = x;
		view->float_geom.y = y;
	} else {
		int w = server->grab_geom.width + dx;
		int h = server->grab_geom.height + dy;
		if (w < FLOAT_MIN_SIZE) {
			w = FLOAT_MIN_SIZE;
		}
		if (h < FLOAT_MIN_SIZE) {
			h = FLOAT_MIN_SIZE;
		}
		view->float_geom.width = w;
		view->float_geom.height = h;
	}

	vidow_arrange(server);
}

void vidow_float_end_grab(struct vidow_server *server) {
	if (server->grab_mode == VIDOW_GRAB_NONE) {
		return;
	}
	wlr_log(WLR_INFO, "grab end");
	server->grab_mode = VIDOW_GRAB_NONE;
	server->grab_view = NULL;
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
}
