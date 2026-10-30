#include "server.h"
#include "float-internal.h"
#include "ipc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

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

	/* X popup identity is independent of FOLLOW layout. Reused CEF widgets
	 * can lose/change WM_TRANSIENT_FOR while staying FIXED; never keep a stale
	 * hidden parent which would suppress the popup's entire scene tree. */
	if (view->popup && view->kind == VIDOW_VIEW_X11) {
		view->parent = resolve_parent(server, vidow_view_parent_surface(view));
	}

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
		/* 浮窗变回平铺：先把工作区的浮窗账摘掉（否则一块只剩浮窗账的工作区
		 * 永远收不掉，ws_has_something 也会误以为还有东西）。 */
		if (!view->popup) {
			vidow_workspace_track(view->ws, view, false);
		}
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
		/* 位置是「相对自己那块屏」记的（见本文件顶上那段注释）：种子也要一起
		 * 给 home_dx/home_dy，不然第一次摆放就被 float_place_recursive() 按
		 * 0 偏移覆盖成屏幕左上角 —— 平铺转浮动会「跳」到角落。 */
		struct wlr_box out_box = {0};
		if (view->output != NULL) {
			wlr_output_layout_get_box(server->output_layout, view->output,
				&out_box);
		}
		if (out_box.width > 0) {
			view->home_dx = view->float_geom.x - out_box.x;
			view->home_dy = view->float_geom.y - out_box.y;
		}
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
		/* 夹回来之后必须把 home_dx/home_dy 一起刷新。上面那两行只改了
		 * float_geom，偏移还是「平铺时那一列」的旧值；下一次 arrange 的
		 * float_place_recursive() 又按旧偏移摆回去 —— 夹了等于没夹。
		 * 微信登录窗就是这么整块停在屏幕右边缘外（x=1917，屏宽 1920）：
		 * mapped / visible 都是 true，用户却看不到窗口。 */
		if (out_box.width > 0) {
			view->home_dx = view->float_geom.x - out_box.x;
			view->home_dy = view->float_geom.y - out_box.y;
		}
	} else {
		memset(&view->float_geom, 0, sizeof(view->float_geom));
	}

	view->float_mode = mode;
	vidow_view_leave_layout(view);
	/* 平铺 -> 浮动（含规则在 map 之前就设好的那条）：记上工作区的浮窗账。
	 * 必须在 vidow_view_leave_layout() **之后**：vidow_workspace_track() 看到
	 * view->column 还在就直接返回（列里的窗口不算浮窗），记账就漏了。 */
	if (!view->popup) {
		vidow_workspace_track(view->ws, view, true);
	}

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
	/* 刚变成浮动的那个摆到浮窗组最上面：它正是用户刚动过的那个。不抬的话它可能
	 * 停在「平铺时候」的位置上（平铺的那些节点本来就在浮窗下面，它从下面翻上来
	 * 之后是组里最低的那个），看起来就像「切成浮动了但还是被别的浮窗压着」。
	 * arrange 里的收口只保证「浮动在平铺之上」，组内的先后得在这里点一下。 */
	vidow_restack(server, view);
	vidow_ipc_emit_window(server, "window-floating-changed", view);
}

/* 自动识别成“固定尺寸对话框”时，把它摆到工作区正中。以前这里沿用了
 * view->geom（平铺时那一列的左上角），所以登录窗 / 对话框一浮起来就贴在
 * 左上；客户端是 xdg，根本不能自己请求绝对坐标。 */
static void float_center_detected_dialog(struct vidow_view *view) {
	if (view == NULL || view->popup || !view->mapped ||
			view->float_mode != VIDOW_FLOAT_FIXED) {
		return;
	}
	int w = 0, h = 0;
	if (!vidow_view_fixed_size_hint(view, &w, &h) || w <= 1 || h <= 1) {
		return;
	}
	view->float_geom.width = w;
	view->float_geom.height = h;
	vidow_float_center(view);
}

void vidow_float_update_mode(struct vidow_view *view) {
	/* 全屏的窗口**不接受自动识别**（规则和 Mod+V 那两条显式的路照旧）。
	 *
	 * 为什么非要有这条：自动识别看的是客户端声明的 min/max 尺寸提示
	 * （detect_mode → vidow_x11_detect_mode 的 x11_has_fixed_size），而**全屏的
	 * 尺寸是我们定的**（整块输出）。proton 游戏进全屏时正好把提示钉成
	 * min == max（等于它全屏的尺寸），离开全屏又松开 —— 而 view.c 里那串
	 * x11_set_* 回调（WM_HINTS / WM_NORMAL_HINTS / _NET_WM_STATE）每次属性变动
	 * 都重判一遍模式，于是同一个全屏游戏在「平铺 <-> 浮动」之间来回翻
	 * （用户日志里翻了 175 次）。
	 *
	 * 翻成浮动的代价是两样一起丢：
	 *   ① 它那条列（vidow_view_leave_layout）→ 条带塌一格，整屏跟着跳；
	 *   ② 「停到屏外 / 裁到自己那块屏」两道闸（vidow_view_should_park 与
	 *      view_clip_box 对浮动窗口一律放行，因为浮窗本来就允许跨屏）——
	 *      它以条带上的坐标整块画出来。1920 宽的全屏窗口在 1920 那块屏上滚出
	 *      视口时，x 正好落在 0 / 963 这种跨两块屏的位置上，于是「突然从左边
	 *      平移到右边，且跨显示器了」。
	 * 列就是列：全屏窗口照旧在条带里跟着滚、滚出去就停到屏外。
	 *
	 * 这里**保持现状**（不硬掰成平铺）：显式浮动起来的窗口（Mod+V / 规则）再
	 * 全屏还得是浮动的 —— test-fullscreen-scroll.sh 里那条「浮动的全屏窗口也
	 * 盖住整块屏」就是它。 */
	if (view->fullscreen && !view->rule_eff.set[VRA_FLOAT]) {
		return;
	}
	/* 窗口规则说了算（float = true / false）：自动识别让位。尺寸提示晚到
	 * （dialogs 在第一次 configure 之后才发 min/max）的时候也不能把它推翻。 */
	bool was_none = view->float_mode == VIDOW_FLOAT_NONE;
	if (view->rule_eff.set[VRA_FLOAT]) {
		vidow_float_set_mode(view, view->rule_eff.val[VRA_FLOAT].b ?
			VIDOW_FLOAT_FIXED : VIDOW_FLOAT_NONE);
		if (was_none) {
			float_center_detected_dialog(view);
		}
		return;
	}
	vidow_float_set_mode(view, detect_mode(view));
	if (was_none) {
		float_center_detected_dialog(view);
	}
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
	/* 变浮窗之前先退出全屏：全屏在层级上永远压着所有窗口（平铺 < 浮动 < 全屏，
	 * 见 vidow_restack），留着它「浮起来」也还是压在最上面 —— 用户报的「这个窗口
	 * 一直叠在最上面，不管平铺还是浮动」就是这么来的：它本来是全屏状态，Mod+V
	 * 只换了浮动标志，全屏标志还在。 */
	vidow_view_unfullscreen(view, "变浮窗");
	vidow_float_set_mode(view, vidow_view_parent_surface(view) != NULL ?
		VIDOW_FLOAT_FOLLOW : VIDOW_FLOAT_FIXED);
}
