#include "server.h"
#include "ipc.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/util/log.h>

/* niri 风格的 overview：把一块屏上的整条列带缩到这块屏里看全景，点谁就去谁。
 *
 * 场景是全局的，所以这里不动布局、不改客户端尺寸，只算一个「世界 -> 屏幕」的变换
 * （见 server.h 里 struct vidow_overview 的注释），view.c 的 vidow_view_apply_anim
 * 负责把它套到每个窗口的场景节点上。
 *
 * t = 0 时必须是**恒等变换**：进出的动画都在 t 上插值，t = 0 那一端不能有跳变。 */

/* 这块屏上「所有窗口」在世界坐标里的范围（用每个窗口实际摆好的 geom，
 * 所以带上了视口的滚动偏移 —— 正好是它们现在真正在的地方）。 */
static bool strip_box(struct vidow_server *server,
		struct wlr_output *output, struct wlr_box *out) {
	bool any = false;
	int x0 = 0, x1 = 0, y0 = 0, y1 = 0;

	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->output != output || view->popup || !view->mapped ||
				view->geom.width < 1 || view->geom.height < 1) {
			continue;
		}
		/* overview 看的是**现在显示着的那块工作区**：藏起来的工作区不在场
		 * （它们的 geom 还是上次显示时的老值，算进去会把镜头拉偏）。 */
		if (!vidow_workspace_view_visible(server, view)) {
			continue;
		}
		int left = view->geom.x, right = left + view->geom.width;
		int top = view->geom.y, bottom = top + view->geom.height;
		if (!any) {
			x0 = left;
			x1 = right;
			y0 = top;
			y1 = bottom;
			any = true;
			continue;
		}
		if (left < x0) {
			x0 = left;
		}
		if (right > x1) {
			x1 = right;
		}
		if (top < y0) {
			y0 = top;
		}
		if (bottom > y1) {
			y1 = bottom;
		}
	}
	if (!any) {
		return false;
	}
	*out = (struct wlr_box){
		.x = x0,
		.y = y0,
		.width = x1 - x0,
		.height = y1 - y0,
	};
	return true;
}

bool vidow_overview_active(const struct vidow_server *server) {
	return server->overview.t > 0.0 || server->overview.on;
}

/* 图层规则的 overview：盖在 overview_apply 里那几刀「整棵树一起开关」上面，
 * 逐个 surface 决定它在全景里露不露。
 *
 *   only  —— 只在全景里露出来（桌面时收起来）。**全景专用壁纸走这条**：跑第二个
 *           壁纸客户端（awww-daemon -n ov → namespace "awww-daemonov"），再
 *           vidow.rule{ namespace=…, overview="only" }。
 *           桌面时它的场景节点是关的，所以既不会盖住桌面壁纸，也不会进窗口的
 *           模糊底衬（底衬抓的就是这几棵树）。
 *   never —— 全景里不出现（比如全景里不想看见壁纸，只剩兜底色）。
 *
 * 默认 always（没写规则）保持 overview_apply 那套：background / bottom 层跟着进
 * 全景，top / overlay 层收起。对 top / overlay 层写 always / only 都没有意义 ——
 * 那两棵树在全景里是整棵关掉的，逐 surface 开不回来（要开得先改那几刀）。 */
static bool overview_layers_apply(struct vidow_server *server) {
	bool on = server->overview.t > 0.002;
	bool changed = false;
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->scene == NULL || ls->layer_surface == NULL ||
				ls->layer_surface->surface == NULL ||
				!ls->layer_surface->surface->mapped) {
			continue;
		}
		/* 抓底衬的时候 fx.c 会临时把某些 surface 的节点关掉（它要的底衬不能
		 * 含它自己），记在 capture_hidden 上 —— 那一笔不是我们的，别抢。 */
		if (ls->capture_hidden) {
			continue;
		}
		bool want;
		switch (ls->overview_mode) {
		case VIDOW_OVERVIEW_ONLY:
			want = on;
			break;
		case VIDOW_OVERVIEW_NEVER:
			want = !on;
			break;
		default:
			/* always：规则没写（或者刚从 only / never 改回来）—— 恢复默认的
			 * 「开着」。top / overlay 层在全景里由上面那几刀整棵树关掉，
			 * 逐 surface 点亮它也不起作用，所以这里一律 true 是安全的。 */
			want = true;
			break;
		}
		if (ls->scene->tree->node.enabled == want) {
			continue;
		}
		wlr_scene_node_set_enabled(&ls->scene->tree->node, want);
		changed = true;
	}
	return changed;
}

/* 规则改了就重算一遍（rule.c 的 apply_layer 调）。 */
void vidow_overview_refresh_layers(struct vidow_server *server) {
	/* 开关变了要重抓底衬：全景里看到的那张壁纸是**底衬**（模糊那份画在遮罩
	 * 下面），不重抓就还是上一张 —— 全景里另一张壁纸只在「开全景的那一刻」
	 * 换过来，抓的正是这一笔。 */
	if (overview_layers_apply(server)) {
		vidow_backdrop_damage(server, true);
	}
}

/* 算好这一帧的 scale / cam / 输出矩形，并把所有窗口按它摆一遍。 */
static void overview_apply(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	struct wlr_output *output = ov->output;
	if (output == NULL) {
		return;
	}

	struct wlr_box work = vidow_work_area(server, output);
	if (work.width < 1 || work.height < 1) {
		return;
	}
	ov->out_x = work.x;
	ov->out_y = work.y;
	ov->out_w = work.width;
	ov->out_h = work.height;

	/* 恒等那一端：scale = 1、相机在世界坐标的屏中心 */
	double id_cx = work.x + work.width / 2.0;
	double id_cy = work.y + work.height / 2.0;
	double to_cx = id_cx, to_cy = id_cy, to_scale = 1.0;

	struct wlr_box strip;
	if (strip_box(server, output, &strip)) {
		/* 展开那一端：整条带（留 4% 边）铺进工作区，相机跟到带中心 */
		double sx = (double)work.width * 0.92 / (double)strip.width;
		double sy = (double)work.height * 0.92 / (double)strip.height;
		to_scale = sx < sy ? sx : sy;
		if (to_scale > 1.0) {
			to_scale = 1.0; /* 只有两列时别放大：放大反而糊 */
		}
		to_cx = strip.x + strip.width / 2.0 + ov->pan;
		to_cy = strip.y + strip.height / 2.0;
	}

	double t = ov->t;
	if (t < 0.0) {
		t = 0.0;
	}
	if (t > 1.0) {
		t = 1.0;
	}
	ov->scale = 1.0 + (to_scale - 1.0) * t;
	ov->cam_x = id_cx + (to_cx - id_cx) * t;
	ov->cam_y = id_cy + (to_cy - id_cy) * t;
	ov->out_cx = id_cx;
	ov->out_cy = id_cy;

	/* 遮罩：压暗壁纸，让缩小的窗口浮在上面。盖住那块屏（用它的整个布局框）。
	 * 颜色和浓度都是配置给的（overview_dim_color，见 17-overview.lua）。 */
	if (ov->dim != NULL) {
		struct wlr_box box = {0};
		wlr_output_layout_get_box(server->output_layout, output, &box);
		if (box.width < 1) {
			box = (struct wlr_box){ .x = work.x, .y = work.y,
				.width = work.width, .height = work.height };
		}
		const float *c = server->config.overview_dim_color;
		float dim[4] = { c[0], c[1], c[2], (float)(c[3] * t) };
		wlr_scene_rect_set_color(ov->dim, dim);
		wlr_scene_rect_set_size(ov->dim, box.width, box.height);
		wlr_scene_node_set_position(&ov->dim->node, box.x, box.y);
		bool want = t > 0.002 && c[3] > 0.0f;
		if (ov->dim->node.enabled != want) {
			wlr_log(WLR_DEBUG, "overview: 遮罩 %s（t=%.3f，alpha=%.2f）",
				want ? "开" : "关", t, c[3] * t);
		}
		wlr_scene_node_set_enabled(&ov->dim->node, want);
	}

	/* 屏幕空间的东西在 overview 里没有意义：弹出物 / 通知 / 输入法候选框收起来。
	 * 关窗残影是世界坐标的、缩不了，也一起收。 */
	bool on = t > 0.002;
	wlr_scene_node_set_enabled(&server->layer_popups->node, !on);
	wlr_scene_node_set_enabled(&server->layer_top->node, !on);
	wlr_scene_node_set_enabled(&server->layer_overlay->node, !on);
	wlr_scene_node_set_enabled(&server->layer_im->node, !on);
	struct vidow_closing *closing;
	wl_list_for_each(closing, &server->closing, link) {
		if (closing->fx.node != NULL) {
			wlr_scene_node_set_enabled(&closing->fx.node->node, !on);
		}
	}

	/* 上面那几刀是整棵树一起开关；图层规则可以逐个 surface 覆盖它 */
	bool layers_changed = overview_layers_apply(server);

	/* 所有窗口重新摆一遍：变换在 vidow_view_apply_anim 里生效 */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		vidow_view_apply_anim(view);
	}
	vidow_backdrop_damage(server, layers_changed);
}

/* ------------------------------------------------------------- 进出 */

/* 动画引擎给的 t 永远是 0 → 1（见 anim.c）：方向由 on 决定，起点是「这次动画
 * 开始时 t 是多少」（ov->anim_from）。**不能**把 t 直接当进度用 —— 那样退出会
 * 把「进入」重放一遍再瞬间跳回原位（用户报的「点选之后闪弹一下」：先弹回原样、
 * 又缩下来、最后啪一下再弹回去）。中途反向（进到一半再按一次）也从当前进度接着
 * 走，不跳。 */
static void overview_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_server *server = data;
	struct vidow_overview *ov = &server->overview;
	double from = ov->anim_from;
	ov->t = ov->on ? from + (1.0 - from) * t : from * (1.0 - t);
	overview_apply(server);
}

static void overview_done(void *data, bool cancelled) {
	struct vidow_server *server = data;
	struct vidow_overview *ov = &server->overview;
	ov->anim = NULL;
	if (cancelled) {
		/* 被紧接着的新动画接管了：t / output 归新的那次管，别在这儿拍端点。 */
		return;
	}
	ov->t = ov->on ? 1.0 : 0.0;
	overview_apply(server);
	if (!ov->on) {
		ov->output = NULL;
		ov->pan = 0.0;
		/* 收掉那张糊壁纸的缓冲（整屏那么大，4K 上两份 33MB）。场景节点留着
		 * ——它挂在 layer_overview_dim 那棵树上，下次进来接着用。 */
		vidow_fx_end(server, &ov->fx);
		ov->fx_on = false;
	}
	/* 进 / 出都重判一次「谁该出现在场景里」：overview 里**所有**窗口都要画
	 * （缩到画面里），所以滚出视口被剔除的那些得先放回来；退出之后又要按框剔除
	 * 一次（不然它们会一直显示着，串到隔壁屏上）。 */
	vidow_workspace_apply_visibility(server);
}

static void overview_start(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	/* 起点先记下来：cancel 会回调 overview_done，把 t 拍成端点值。没在动画里
	 * 就是干净的两端 —— 进从 0 起，退从 1 起。 */
	double from = ov->anim != NULL ? ov->t : (ov->on ? 0.0 : 1.0);
	vidow_anim_cancel(ov->anim);
	ov->anim = NULL;
	ov->anim_from = from;
	ov->t = from;
	ov->anim = vidow_anim_start(server, "overview", server->config.window_move_ms,
		server->config.easing, overview_update, overview_done, server);
	if (ov->anim == NULL) {
		overview_done(server, false);
	}
}

void vidow_overview_toggle(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	if (ov->on) {
		vidow_overview_exit(server);
		return;
	}
	if (server->focused_output == NULL) {
		return; /* 一块屏都没有，没得看 */
	}
	ov->on = true;
	ov->output = server->focused_output;
	ov->pan = 0.0;
	/* 缩下去的窗口不该还挂着指针焦点（否则客户端以为鼠标还在它身上） */
	wlr_seat_pointer_notify_clear_focus(server->seat);
	overview_start(server);

	/* 顺手把每个窗口在 overview 里的位置算出来打一行：一是让日志能自证，
	 * 二是测试要拿它做像素断言（缩放后的矩形）。 */
	struct wlr_box strip;
	if (strip_box(server, ov->output, &strip)) {
		struct wlr_box work = vidow_work_area(server, ov->output);
		double sx = (double)work.width * 0.92 / (double)strip.width;
		double sy = (double)work.height * 0.92 / (double)strip.height;
		double scale = sx < sy ? sx : sy;
		if (scale > 1.0) {
			scale = 1.0;
		}
		wlr_log(WLR_INFO, "overview: 进入（%s 上 %d 个窗口，占 %dx%d 缩到 %.3f，"
			"铺在工作区 %dx%d 里）", ov->output->name,
			wl_list_length(&server->views), strip.width, strip.height,
			scale, work.width, work.height);

		int id_cx = work.x + work.width / 2;
		int id_cy = work.y + work.height / 2;
		int to_cx = strip.x + strip.width / 2;
		int to_cy = strip.y + strip.height / 2;
		struct vidow_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->output != ov->output || !view->mapped) {
				continue;
			}
			int x = (int)lround(id_cx + (view->geom.x - to_cx) * scale);
			int y = (int)lround(id_cy + (view->geom.y - to_cy) * scale);
			wlr_log(WLR_INFO, "overview: 窗口 %s -> 屏幕 %d,%d %dx%d（缩放 %.3f）",
				vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
				x, y, (int)lround(view->geom.width * scale),
				(int)lround(view->geom.height * scale), scale);
		}
	} else {
		wlr_log(WLR_INFO, "overview: 进入（%s 上没有列）", ov->output->name);
	}
	vidow_ipc_emit_note(server, "overview-toggled");
}

void vidow_overview_exit(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	if (!ov->on && ov->t <= 0.0) {
		return;
	}
	ov->on = false;
	ov->pan = 0.0;
	overview_start(server);
	wlr_log(WLR_INFO, "overview: 退出");
	vidow_ipc_emit_note(server, "overview-toggled");
}

/* ------------------------------------------------------------- 坐标 */

void vidow_overview_map(struct vidow_server *server, double wx, double wy,
		double *sx, double *sy) {
	const struct vidow_overview *ov = &server->overview;
	if (ov->t <= 0.0) {
		*sx = wx;
		*sy = wy;
		return;
	}
	*sx = ov->out_cx + (wx - ov->cam_x) * ov->scale;
	*sy = ov->out_cy + (wy - ov->cam_y) * ov->scale;
}

void vidow_overview_unmap(struct vidow_server *server, double sx, double sy,
		double *wx, double *wy) {
	const struct vidow_overview *ov = &server->overview;
	if (ov->t <= 0.0 || ov->scale <= 0.0) {
		*wx = sx;
		*wy = sy;
		return;
	}
	*wx = ov->cam_x + (sx - ov->out_cx) / ov->scale;
	*wy = ov->cam_y + (sy - ov->out_cy) / ov->scale;
}

double vidow_overview_scale(const struct vidow_server *server) {
	const struct vidow_overview *ov = &server->overview;
	return ov->t > 0.0 ? ov->scale : 1.0;
}

/* ------------------------------------------------------------- 交互 */

/* 点一下：命中的那个窗口就选中（焦点给它，然后退出 overview 缩回去）。
 * 点在空白处 = 什么都不选，直接退出。返回 true 表示这次点击被吃掉了。 */
bool vidow_overview_click(struct vidow_server *server, double sx, double sy) {
	struct vidow_overview *ov = &server->overview;
	if (!vidow_overview_active(server)) {
		return false;
	}

	struct wlr_surface *surface = NULL;
	double lx = 0, ly = 0;
	struct vidow_view *hit = vidow_view_at(server, sx, sy, &surface, &lx, &ly);
	if (hit != NULL && hit->output == ov->output) {
		if (hit->popup) {
			/* 弹出物在 overview 里是收着的，不该被点到 */
			return true;
		}
		vidow_focus_view(hit);
		/* 点下去就退出，但客户端从没收到这次按下；对应的松手也吃掉，
		 * 免得它收到一个没有按下配对的 release。 */
		ov->swallow_release = true;
		wlr_log(WLR_INFO, "overview: 选中 %s",
			vidow_view_title(hit) != NULL ? vidow_view_title(hit) : "(无题)");
	}
	vidow_overview_exit(server);
	return true;
}

/* 滚轮：平移相机（世界坐标）。换列（Mod+H/L）会把平移清零、改回跟焦点走。 */
void vidow_overview_pan(struct vidow_server *server, double dx) {
	struct vidow_overview *ov = &server->overview;
	if (!vidow_overview_active(server)) {
		return;
	}
	ov->pan += dx;
	overview_apply(server);
}

void vidow_overview_update(struct vidow_server *server) {
	if (!vidow_overview_active(server)) {
		return;
	}
	overview_apply(server);
}

bool vidow_overview_init(struct vidow_server *server) {
	/* 层本身由 main.c 按叠放顺序创建（紧贴壁纸之上、窗口之下），这里只放遮罩。
	 * 别再 wlr_scene_tree_create 一次：那会新建一层挂到最后，遮罩就跑窗口上面去了
	 * （踩过一次：overview 里连窗口都被一起压暗）。 */
	server->overview.dim = wlr_scene_rect_create(server->layer_overview_dim, 1, 1,
		(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
	if (server->overview.dim == NULL) {
		return false;
	}
	wlr_scene_node_set_enabled(&server->overview.dim->node, false);
	return true;
}
