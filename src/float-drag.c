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
 * 运行时切换）；这里管「浮动窗口摆哪儿、拖它的时候发生了什么」。
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
			/* 夹取用**盒子当前的尺寸**，不是按下那一刻那份（grab_geom）：拖动中
			 * 跨屏会当场换归属，盒子尺寸跟着新屏重算，拿旧尺寸夹就少走
			 * 「旧 - 新」那一段 —— 实测从 1260x700
			 * 的屏（盒 504x315）拖到 620x340 的屏（盒 248x153）之后，盒子右边缘
			 * 停在 1416 = 1920 - 504，明明还差 248 才到布局右边界。位置本身照旧
			 * 按 grab_geom 推（抓取偏移不变才是「拎着走」）。 */
			int cw = view->float_geom.width > 0 ?
				view->float_geom.width : server->grab_geom.width;
			int ch = view->float_geom.height > 0 ?
				view->float_geom.height : server->grab_geom.height;
			int out_x = 0;
			int out_y = 0;
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
		view->float_geom.width = w;
		view->float_geom.height = h;
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
	if (view == NULL || view->popup ||
			view->float_mode == VIDOW_FLOAT_NONE) {
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
