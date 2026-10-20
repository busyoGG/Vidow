#include "server.h"
#include "ipc.h"

#include <math.h>
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
			int scale = vidow_view_x11_scale_int(view);
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
	vidow_output_schedule_frame(server, view->output, "拖浮动窗口");
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
	struct vidow_workspace *target =
		vidow_workspace_shown_for(server, under);
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

/* -------------------------------------------------- 甩出（mini fling，S5）
 *
 * 用户要的「像安卓那样惯性甩出」：把迷你小窗快速甩向边缘 → 飞过去收成图标，
 * 不用慢慢拖到底。三条设计决定：
 *
 *  1) **速度按最近 ~100ms 的采样算**（8 格采样环，vidow_pointer_motion 每帧推
 *     一格），不是单帧增量 —— 单帧增量在高刷新率下小得量不出意图、在低刷新率
 *     下抖得离谱。至少 3 个点、且最后一点离松手不超过 80ms 才算数（指针停住
 *     一会儿再松手不是甩）。
 *  2) **方向取最近的那条边**（±45° 容差：横向速度占优就取左 / 右边）。落点
 *     判据沿用已定的那条：那条边外侧有相邻输出 → 换屏保持形态；没有 → 收成
 *     图标贴那条边。
 *  3) **不做真物理**：时长由速度推（越快越短，夹在 110ms..mini_transition_ms），
 *     位置曲线照旧 out_cubic（单调、**不过冲**）—— 边缘是吸附目标，不是自由
 *     物体的落点，越界再弹回来是错的。
 */
void vidow_mini_fling_sample(struct vidow_server *server, double x, double y) {
	if (server == NULL) {
		return;
	}
	for (int i = 1; i < 8; i++) {
		server->mini_fling_pts[i - 1] = server->mini_fling_pts[i];
	}
	server->mini_fling_pts[7].x = x;
	server->mini_fling_pts[7].y = y;
	server->mini_fling_pts[7].ns = vidow_now_ns();
	if (server->mini_fling_n < 8) {
		server->mini_fling_n++;
	}
}

bool vidow_mini_fling_velocity(struct vidow_server *server, double *vx,
		double *vy) {
	/* 至少 2 个点：快速一甩（144Hz 下 8 格采样环只覆盖 ~55ms）常常只有 2-3 个
	 * 点，要求 3 个 + dt ≥ 20ms 会让最快的那些甩动直接「没有速度」。 */
	if (server == NULL || server->mini_fling_n < 2) {
		return false;
	}
	int n = server->mini_fling_n;
	/* 采样环永远是「最新的那一格在 [7]、往前依次更早」：有效的是
	 * [8-n .. 7]，别拿 n-1 当末尾（那是「环形缓冲从头读」的写法，这里不是）。 */
	int last = 7;
	int oldest = 8 - n;
	if (vidow_now_ns() - server->mini_fling_pts[last].ns > 80000000LL) {
		return false;
	}
	int first = last;
	for (int i = last; i >= oldest; i--) {
		if (server->mini_fling_pts[last].ns -
				server->mini_fling_pts[i].ns > 100000000LL) {
			break;
		}
		first = i;
	}
	int64_t dt = server->mini_fling_pts[last].ns -
		server->mini_fling_pts[first].ns;
	if (dt < 8000000LL) {
		return false;
	}
	double secs = (double)dt / 1e9;
	*vx = (server->mini_fling_pts[last].x -
		server->mini_fling_pts[first].x) / secs;
	*vy = (server->mini_fling_pts[last].y -
		server->mini_fling_pts[first].y) / secs;
	return true;
}

/* 速度方向最近的那条边（±45° 容差）。 */
static enum vidow_mini_edge fling_edge(double vx, double vy) {
	double ax = fabs(vx), ay = fabs(vy);
	if (ax == 0.0 && ay == 0.0) {
		return VIDOW_MINI_EDGE_NONE;
	}
	if (ax >= ay) {
		return vx > 0.0 ? VIDOW_MINI_EDGE_RIGHT : VIDOW_MINI_EDGE_LEFT;
	}
	return vy > 0.0 ? VIDOW_MINI_EDGE_BOTTOM : VIDOW_MINI_EDGE_TOP;
}

bool vidow_mini_fling(struct vidow_server *server, struct vidow_view *view,
		double vx, double vy) {
	if (server == NULL || view == NULL || view->popup ||
			view->mini == VIDOW_MINI_NONE) {
		return false;
	}
	int threshold = server->config.mini_fling_speed;
	if (threshold <= 0) {
		return false;
	}
	double speed = hypot(vx, vy);
	if (speed < (double)threshold) {
		return false;
	}
	enum vidow_mini_edge edge = fling_edge(vx, vy);
	if (edge == VIDOW_MINI_EDGE_NONE) {
		return false;
	}
	/* 时长：越快越短。180000/speed 在 800px/s（阈值）时是 225ms、1600px/s 时
	 * 是 112ms；夹在 [110, mini_transition_ms] —— 再快也得看得见那一下，
	 * 再慢也不比普通转换更久（它是「甩」，不是「慢慢飞」）。 */
	int ms = (int)lround(180000.0 / speed);
	int cap = server->config.mini_transition_ms;
	if (cap <= 0) {
		cap = 260;
	}
	if (ms > cap) {
		ms = cap;
	}
	if (ms < 110) {
		ms = 110;
	}

	struct wlr_output *across =
		vidow_mini_output_across(server, view->output, edge);
	if (across != NULL) {
		/* 跨屏边：换屏、保持形态（锚点是比例，位置天然对得上）。 */
		wlr_log(WLR_INFO, "mini fling: %s 以 %.0f px/s 甩向 %s 边（外侧是 %s）"
			"→ 换屏保持 %s，%dms",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			speed,
			edge == VIDOW_MINI_EDGE_LEFT ? "左" :
				edge == VIDOW_MINI_EDGE_RIGHT ? "右" :
				edge == VIDOW_MINI_EDGE_TOP ? "上" : "下",
			across->name, vidow_mini_state_name(view->mini), ms);
		server->mini_transition_override_ms = ms;
		vidow_mini_move_to_output(server, view, across);
		return true;
	}

	/* 内边缘：收成图标，贴这条边 —— 沿边的那一轴按窗口**现在**的位置记锚点
	 * （甩出去的那一下不该把图标挪到屏幕另一头）。 */
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width > 0 && work.height > 0) {
		int iw = server->config.mini_icon_size;
		double span_x = (double)(work.width - iw);
		double span_y = (double)(work.height - iw);
		double cx = view->float_geom.x + view->float_geom.width / 2.0;
		double cy = view->float_geom.y + view->float_geom.height / 2.0;
		if (edge == VIDOW_MINI_EDGE_LEFT || edge == VIDOW_MINI_EDGE_RIGHT) {
			view->mini_ax = edge == VIDOW_MINI_EDGE_LEFT ? 0.0 : 1.0;
			view->mini_ay = span_y > 1.0 ?
				(cy - iw / 2.0 - (double)work.y) / span_y : 0.0;
		} else {
			view->mini_ay = edge == VIDOW_MINI_EDGE_TOP ? 0.0 : 1.0;
			view->mini_ax = span_x > 1.0 ?
				(cx - iw / 2.0 - (double)work.x) / span_x : 0.0;
		}
		if (view->mini_ax < 0.0) {
			view->mini_ax = 0.0;
		} else if (view->mini_ax > 1.0) {
			view->mini_ax = 1.0;
		}
		if (view->mini_ay < 0.0) {
			view->mini_ay = 0.0;
		} else if (view->mini_ay > 1.0) {
			view->mini_ay = 1.0;
		}
	}
	view->mini_edge = edge;
	wlr_log(WLR_INFO, "mini fling: %s 以 %.0f px/s 甩向 %s 边（内边缘）→ 收成"
		"图标，%dms",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		speed,
		edge == VIDOW_MINI_EDGE_LEFT ? "左" :
			edge == VIDOW_MINI_EDGE_RIGHT ? "右" :
			edge == VIDOW_MINI_EDGE_TOP ? "上" : "下",
		ms);
	server->mini_transition_override_ms = ms;
	vidow_mini_set_state(view, VIDOW_MINI_ICON);
	return true;
}

/* 跨屏边松手：换屏、保持形态。锚点是「相对那块屏工作区的比例」，所以位置和
 * 尺寸一个字都不用换算（vidow_mini_resolve_geom 会按新屏的工作区算出来）——
 * 这就是当初选比例锚点而不是绝对坐标的理由。 */
void vidow_mini_move_to_output(struct vidow_server *server,
		struct vidow_view *view, struct wlr_output *to) {
	if (server == NULL || view == NULL || to == NULL ||
			view->output == to) {
		return;
	}
	struct wlr_output *from = view->output;
	view->output = to;
	/* 跟屏的记账（热插拔按输出名认）：换屏就是换这块 memory。 */
	struct vidow_output *out = vidow_output_lookup(server, to);
	view->mini_home = out != NULL ? out->memory : NULL;
	/* 迷你 / 图标态的位置不靠 home_dx/home_dy（那是给普通浮窗的），FREE 才是
	 * 普通浮窗的摆放路径 —— 顺手把它刷新一下，免得以后从 FREE 转出来时用旧屏
	 * 的偏移摆一次（拖了等于没拖那个坑）。 */
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, to, &box);
	if (box.width > 0) {
		view->home_dx = view->float_geom.x - box.x;
		view->home_dy = view->float_geom.y - box.y;
	}
	vidow_mini_resolve_geom(view);
	vidow_arrange(server);
	wlr_log(WLR_INFO, "mini: %s 从 %s 挪到 %s（形态 %s 不变，锚点 %.3f,%.3f）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		from != NULL ? from->name : "-", to->name,
		vidow_mini_state_name(view->mini), view->mini_ax, view->mini_ay);
	vidow_ipc_emit_window(server, "window-mini-changed", view);
}

/* 工具条按钮的动作。只读原则照 S2：这些动作是合成器自己的形态操作，不往客户端
 * 转任何事件（工具条本身也不是表面）。 */
void vidow_mini_toolbar_press(struct vidow_server *server,
		struct vidow_view *view, int button) {
	if (server == NULL || view == NULL) {
		return;
	}
	switch (button) {
	case VIDOW_MINI_BTN_EXPAND:
		/* 放大一档：图标 → 迷你 → 自由（自由是最上面那一档）。 */
		if (view->mini == VIDOW_MINI_ICON) {
			vidow_mini_set_state(view, VIDOW_MINI_MINI);
		} else if (view->mini == VIDOW_MINI_MINI) {
			vidow_mini_set_state(view, VIDOW_MINI_FREE);
			vidow_focus_view(view);
		}
		break;
	case VIDOW_MINI_BTN_ICON:
		vidow_mini_set_state(view, VIDOW_MINI_ICON);
		break;
	case VIDOW_MINI_BTN_RESTORE:
		vidow_mini_set_state(view, VIDOW_MINI_NONE);
		break;
	case VIDOW_MINI_BTN_CLOSE:
		vidow_view_close(view);
		break;
	default:
		return;
	}
	wlr_log(WLR_INFO, "mini toolbar: %s 点了按钮 %d（现在 %s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		button, vidow_mini_state_name(view->mini));
}

/* ------------------------------------------------- 小窗（mini window，S1）
 *
 * 数据模型见 server.h 的 enum vidow_mini_state 顶上那段。这里只有三件事：
 *   1) 形态字段的读写（vidow_mini_set_state）
 *   2) 比例锚点 -> float_geom 的换算（vidow_mini_resolve_geom）
 *   3) 进 / 出小窗态时的账：从工作区摘出来（view->ws = NULL）并把那块工作区
 *      的 floats 减掉 —— 不减的话一块只剩小窗的工作区永远收不掉
 *      （ws_keeps/ws_empty 看 ws->floats，见 workspace.c）。
 *
 * S1 只做数据模型 + 配置 + IPC：可见性旁路、焦点守卫、输入路由、渲染与动画
 * 一律还没动（S2–S5）。所以小窗态现在会被现有的可见性判据当成「工作区不亮着」
 * 而停到屏外 —— 这是预期的，S2 的旁路一上来它就留在屏上了。
 */

/* S3：小窗态要不要保留客户端原生分辨率（默认 true）。规则 > 全局键，
 * 和 mini_aspect 那套三级优先级无关 —— 这个只有两级（规则写了就是它）。
 *
 * **只管 MINI / ICON**（缩略图语义：保留原生 buffer，合成器缩着画，便宜且
 * 「内容比例不能变」正是它们要的）。FREE 恒为 false：它是自由摆放的正常窗口，
 * 盒子多大就 configure 客户端多大（客户端跟着重新排版）—— 保留缓冲那条路上，
 * 改盒子只改留白、内容比例一动不动，正是用户报的「改大小被限制了比例」。 */
bool vidow_mini_preserve_buffer(const struct vidow_view *view) {
	if (view == NULL) {
		return true;
	}
	if (view->mini == VIDOW_MINI_FREE) {
		return false;
	}
	if (view->mini_preserve_buffer_set) {
		return view->mini_preserve_buffer;
	}
	return view->server->config.mini_preserve_buffer;
}

const char *vidow_mini_state_name(enum vidow_mini_state state) {
	switch (state) {
	case VIDOW_MINI_FREE:
		return "free";
	case VIDOW_MINI_MINI:
		return "mini";
	case VIDOW_MINI_ICON:
		return "icon";
	case VIDOW_MINI_NONE:
		break;
	}
	return "none";
}

bool vidow_mini_parse_state(const char *text, enum vidow_mini_state *out) {
	if (text == NULL) {
		return false;
	}
	if (strcmp(text, "none") == 0) {
		*out = VIDOW_MINI_NONE;
	} else if (strcmp(text, "free") == 0) {
		*out = VIDOW_MINI_FREE;
	} else if (strcmp(text, "mini") == 0) {
		*out = VIDOW_MINI_MINI;
	} else if (strcmp(text, "icon") == 0) {
		*out = VIDOW_MINI_ICON;
	} else {
		return false;
	}
	return true;
}

const char *vidow_mini_aspect_name(enum vidow_mini_aspect aspect) {
	switch (aspect) {
	case VIDOW_MINI_ASPECT_AUTO:
		return "auto";
	case VIDOW_MINI_ASPECT_LOCK:
		return "lock";
	case VIDOW_MINI_ASPECT_FREE:
		return "free";
	case VIDOW_MINI_ASPECT_UNSET:
		break;
	}
	return "unset";
}

bool vidow_mini_parse_aspect(const char *text, enum vidow_mini_aspect *out) {
	if (text == NULL) {
		return false;
	}
	if (strcmp(text, "auto") == 0) {
		*out = VIDOW_MINI_ASPECT_AUTO;
	} else if (strcmp(text, "lock") == 0) {
		*out = VIDOW_MINI_ASPECT_LOCK;
	} else if (strcmp(text, "free") == 0) {
		*out = VIDOW_MINI_ASPECT_FREE;
	} else {
		return false;
	}
	return true;
}

/* ------------------------------------------- 宽高比（mini window，S3b，§11.3）
 *
 * 三级显式优先级：键位（运行时）> 窗口规则 > 全局键。**三个取值都是显式选择**
 * —— `free` 就是真的不锁（压过客户端的 PAspect），`auto` 才是「我选择按自动
 * 信号走」。判策略只有下面这一个出口，别在拖拽 / 摆放 / IPC 里各判一份。 */

/* 内容比例（w/h）。优先场景里的 1:1 尺寸（base_dst_*，X11 那条已经除过
 * x11_scale），没有就取表面自己的逻辑尺寸 —— 两者是同一个比例。**不要**直接
 * 拿 surface->current.width 混 X11 的缩放桥（计划 §11.3 那条）。 */
static bool mini_native_ratio(const struct vidow_view *view, double *ratio) {
	int nw = view->base_dst_w, nh = view->base_dst_h;
	/* FREE 的「内容比例」就是**当前**表面比例：FREE 不保留原生 buffer（盒子多大
	 * 就 configure 客户端多大），base_dst_* 那份 1:1 基线只在小窗态之外记（见
	 * view.c 的 view_dst_capture），进 FREE 之后它就是过期的 —— 拿它当内容比例
	 * 会把锁比例算错（实测锁着改窄：客户端被 configure 成 404 宽、高按过期的
	 * 0.89 算成 452，而表面自己的比例是 1.53）。 */
	if (view->mini == VIDOW_MINI_FREE || nw <= 0 || nh <= 0) {
		struct wlr_surface *surface = vidow_view_surface(view);
		if (surface != NULL && surface->current.width > 0 &&
				surface->current.height > 0) {
			nw = surface->current.width;
			nh = surface->current.height;
		}
	}
	if (nw <= 0 || nh <= 0) {
		return false;
	}
	*ratio = (double)nw / (double)nh;
	return true;
}

enum vidow_mini_aspect vidow_mini_aspect_effective(const struct vidow_view *view) {
	if (view == NULL) {
		return VIDOW_MINI_ASPECT_AUTO;
	}
	if (view->mini_aspect_runtime != VIDOW_MINI_ASPECT_UNSET) {
		return view->mini_aspect_runtime;
	}
	if (view->mini_aspect_rule != VIDOW_MINI_ASPECT_UNSET) {
		return view->mini_aspect_rule;
	}
	/* 全局键：配置面已经校验过取值，读不出来就当 auto（不静默退回别的值）。 */
	enum vidow_mini_aspect global = VIDOW_MINI_ASPECT_AUTO;
	vidow_mini_parse_aspect(view->server->config.mini_aspect, &global);
	return global;
}

bool vidow_mini_aspect_lock(const struct vidow_view *view, double *ratio) {
	/* 迷你 / 图标态永远按内容比例显示（contain），这条不受 mini_aspect 影响
	 * —— 那是「预览能不能被认出来」的呈现职责，不是用户偏好。 */
	if (view == NULL || view->mini != VIDOW_MINI_FREE) {
		return false;
	}
	switch (vidow_mini_aspect_effective(view)) {
	case VIDOW_MINI_ASPECT_FREE:
		return false;
	case VIDOW_MINI_ASPECT_LOCK: {
		/* 强制锁：没有 PAspect 也锁，比值取内容自己的比例。 */
		double r = 0.0;
		if (!mini_native_ratio(view, &r)) {
			return false;
		}
		if (ratio != NULL) {
			*ratio = r;
		}
		return true;
	}
	case VIDOW_MINI_ASPECT_AUTO:
	case VIDOW_MINI_ASPECT_UNSET:
		break;
	}
	/* auto：让自动信号决定。X11 的 PAspect 最权威（客户端自己声明的），其次
	 * min==max 的尺寸提示（固定尺寸窗口拖不动，锁比例是它的自然语义）。
	 * Wayland 协议没有比例提示 → 两条都不命中 → free。 */
	int num = 0, den = 0;
	if (vidow_view_aspect_hint(view, &num, &den) && num > 0 && den > 0) {
		if (ratio != NULL) {
			*ratio = (double)num / (double)den;
		}
		return true;
	}
	int fw = 0, fh = 0;
	if (vidow_view_fixed_size_hint(view, &fw, &fh) && fw > 0 && fh > 0) {
		if (ratio != NULL) {
			*ratio = (double)fw / (double)fh;
		}
		return true;
	}
	return false;
}

void vidow_mini_aspect_changed(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	/* 只有自由小窗在乎：迷你 / 图标态的比例跟这个键无关。 */
	if (view->mini != VIDOW_MINI_FREE) {
		return;
	}
	double ratio = 0.0;
	bool lock = vidow_mini_aspect_lock(view, &ratio);
	int num = 0, den = 0;
	bool hinted = vidow_view_aspect_hint(view, &num, &den);
	wlr_log(WLR_INFO, "mini: %s 比例信号变了（PAspect %s，有效值 %s → %s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		hinted ? "有" : "没有",
		vidow_mini_aspect_name(vidow_mini_aspect_effective(view)),
		lock ? "锁" : "不锁");
	if (!lock || ratio <= 0.0 || view->float_geom.width <= 0 ||
			view->float_geom.height <= 0) {
		return;
	}
	/* 锁着但盒子还不是那个比例（客户端**映射之后**才设 PAspect 的常见情况：
	 * 进小窗那一刻还没有信号，盒子是配置的初始比例）→ 按宽度重排盒子，让它
	 * 当场变成锁定的形状。宽度驱动，和拖拽那条一致。 */
	double want = (double)view->float_geom.width / ratio;
	if (fabs(want - (double)view->float_geom.height) < 1.0) {
		return;
	}
	if (want < FLOAT_MIN_SIZE) {
		want = FLOAT_MIN_SIZE;
	}
	view->float_geom.height = (int)lround(want);
	view->mini_hf = view->float_geom.height /
		(double)vidow_work_area(view->server, view->output).height;
	vidow_float_set_size(view, view->float_geom.width, view->float_geom.height);
	vidow_arrange(view->server);
}

/* 配置里的百分比键（"28%"）-> 比例（0.28）。读不出来就用兜底值 ——
 * 写配置那一刻已经校验过（config.c 的 K_STRING 分支），这里不该再报错。 */
static double mini_frac(struct vidow_server *server, const char *key,
		double fallback) {
	const char *text = vidow_config_get(&server->config, key);
	double pct = 0.0;
	if (text != NULL && text[0] != '\0' &&
			vidow_config_parse_percent(text, &pct)) {
		return pct / 100.0;
	}
	return fallback;
}

/* 进各态的默认锚点：自由小窗居中，迷你 / 图标贴右上角（落点区的默认角）。
 * 只在「本来不在小窗态」时给一次，三态互切不动它（位置是用户拖出来的）。 */
static void mini_default_anchor(struct vidow_view *view,
		enum vidow_mini_state state) {
	switch (state) {
	case VIDOW_MINI_FREE:
		view->mini_ax = 0.5;
		view->mini_ay = 0.5;
		break;
	case VIDOW_MINI_MINI:
		view->mini_ax = 1.0;
		view->mini_ay = 0.0;
		break;
	case VIDOW_MINI_ICON:
		view->mini_ax = 1.0;
		view->mini_ay = 0.0;
		view->mini_edge = VIDOW_MINI_EDGE_RIGHT;
		break;
	case VIDOW_MINI_NONE:
		break;
	}
}

void vidow_mini_resolve_geom(struct vidow_view *view) {
	if (view == NULL || view->mini == VIDOW_MINI_NONE || view->popup) {
		return;
	}
	struct vidow_server *server = view->server;
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width < 1 || work.height < 1) {
		/* 没有屏（嵌套会话的外层窗口关了 / 最后一块屏拔了）：没得算，留着
		 * 上一次的盒子。屏回来时 arrange 会再算一遍。 */
		return;
	}
	const struct vidow_config *cfg = &server->config;

	int w = 0, h = 0;
	switch (view->mini) {
	case VIDOW_MINI_FREE:
		if (!view->mini_size_known) {
			view->mini_wf = mini_frac(server, "mini_free_w", 0.40);
			view->mini_hf = mini_frac(server, "mini_free_h", 0.45);
		}
		w = (int)lround(view->mini_wf * (double)work.width);
		h = (int)lround(view->mini_hf * (double)work.height);
		break;
	case VIDOW_MINI_MINI:
		w = (int)lround(mini_frac(server, "mini_w", 0.28) *
			(double)work.width);
		h = (int)lround(mini_frac(server, "mini_h", 0.28) *
			(double)work.height);
		break;
	case VIDOW_MINI_ICON:
		w = h = cfg->mini_icon_size;
		break;
	case VIDOW_MINI_NONE:
		return;
	}
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

	/* 锚点 -> 绝对位置：0 = 贴工作区左 / 上边，1 = 贴右 / 下边。图标允许
	 * 超出 0..1（夹在 -0.5..1.5），好让它按 mini_icon_overflow 出屏一点。 */
	/* 锚点 -> 绝对位置：0 = 贴工作区左 / 上边，1 = 贴右 / 下边。
	 * FREE 允许越界（拖动里出屏半个盒子，盒子接近满屏时 w/(2*span) 能到 0.5，
	 * 所以兜底夹 -0.5..1.5）；MINI / ICON **夹在 0..1** —— 它们本来就该在工作区
	 * 里，图标那点出屏是下面 mini_icon_overflow 额外加的偏移。以前 MINI / ICON
	 * 也照 -0.5..1.5 兜底 = 允许半个工作区在外面：迷你盒整个能被拖出屏幕。 */
	double ax = view->mini_ax, ay = view->mini_ay;
	/* 迷你态和自由态**同一套夹取**（-0.5..1.5）：用户要的就是「两者一样」——
	 * 他原话「迷你态在工作区有左右死区，无法拖拽过去，自由态可以拖过去」。上一轮
	 * 我把迷你态单独夹在 0..1（当时是为了修视频里「盒子整个拖出屏、两秒看不见」），
	 * 结果正好造出他报的左右死区 —— 迷你态再也推不到工作区边上、更推不出去。
	 * 图标态走它自己的出屏偏移（mini_icon_overflow），同样不夹死。 */
	double lo = -0.5, hi = 1.5;
	if (ax < lo) {
		ax = lo;
	} else if (ax > hi) {
		ax = hi;
	}
	if (ay < -0.5) {
		ay = -0.5;
	} else if (ay > 1.5) {
		ay = 1.5;
	}
	int x = work.x + (int)lround(ax * (double)(work.width - w));
	int y = work.y + (int)lround(ay * (double)(work.height - h));

	if (view->mini == VIDOW_MINI_ICON) {
		double ov = cfg->mini_icon_overflow;
		if (ov < 0.0) {
			ov = 0.0;
		} else if (ov > 0.6) {
			ov = 0.6;
		}
		int dx = (int)lround(ov * (double)w);
		int dy = (int)lround(ov * (double)h);
		switch (view->mini_edge) {
		case VIDOW_MINI_EDGE_LEFT:
			x -= dx;
			break;
		case VIDOW_MINI_EDGE_RIGHT:
			x += dx;
			break;
		case VIDOW_MINI_EDGE_TOP:
			y -= dy;
			break;
		case VIDOW_MINI_EDGE_BOTTOM:
			y += dy;
			break;
		case VIDOW_MINI_EDGE_NONE:
			break;
		}
	} else {
		/* 边距**只给迷你态**：它是「贴边的小窗」，留一点缝好看。
		 * 自由小窗（FREE）和普通浮窗（NONE）都不吃 —— 它们能贴到工作区边上。
		 *
		 * 用户报的「迷你态和自由态一样的啊，现在还是左右有死区」就是这里：以前
		 * 只有 FREE 被特判成 0，MINI 和 NONE 都拿 mini_edge_margin（8px）—— 于是
		 * 「迷你态」和「普通浮窗/自由态」在左右各留 8px 进不去，一模一样。普通
		 * 浮窗更冤：它不是小窗家族的东西，凭什么吃小窗的边距。 */
		/* 内缩 = 0：**迷你态和自由态一模一样**（用户原话：「迷你态在工作区有
		 * 左右死区，无法拖拽过去，自由态可以拖过去」）。死区实测正好 8px = 旧
		 * 的 cfg->mini_edge_margin，两态都吃它 —— 这就是他说的「两者不一样 /
		 * 左右有死区」。mini_edge_margin 这个配置项留着（旧配置里有它，别报
		 * 未知键），但不再参与位置计算。 */
		int margin = 0;
		if (margin < 0) {
			margin = 0;
		}
		/* **迷你态和自由态一样**能出屏半个盒子（用户：「迷你态在工作区有左右
		 * 死区，无法拖拽过去，自由态可以拖过去」+「死区显然不止 8px」）。这里
		 * 以前只有 FREE 拿 w/2、h/2，MINI 是 0 —— 兜底夹回工作区就等于「工作区
		 * 左右那两片进不去」：盒子左边缘最多停在 work.x，离屏边还差整整半个
		 * 盒子。8px 那点只是它的尾巴（mini_edge_margin），大头是这儿。 */
		int out_x = w / 2;
		int out_y = h / 2;
		if (x < work.x + margin - out_x) {
			x = work.x + margin - out_x;
		}
		if (y < work.y + margin - out_y) {
			y = work.y + margin - out_y;
		}
		if (x + w > work.x + work.width - margin + out_x) {
			x = work.x + work.width - margin + out_x - w;
		}
		if (y + h > work.y + work.height - margin + out_y) {
			y = work.y + work.height - margin + out_y - h;
		}
	}

	view->float_geom.x = x;
	view->float_geom.y = y;
	view->float_geom.width = w;
	view->float_geom.height = h;
}

/* ------------------------------------------ 形态转换的过渡动画（S5，§1.4）
 *
 * 两条硬规矩，写在这儿当注释钉住：
 *
 *  1) **起点严格取「上一帧画在哪」**：`geom + anim_off`，和 view.c 的
 *     `vidow_place_view()` 里算 drawn_x/drawn_y 的那两行**完全同源**，而且
 *     必须在**改几何之前**取（下面 set_state 第一行就是它）。起点存成
 *     「起点 − 目标」的常量差值，update() 里**绝不重读 view->geom** ——
 *     转换中途夹着 arrange，geom 会被改写，重读 = 起点漂移。
 *     `daeba31` 的教训（计划 §1.4 引了原文）：那次用「起点落在输出框里就推到
 *     框的一侧」这种推断，正常滚动时也命中 → 每滚一列窗口都从屏幕外飞入。
 *     这里**不做任何推断**：起点是什么就是什么，哪怕它在屏外。
 *
 *  2) **位置曲线一律单调**（out_cubic），`out_back` 只用在**缩放**上 ——
 *     「中间帧位置单调」因此是构造性保证，测试不用给过冲开例外。
 *
 * 位置 / 缩放都走现成的 anim_off_x/anim_off_y 与 anim_scale（view_anim_place
 * 每帧读它们，fx.c 按窗口中心缩放），不新造第二条摆放管线。
 */
static const struct vidow_ease MINI_POS_EASE = {
	.kind = VIDOW_EASE_OUT_CUBIC,
};

static void mini_anim_apply(struct vidow_view *view, double t, double ts) {
	/* t = 位置进度（单调曲线），ts = 缩放进度（可能过冲）。 */
	view->anim_off_x = view->mini_from_dx * (1.0 - t);
	view->anim_off_y = view->mini_from_dy * (1.0 - t);
	double s = 1.0 + (view->mini_scale_from - 1.0) * (1.0 - ts);
	if (s < 0.01) {
		s = 0.01;
	}
	view->anim_scale = s;
	vidow_view_apply_anim(view);
	wlr_log(WLR_DEBUG, "mini anim: %s t=%.3f 画在 %d,%d（偏移 %.1f,%.1f）缩放 %.3f",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		t, view->geom.x + (int)lround(view->anim_off_x),
		view->geom.y + (int)lround(view->anim_off_y),
		view->anim_off_x, view->anim_off_y, view->anim_scale);
}

static void mini_anim_update(void *data, double t, double linear) {
	struct vidow_view *view = data;
	mini_anim_apply(view, t,
		vidow_ease_value(&view->mini_scale_ease, linear));
}

static void mini_anim_done(void *data, bool cancelled) {
	struct vidow_view *view = data;
	view->mini_anim = NULL;
	if (!cancelled) {
		/* 收尾：偏移归零、缩放归 1 —— 不归零的话窗口会永远画在差一点的地方。 */
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		view->anim_scale = 1.0;
	}
	vidow_view_apply_anim(view);
}

/* 形态转换的收口（set_state 的最后一步）：起点 `from` 是**改几何之前**取的
 * 「上一帧画在哪」，view->geom 这时已经是这一趟 arrange 摆出来的目标盒。
 *
 * `from_scale` 是同一时刻的「上一帧内容的缩放」（set_state 里在改之前问
 * vidow_view_mini_scale），**不是**从盒子面积推出来的那个 —— 见下面 scale_from
 * 的注释：推出来的那一个只在「内容正好填满盒子」时才等于屏上真正画的那个。 */
static void mini_transition_start(struct vidow_view *view,
		const struct wlr_box *from, double from_scale) {
	struct vidow_server *server = view->server;
	/* 上一趟还在飞就被打断了：先掐掉（cancelled 那条不清偏移 —— 新的这条
	 * 紧接着会把偏移重写成新起点，中途不清才不会闪）。 */
	vidow_anim_cancel(view->mini_anim);
	view->mini_anim = NULL;
	/* arrange 里 vidow_place_view 起的那条普通位移补间让位（它用的是
	 * window_move_ms 和全局曲线，也不动缩放）。 */
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;

	int ms = server->config.mini_transition_ms;
	/* 甩出：时长由速度推出来（越快越短），一次性的覆盖，读完就清 —— 不覆盖
	 * 配置键，也不留在下一次转换上。 */
	if (server->mini_transition_override_ms > 0) {
		ms = server->mini_transition_override_ms;
		server->mini_transition_override_ms = 0;
	}
	double dx = (double)(from->x - view->geom.x);
	double dy = (double)(from->y - view->geom.y);
	/* 起点缩放：view_anim_place 每帧画出来的是 **ms(新状态) × anim_scale**，
	 * 所以 anim_scale 的起点要取「上一帧的内容缩放 ÷ 新状态的 contain 系数」——
	 * 这样 t = 0 那一帧画出来的内容尺寸和上一帧**逐像素一样**（daeba31 那条
	 * 教训：补间起点必须是「上一帧画在哪」，不能是推出来的量）。
	 *
	 * 以前这里取 sqrt(盒面积比)：只在「内容正好填满盒子」时才等于屏上真正画的
	 * 那个（`sqrt` 是面积比的几何平均，撑不起 contain 的 min）。实测的偏差：
	 * 自由小窗（盒 504x315、内容 625x700 原生、contain 0.4257）还原成列
	 * （目标 625x700）时起点取到 0.602 → 退出动画第一帧的内容比上一帧大
	 * **41%**（用户报的「退出小窗的一瞬间显示原始大小」）；图标态那趟同理
	 * （0.097 vs 真正的 0.0865）。 */
	double to_scale = vidow_view_mini_scale_export(view);
	double scale_from = to_scale > 0.0 ? from_scale / to_scale : 1.0;
	double area_from = (double)from->width * (double)from->height;
	double area_to = (double)view->geom.width * (double)view->geom.height;
	if (ms <= 0 || (dx == 0.0 && dy == 0.0 &&
			fabs(scale_from - 1.0) < 0.01)) {
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		view->anim_scale = 1.0;
		vidow_view_apply_anim(view);
		return;
	}

	view->mini_from_dx = dx;
	view->mini_from_dy = dy;
	view->mini_scale_from = scale_from;
	/* 缩放曲线：**变大**（展开）给一点回弹（out_back，幅度取全局
	 * back_overshoot 反解出来的常数）；变小用 out_cubic 收住。位置永远是
	 * out_cubic —— 见本节顶上第 2 条。 */
	bool expanding = area_from < area_to;
	if (expanding) {
		double c1 = vidow_ease_out_back_c1(server->config.back_overshoot);
		view->mini_scale_ease = (struct vidow_ease){
			.kind = VIDOW_EASE_OUT_BACK, .back_c1 = c1,
		};
	} else {
		view->mini_scale_ease = (struct vidow_ease){
			.kind = VIDOW_EASE_OUT_CUBIC,
		};
	}

	/* 先把 t = 0 那一帧摆出来（第一帧就必须画在起点，不能等下一次 tick ——
	 * 判据 19 查的就是这个）。 */
	mini_anim_apply(view, 0.0, 0.0);
	view->mini_anim = vidow_anim_start(server, "mini-transition", ms,
		MINI_POS_EASE, mini_anim_update, mini_anim_done, view);
	if (view->mini_anim == NULL) {
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		view->anim_scale = 1.0;
		vidow_view_apply_anim(view);
		return;
	}
	wlr_log(WLR_INFO, "mini: 过渡动画 %s → 起点 %d,%d（%.1f,%.1f 偏移）→ "
		"目标 %d,%d %dx%d，缩放 %.3f → 1，%dms",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		from->x, from->y, dx, dy,
		view->geom.x, view->geom.y, view->geom.width, view->geom.height,
		scale_from, ms);
}

void vidow_mini_set_state(struct vidow_view *view, enum vidow_mini_state state) {
	if (view == NULL || view->popup) {
		return;
	}
	struct vidow_server *server = view->server;
	enum vidow_mini_state old = view->mini;
	if (old == state) {
		return;
	}
	/* 过渡动画的起点：**改几何之前**的「上一帧画在哪」（§1.4 第 1 条）。
	 * 对小窗态来说「画在哪」是**内容**的原点，不是盒子的原点 —— 内容在盒子里
	 * 是 contain 居中摆的（S3b，见 view.c 的 mini_cx/mini_cy），盒子原点到
	 * 内容原点还差一个居中偏移。不把它算进来的话，退出小窗的第一帧会横着跳
	 * 一下（实测自由小窗那趟差 119px，比动画本身一步的位移还大）。 */
	int content_dx = 0, content_dy = 0;
	vidow_view_mini_content_offset(view, &content_dx, &content_dy);
	double from_scale = vidow_view_mini_scale_export(view);
	struct wlr_box from = {
		.x = view->geom.x + (int)lround(view->anim_off_x) + content_dx,
		.y = view->geom.y + (int)lround(view->anim_off_y) + content_dy,
		.width = view->geom.width,
		.height = view->geom.height,
	};

	/* S2：迷你 / 图标**不接键盘**，所以进这两个态之前先把焦点交出去（规矩照抄
	 * 最小化那条：位置不动，谁滑到空出来的位置就是谁）。必须在下面
	 * vidow_float_set_mode() 摘列**之前** —— focus_after_close() 要靠窗口还挂在
	 * 列里才找得到邻居。FREE 照常拿焦点，不用交。 */
	if (state == VIDOW_MINI_MINI || state == VIDOW_MINI_ICON) {
		vidow_view_yield_focus_for_mini(view);
	}
	/* 小窗（S4）：形态一变，悬停工具条就收掉（它的位置是照**上一个**形态的
	 * 盒子算的，留着会画在一个已经不在那儿的窗口旁边）。下一次指针动会重新
	 * 露出来。 */
	vidow_mini_toolbar_hide(server);
	/* 小窗（S5）：形态一变，悬停预览也收掉（预览画的是**上一个**形态旁边的
	 * 那块，留着会停在已经不在那儿的图标旁边）。 */
	vidow_mini_preview_hide(server);
	/* 键位动作用的指认（迷你 / 图标态不拿键盘焦点，server->focused 指不到）。 */
	if (state != VIDOW_MINI_NONE) {
		server->mini_last_id = view->id;
	}

	if (state == VIDOW_MINI_NONE) {
		view->mini = VIDOW_MINI_NONE;
		view->mini_edge = VIDOW_MINI_EDGE_NONE;
		/* S3b：键位翻转是「这一次小窗」的运行时选择，还原成列就忘掉 —— 下次
		 * 进小窗重新按 规则 > 全局键 判。 */
		view->mini_aspect_runtime = VIDOW_MINI_ASPECT_UNSET;
		/* 还原成列。小窗本来不属于任何工作区，先认领回自己那块屏显示着的
		 * 工作区（屏没了就留 NULL，view_ensure_column 会安置到一块无屏的
		 * 工作区上，屏回来时跟着认领回来）。浮窗记账不减 —— 窗口这就回列了，
		 * 列里的窗口本来就不算 ws->floats（见 vidow_workspace_track）。 */
		if (view->output != NULL) {
			view->ws = vidow_workspace_shown_for(server, view->output);
		}
		vidow_float_set_mode(view, VIDOW_FLOAT_NONE);
		/* S5：接回条带也走过渡动画（目标盒是 join_layout 算出来的条带位置，
		 * 上面那次 arrange 已经把它摆进 geom 了）。 */
		mini_transition_start(view, &from, from_scale);
		wlr_log(WLR_INFO, "mini: %s 还原成列（从 %s）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			vidow_mini_state_name(old));
		vidow_ipc_emit_window(server, "window-mini-changed", view);
		return;
	}

	/* 进小窗态 / 三态互切。小窗是浮动窗口上的一个形态：先把浮动这件事办齐
	 * （float_mode = FIXED，从条带摘走、种子 float_geom），再谈形态。 */
	bool was_float = view->float_mode != VIDOW_FLOAT_NONE;
	if (view->float_mode != VIDOW_FLOAT_FIXED) {
		vidow_view_unfullscreen(view, "进小窗");
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	if (old == VIDOW_MINI_NONE) {
		mini_default_anchor(view, state);
		/* 小窗是**屏级对象**：从工作区摘出来。这一步的账必须减 ——
		 * ws_keeps() = named || !ws_empty(ws)，而 ws_empty() 看 ws->floats，
		 * 不减的话一块只剩小窗的工作区永远收不掉（vidow_workspace_cleanup
		 * 一直跳过它），在 vidowmsg workspaces 和 overview 里永远多一行。
		 *
		 * **但只有本来就占着这笔账的那个才减**：ws->floats 的 +1 只在
		 * view_map()（src/view.c，`float_mode != NONE` 那条）与 ws_adopt_view()
		 * 里记（vidow_view_leave_column() 自己不动 floats —— 平铺窗口的账在
		 * columns 上）。平铺窗口进小窗时摘掉那条列已经够了，这里再减一下就是
		 * 减**别人**的账：同一块工作区里那个真浮窗的计数会被吃掉（实测：
		 * 浮窗 miniwin + 平铺 tilewin 进小窗，floats 1 → 0，miniwin 的账没了）。 */
		view->mini_home = NULL;
		if (view->output != NULL) {
			struct vidow_output *out =
				vidow_output_lookup(server, view->output);
			view->mini_home = out != NULL ? out->memory : NULL;
		}
		struct vidow_workspace *ws = view->ws;
		view->ws = NULL;
		if (ws != NULL && was_float) {
			vidow_workspace_track(ws, view, false);
		}
	}
	if (state == VIDOW_MINI_FREE) {
		/* FREE 的尺寸：记过就接着用（mini_remember_size 关掉 = 每次都用
		 * 配置的初始比例）。MINI / ICON 不记 —— 尺寸永远跟配置。 */
		if (!view->mini_size_known || !server->config.mini_remember_size) {
			view->mini_wf = mini_frac(server, "mini_free_w", 0.40);
			view->mini_hf = mini_frac(server, "mini_free_h", 0.45);
			view->mini_size_known = true;
		}
	}
	view->mini = state;

	vidow_mini_resolve_geom(view);
	if (view->float_geom.width > 0 && view->float_geom.height > 0) {
		/* 尺寸是客户端的事：configure 一次，并把 float_want_w/h 钉住
		 * （见 vidow_float_set_size 的说明）。 */
		vidow_float_set_size(view, view->float_geom.width,
			view->float_geom.height);
	}
	vidow_arrange(server);
	wlr_log(WLR_INFO, "mini: %s %s -> %s，%dx%d @ %d,%d（锚点 %.3f,%.3f，屏 %s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		vidow_mini_state_name(old), vidow_mini_state_name(state),
		view->float_geom.width, view->float_geom.height,
		view->float_geom.x, view->float_geom.y,
		view->mini_ax, view->mini_ay,
		view->output != NULL ? view->output->name : "-");
	if (state == VIDOW_MINI_FREE) {
		/* S3b：把「这一级算出来的宽高比策略」说清楚 —— 用户报「设了 PAspect 的
		 * 游戏还是锁着 / 没锁」时，这一行能直接看出是哪一级（键位 / 规则 /
		 * 全局键）说了算、自动信号有没有被读到。 */
		int anum = 0, aden = 0;
		double aratio = 0.0;
		bool alock = vidow_mini_aspect_lock(view, &aratio);
		bool ahint = vidow_view_aspect_hint(view, &anum, &aden);
		wlr_log(WLR_INFO, "mini: %s 宽高比 有效值=%s（键位 %s / 规则 %s / 全局 %s），"
			"PAspect=%s，min==max=%s → %s（比值 %.4f）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			vidow_mini_aspect_name(vidow_mini_aspect_effective(view)),
			vidow_mini_aspect_name(view->mini_aspect_runtime),
			vidow_mini_aspect_name(view->mini_aspect_rule),
			server->config.mini_aspect,
			ahint ? "有" : "没有",
			vidow_view_fixed_size_hint(view, &anum, &aden) ? "有" : "没有",
			alock ? "锁" : "不锁", aratio);
	}
	/* S5：图标态的出帧策略（§9②）—— 退化情形说清楚：客户端一帧都还没交过
	 * （last_buffer 空）时没有快照可用，只能退回活节点（客户端继续出帧），
	 * 屏上照旧是动的图标。 */
	if (state == VIDOW_MINI_ICON && !view->force_render &&
			view->last_buffer == NULL) {
		wlr_log(WLR_INFO, "mini: %s 进图标态但手里没有最后一帧（客户端还没交过"
			"帧）→ 退回活节点、客户端继续出帧（force_render 关）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	}
	/* S5：进小窗 / 三态互切 —— 上面那次 arrange 已经把目标盒摆进 geom 了，
	 * 这里起过渡动画（起点是进函数时取的「上一帧画在哪」）。 */
	mini_transition_start(view, &from, from_scale);
	/* 形态刚定下来：出帧策略（快照 / 活）与快照节点跟着走一趟。 */
	vidow_mini_frames_sync(view);
	vidow_ipc_emit_window(server, "window-mini-changed", view);
}
