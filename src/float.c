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
	/* 刚变成浮动的那个摆到浮窗组最上面：它正是用户刚动过的那个。不抬的话它可能
	 * 停在「平铺时候」的位置上（平铺的那些节点本来就在浮窗下面，它从下面翻上来
	 * 之后是组里最低的那个），看起来就像「切成浮动了但还是被别的浮窗压着」。
	 * arrange 里的收口只保证「浮动在平铺之上」，组内的先后得在这里点一下。 */
	vidow_restack(server, view);
	vidow_ipc_emit_window(server, "window-floating-changed", view);
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
	if (view->rule_eff.set[VRA_FLOAT]) {
		vidow_float_set_mode(view, view->rule_eff.val[VRA_FLOAT].b ?
			VIDOW_FLOAT_FIXED : VIDOW_FLOAT_NONE);
		return;
	}
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
		 * 那条老路：把客户端 configure 成小窗尺寸。 */
		if (vidow_mini_preserve_buffer(view)) {
			return;
		}
		int cw = 0, ch = 0;
		vidow_view_client_size(view, &cw, &ch);
		if (view->mini == VIDOW_MINI_FREE && cw > 1 && ch > 1 &&
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
		struct vidow_view *view, bool resize, bool from_client) {
	server->grab_mode = resize ? VIDOW_GRAB_RESIZE : VIDOW_GRAB_MOVE;
	server->grab_view = view;
	/* 客户端自己发起的那次（见 server.h 里 grab_client_press 的说明）：按下
	 * 已经转给它了，松手也要转给它，状态才对称。 */
	server->grab_client_press = from_client;
	/* 拖动有果冻，改大小没有：改大小每帧换源缓冲尺寸，网格对不上（见 jelly.c）。 */
	if (!resize) {
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
			if (x + server->grab_geom.width > all.x + all.width) {
				x = all.x + all.width - server->grab_geom.width;
			}
			if (y + server->grab_geom.height > all.y + all.height) {
				y = all.y + all.height - server->grab_geom.height;
			}
			if (x < all.x) {
				x = all.x;
			}
			if (y < all.y) {
				y = all.y;
			}
		}

		view->float_geom.x = x;
		view->float_geom.y = y;
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
			}
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

	/* 松手时光标落在**另一块屏**上：这块浮窗改归那块屏显示着的工作区（跨屏
	 * 拖拽）。位置保持绝对坐标不变，改完按新屏原点刷新相对偏移 —— 不刷新的话
	 * 下一次 arrange 会把它按旧偏移摆到新屏的别处去。 */
	if (view == NULL || view->popup ||
			view->float_mode == VIDOW_FLOAT_NONE) {
		return;
	}
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
 * 和 mini_aspect 那套三级优先级无关 —— 这个只有两级（规则写了就是它）。 */
bool vidow_mini_preserve_buffer(const struct vidow_view *view) {
	if (view == NULL) {
		return true;
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
	if (nw <= 0 || nh <= 0) {
		struct wlr_surface *surface = vidow_view_surface(view);
		nw = surface != NULL ? surface->current.width : 0;
		nh = surface != NULL ? surface->current.height : 0;
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
	double ax = view->mini_ax, ay = view->mini_ay;
	if (ax < -0.5) {
		ax = -0.5;
	} else if (ax > 1.5) {
		ax = 1.5;
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
		/* 迷你 / 自由小窗夹在工作区里（留 mini_edge_margin 的边）。 */
		int margin = cfg->mini_edge_margin;
		if (margin < 0) {
			margin = 0;
		}
		if (x < work.x + margin) {
			x = work.x + margin;
		}
		if (y < work.y + margin) {
			y = work.y + margin;
		}
		if (x + w > work.x + work.width - margin) {
			x = work.x + work.width - margin - w;
		}
		if (y + h > work.y + work.height - margin) {
			y = work.y + work.height - margin - h;
		}
	}

	view->float_geom.x = x;
	view->float_geom.y = y;
	view->float_geom.width = w;
	view->float_geom.height = h;
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

	/* S2：迷你 / 图标**不接键盘**，所以进这两个态之前先把焦点交出去（规矩照抄
	 * 最小化那条：位置不动，谁滑到空出来的位置就是谁）。必须在下面
	 * vidow_float_set_mode() 摘列**之前** —— focus_after_close() 要靠窗口还挂在
	 * 列里才找得到邻居。FREE 照常拿焦点，不用交。 */
	if (state == VIDOW_MINI_MINI || state == VIDOW_MINI_ICON) {
		vidow_view_yield_focus_for_mini(view);
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
	vidow_ipc_emit_window(server, "window-mini-changed", view);
}
