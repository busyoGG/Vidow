#include "server.h"
#include "ipc.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/util/log.h>

struct layer_scale_ctx {
	int32_t want;
};

static void layer_pin_scale_surface(struct wlr_surface *surface, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	struct layer_scale_ctx *ctx = data;
	if (surface->WLR_PRIVATE.preferred_buffer_scale == ctx->want) {
		return;
	}
	wlr_surface_set_preferred_buffer_scale(surface, ctx->want);
	wlr_fractional_scale_v1_notify_scale(surface, (double)ctx->want);
}

/* layer surface 也要按它所在那块屏的倍率画。wlroots 的 scene layer 只负责
 * enter / leave，不会替我们钉 preferred_buffer_scale；2x 屏上状态栏 / 菜单
 * 于是按 1x 的缓冲铺满整块，屏幕上就是「分辨率糊了」。和 view_pin_scale 一样
 * 只在值变化时发，避免客户端每帧重算。 */
static void layer_pin_popup_scale(struct vidow_layer_surface *ls,
		struct wlr_xdg_popup *popup) {
	if (ls == NULL || popup == NULL || popup->base == NULL ||
			popup->base->surface == NULL || ls->output == NULL) {
		return;
	}
	double scale = ls->output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int32_t want = (int32_t)lround(scale);
	if (want < 1) {
		want = 1;
	}
	struct wlr_surface *surface = popup->base->surface;
	wlr_surface_set_preferred_buffer_scale(surface, want);
	wlr_fractional_scale_v1_notify_scale(surface, (double)want);
	struct layer_scale_ctx ctx = { .want = want };
	wlr_surface_for_each_surface(surface, layer_pin_scale_surface, &ctx);
}

static void layer_pin_scale(struct vidow_layer_surface *ls) {
	if (ls == NULL || ls->layer_surface == NULL ||
			ls->layer_surface->surface == NULL || ls->output == NULL) {
		return;
	}
	double scale = ls->output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int32_t want = (int32_t)lround(scale);
	if (want < 1) {
		want = 1;
	}
	if (ls->pref_scale == want) {
		return;
	}
	ls->pref_scale = want;
	struct wlr_surface *surface = ls->layer_surface->surface;
	wlr_surface_set_preferred_buffer_scale(surface, want);
	wlr_fractional_scale_v1_notify_scale(surface, (double)want);
	struct layer_scale_ctx ctx = { .want = want };
	wlr_surface_for_each_surface(surface, layer_pin_scale_surface, &ctx);
	wlr_log(WLR_INFO, "layer %s: 钉住 preferred_buffer_scale = %d",
		ls->layer_surface->namespace != NULL ?
			ls->layer_surface->namespace : "?", want);
}

static struct wlr_scene_tree *layer_tree(struct vidow_server *server,
		uint32_t layer) {
	switch (layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		return server->layer_background;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		return server->layer_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		return server->layer_top;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		return server->layer_overlay;
	default:
		return server->layer_top;
	}
}

/* 桌面底色（background_color）：垫在所有东西下面的一层纯色。
 *
 * 壁纸本身是 background 层的客户端画的（swaybg / swww / quickshell），vidow 只
 * 负责「谁都没画的时候别是一块死黑」。所以这层在最底下（main.c 里建在
 * layer_background 之前），有壁纸时被完全盖住。
 *
 * 尺寸取整个输出布局的包围盒：多屏之间那点缝（用户手动摆位置摆出来的）没有输出
 * 在那儿，画了也看不见；换成「一块屏一个矩形」要多维护一套增删 / 挪位的账，不值。
 * 输出热插拔 / 改位置（vidow_arrange）、配置重载（vidow_config_sync）都会再调一次。
 *
 * shutting_down 那道门是必须的：退出时 wl_display_destroy() 会把输出一个个拆掉，
 * 每拆一块都会走一遍 vidow_arrange()，而那时候 wlr_output_layout 已经被销毁了 ——
 * 不挡就是 SIGBUS（gdb 里实测：wlr_output_layout_get_box -> vidow_background_apply
 * -> vidow_arrange -> output_destroy）。 */
void vidow_background_apply(struct vidow_server *server) {
	if (server->backdrop == NULL || server->shutting_down) {
		return;
	}
	const float *c = server->config.background_color;
	wlr_scene_rect_set_color(server->backdrop, c);

	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, NULL, &box);
	if (box.width < 1 || box.height < 1 || c[3] <= 0.0f) {
		wlr_scene_node_set_enabled(&server->backdrop->node, false);
		return;
	}
	wlr_scene_rect_set_size(server->backdrop, box.width, box.height);
	wlr_scene_node_set_position(&server->backdrop->node, box.x, box.y);
	wlr_scene_node_set_enabled(&server->backdrop->node, true);
}

/* 「谁在画壁纸」变了（background 层的 surface 上屏 / 下屏 / 没了）：发一个事件。
 * shell 常驻画背景时靠它决定要不要接管（vidowmsg background 能看到当前状态）。 */
static void background_changed(struct vidow_layer_surface *ls) {
	if (ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
		return;
	}
	vidow_ipc_emit_background(ls->server, "background-changed");
}

/* 把这一块 layer 面自己的动画状态（偏移 + 透明度）写到场景节点上：
 * 位置 = 布局定下的 base + 动画偏移。
 *
 * 为什么单独拎出来、而不是让动画每帧再跑一趟 vidow_arrange_layers：那一趟是
 * **布局**（谁排哪、usable area 多少），它每碰到一块 layer 面就调一次 wlroots 的
 * configure，而 configure 是无条件发包的。动画每帧、每块正在动的面各跑一趟 =
 * 面数² 条 configure：14 块面一起上屏时每帧 196 条、一秒上万条（实测 300ms 里
 * 8000 条 / 190KB）。客户端一时读不过来（启动那几秒正是它最忙的时候），socket
 * 灌满之后 libwayland 的 4KB 出口缓冲放不下下一条事件，直接判 E2BIG 把整个
 * 客户端踢掉 —— 日志里就是 "Data too big for buffer (4092 + 12 > 4096)." 紧跟
 * "error in client communication"。动画只动这一块，不需要重排布局。 */
static void layer_apply_anim(struct vidow_layer_surface *ls) {
	wlr_scene_node_set_position(&ls->scene->tree->node,
		ls->base_x + (int)lround(ls->anim_off_x),
		ls->base_y + (int)lround(ls->anim_off_y));
	/* 入场 / 退场动画的 alpha 乘上规则给的 opacity（图层规则写了 opacity 才有
	 * 这一项）：面板半透明 = 它自己的内容淡下去，背后的东西透出来。 */
	vidow_fx_set_alpha(&ls->scene->tree->node,
		ls->anim_alpha * vidow_rule_opacity(&ls->rule_eff));
	/* 垫在下面的模糊底衬不跟着淡：opacity 说的是「这块面板多透」，透出来的
	 * 应该是糊过的背景（和窗口那条路一致 —— 装饰着色器里 u_opacity 也只乘内容
	 * 和边框）。fx 节点是这块场景树的子节点，上面那一趟会连它一起乘，这里按它
	 * 自己的状态放回去（masked 是抓帧遮挡，见 fx.c）。 */
	if (ls->fx.node != NULL) {
		wlr_scene_buffer_set_opacity(ls->fx.node,
			ls->fx.masked ? 0.0f : 1.0f);
	}
}

/* 全屏窗口底下那条「看不见但还在吃输入」的状态栏：layer 节点关掉，
 * 场景命中测试就不会再落它头上。只动 top 层且 exclusive_zone > 0 的
 * （状态栏 / dock 那类占位的）；通知、截图 overlay（exclusive_zone = -1）
 * 照旧显示。 */
void vidow_layers_update_fullscreen(struct vidow_server *server) {
	if (server == NULL) {
		return;
	}
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		bool full = false;
		struct vidow_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->popup || !view->fullscreen || !view->mapped ||
					view->scene_parked) {
				continue;
			}
			struct wlr_output *want = view->ws != NULL &&
				view->ws->output != NULL ? view->ws->output : view->output;
			if (want == output->wlr_output &&
					vidow_workspace_view_visible(server, view)) {
				full = true;
				break;
			}
		}
		struct vidow_layer_surface *ls;
		wl_list_for_each(ls, &server->layers, link) {
			if (ls->output != output ||
					ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_TOP ||
					ls->scene == NULL ||
					ls->layer_surface->current.exclusive_zone <= 0) {
				continue;
			}
			if (ls->scene->tree->node.enabled == !full) {
				continue; /* 已经是想要的状态：别每趟都碰节点 */
			}
			wlr_scene_node_set_enabled(&ls->scene->tree->node, !full);
			/* 状态栏被藏掉 / 放回来：底衬里那一块变了，而且这条变化不经过
			 * blur_note_damage（它改的是节点可见性）—— 让 arrange 收口回退
			 * 整屏。低频（进出全屏那一下），不值得为它算精确区域。 */
			if (server->arrange_in_progress) {
				server->arrange_force_full = true;
			}
		}
	}
}

/* Position every layer surface and recompute each output's usable area.
 * Called whenever the set of layer surfaces changes.
 *
 * 注意这里**不**打 backdrop 标记：这个函数被好几条路径调（面出现 / 消失 / 换屏 /
 * 规则重算），而每打一次标记，所有窗口的模糊金字塔就要重建一遍 —— 重新抓一屏、
 * 重跑 4 级 kawase、再重新分配 7 个 GBM 缓冲。以前 layer 开关动画每帧也走这里
 * （量到 143 次/秒），动画直接不丝滑。真正改变底衬的地方自己打标记：layer
 * 出现 / 消失 / 换屏 / 内容重画 / 动画落定。
 *
 * 反过来：**动画每帧不该调这里**（noctalia 被灌断的那条路就是这么来的）。这里每
 * 碰到一块 layer 面就发一次 configure，而动画是「每块正在动的面各调一次」——
 * 上屏那一下就是面数² 条 configure，能把客户端的 socket 灌爆（见
 * layer_apply_anim）。 */
void vidow_arrange_layers(struct vidow_server *server) {
	struct vidow_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box full_area;
		wlr_output_layout_get_box(server->output_layout, output->wlr_output,
			&full_area);
		struct wlr_box usable_area = full_area;

		for (uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;
				layer <= ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY; layer++) {
			struct vidow_layer_surface *ls;
			/* newest first, matching sway's exclusive-zone ordering */
			wl_list_for_each_reverse(ls, &server->layers, link) {
				if (ls->output != output || ls->layer != layer) {
					continue;
				}
				/* wlroots asserts unless the client has committed once;
				 * the initial configure is sent from the commit handler. */
				if (!ls->layer_surface->initialized) {
					continue;
				}
				layer_pin_scale(ls);
				wlr_log(WLR_DEBUG, "configure layer %s layer=%u",
					ls->layer_surface->namespace ?: "?", layer);
				/* configure() writes the node position itself; clear our
				 * animation offset first so it cannot accumulate. */
				wlr_scene_node_set_position(&ls->scene->tree->node, 0, 0);
				wlr_scene_layer_surface_v1_configure(ls->scene, &full_area,
					&usable_area);

				int lx = 0, ly = 0;
				wlr_scene_node_coords(&ls->scene->tree->node, &lx, &ly);
				struct wlr_box box = {
					.x = lx, .y = ly,
					.width = ls->layer_surface->surface->current.width,
					.height = ls->layer_surface->surface->current.height,
				};
				/* 位置 / 尺寸真的变了才作废背后的模糊：这个函数在开关动画里
				 * 每帧都调，而 anim_off 是加在场景节点上的、geom 不含它 ——
				 * 所以动画期间这里不会每帧作废。 */
				if (memcmp(&box, &ls->geom, sizeof(box)) != 0) {
					ls->geom = box;
					vidow_layer_blur_invalidate(ls);
				}
				/* 布局位置存下来：动画（layer_apply_anim）按 base + 偏移
				 * 挪，不必再跑这一趟布局。 */
				ls->base_x = lx;
				ls->base_y = ly;
				layer_apply_anim(ls);
			}
		}

		output->usable_area = usable_area;
	}
}

/* ------------------------------------------------------------- keyboard */

static void layer_focus(struct vidow_layer_surface *ls) {
	struct vidow_server *server = ls->server;
	struct wlr_seat *seat = server->seat;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);

	if (server->keyboard_layer == NULL) {
		server->focus_before_layer = server->focused;
	}
	server->keyboard_layer = ls;
	if (kb != NULL) {
		wlr_seat_keyboard_notify_enter(seat, ls->layer_surface->surface,
			kb->keycodes, kb->num_keycodes, &kb->modifiers);
	} else {
		wlr_seat_keyboard_notify_enter(seat, ls->layer_surface->surface,
			NULL, 0, NULL);
	}
	vidow_im_focus(server, ls->layer_surface->surface);
}

static void layer_unfocus(struct vidow_layer_surface *ls) {
	struct vidow_server *server = ls->server;

	if (server->keyboard_layer != ls) {
		return;
	}
	server->keyboard_layer = NULL;

	/* Hand the keyboard back to the toplevel that had it before this layer
	 * grabbed it. 不能直接用 server->focused：layer 活着期间它可能已经被
	 * 别处改过（点通知 / 客户端 activate），那样截图一关焦点就跑到下一个
	 * 窗口去了。 */
	struct vidow_view *view = server->focus_before_layer != NULL ?
		server->focus_before_layer : server->focused;
	server->focus_before_layer = NULL;
	if (view != NULL) {
		wlr_seat_keyboard_notify_clear_focus(server->seat);
		vidow_focus_view(view);
	}
}

bool vidow_layer_focus_surface(struct vidow_server *server,
		struct wlr_surface *surface) {
	if (surface == NULL) {
		return false;
	}
	struct wlr_layer_surface_v1 *layer_surface =
		wlr_layer_surface_v1_try_from_wlr_surface(surface);
	if (layer_surface == NULL) {
		return false;
	}

	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->layer_surface != layer_surface) {
			continue;
		}
		if (layer_surface->current.keyboard_interactive ==
				ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
			return false;
		}
		layer_focus(ls);
		return true;
	}
	return false;
}

/* ------------------------------------------------------------ animation */

/* Which way a layer surface should fly in from, based on its anchors. */
static void layer_slide_dir(struct vidow_layer_surface *ls, int *dx, int *dy) {
	uint32_t anchor = ls->layer_surface->current.anchor;
	*dx = 0;
	*dy = 0;
	if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP) {
		*dy = -1;
	} else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM) {
		*dy = 1;
	} else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT) {
		*dx = -1;
	} else if (anchor & ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT) {
		*dx = 1;
	} else {
		*dy = -1;
	}
}

static void layer_anim_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_layer_surface *ls = data;
	struct vidow_config *cfg = &ls->server->config;
	double k = 1.0 - t;

	ls->anim_off_x = ls->anim_from_x * k;
	ls->anim_off_y = ls->anim_from_y * k;
	ls->anim_alpha = cfg->open_alpha_from + (1.0 - cfg->open_alpha_from) * t;
	/* 只挪 / 只淡这一块自己。这里以前调的是 vidow_arrange_layers()，等于每帧
	 * 给每一块 layer 面重发一次 configure —— 上屏那 200ms 里上万条，客户端的
	 * socket 就是被这个灌满的（见 layer_apply_anim 的注释）。 */
	layer_apply_anim(ls);
}

/* layer 自己变了：只标**它经过的那一块**，不整屏。
 *
 * 整屏标脏的代价：每个窗口的私有底衬都要重算一遍（从它自己那块底衬上跑一条
 * 金字塔，代价正比于窗口面积，见 fx.c:4155），而实际变的只有这一块 layer 底下
 * 的东西。在 3840x2160 @2x 那块屏上窗口本来就大，26 个窗口一起重算就是白烧。
 * 一条通知弹出、一个菜单收起、vshot 的冻结覆盖层上屏，原来都是整屏。
 *
 * box == NULL 或空 → 退回整屏（管不了的时候宁可多抓一次）。
 * 传入的是 layer 在布局坐标系里的**当前**几何；正在滑动的那些调用点自己把「动
 * 之前那块」并进来再传。 */
/* wlr_box_union 在 0.20 的头里没导出（wlr_box.h 只给了 empty/intersection/
 * contains），自己写一个：一次并集，别为它拉个头文件依赖。 */
static void layer_box_union(struct wlr_box *a, const struct wlr_box *b) {
	if (b->width <= 0 || b->height <= 0) {
		return;
	}
	if (a->width <= 0 || a->height <= 0) {
		*a = *b;
		return;
	}
	int x1 = a->x < b->x ? a->x : b->x;
	int y1 = a->y < b->y ? a->y : b->y;
	int x2 = a->x + a->width > b->x + b->width ? a->x + a->width : b->x + b->width;
	int y2 = a->y + a->height > b->y + b->height ? a->y + a->height : b->y + b->height;
	*a = (struct wlr_box){ .x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1 };
}

static void layer_damage_box(struct vidow_layer_surface *ls,
		const struct wlr_box *box) {
	vidow_backdrop_damage_output_box(ls->output, box);
}

static void layer_anim_done(void *data, bool cancelled) {
	struct vidow_layer_surface *ls = data;
	ls->anim = NULL;
	if (!cancelled) {
		ls->anim_off_x = 0.0;
		ls->anim_off_y = 0.0;
		ls->anim_alpha = 1.0;
	}
	/* 动画停在最终位置上了，现在重做一次模糊，把这一帧的底衬固化下来。
	 *
	 * 脏的是**整条走过的带子**，不只是停下的那块：起点和终点并起来。 */
	struct wlr_box anim_box = ls->geom;
	struct wlr_box from = ls->geom;
	from.x += (int)lround(ls->anim_off_x);
	from.y += (int)lround(ls->anim_off_y);
	layer_box_union(&anim_box, &from);
	layer_damage_box(ls, &anim_box);
	layer_apply_anim(ls);
}

/* 有些 layer 不该有开合动画：截图 overlay 自己弹一下纯属碍事，通知也是
 * 「来就来了」，回弹反而像窗口。规则里显式写了 anim / open_ms / close_ms
 * 的以规则为准（用户明确要动就动）。 */
static bool layer_anim_skip(const struct vidow_layer_surface *ls) {
	const char *ns = ls->layer_surface->namespace;
	if (ns == NULL) {
		return false;
	}
	if (strncmp(ns, "vshot", 5) == 0) {
		return true;
	}
	if (strstr(ns, "notif") != NULL ||
			strcmp(ns, "mako") == 0 || strcmp(ns, "dunst") == 0 ||
			strcmp(ns, "fnott") == 0 || strcmp(ns, "swaync") == 0) {
		return true;
	}
	return false;
}

static void layer_open_anim(struct vidow_layer_surface *ls) {
	struct vidow_config *cfg = &ls->server->config;
	struct vidow_rule_anim anim;
	vidow_rule_anim(cfg, &ls->rule_eff, true, &anim);
	if (layer_anim_skip(ls) &&
			!ls->rule_eff.set[VRA_ANIM] &&
			!ls->rule_eff.set[VRA_OPEN_MS]) {
		anim.open_ms = 0;
	}
	/* 图层规则 anim = false / open_ms = 0：不滑也不淡，直接到位（时长 0 会让
	 * vidow_anim_start 返回 NULL，下面那段就是「瞬间到位」）。 */
	if (anim.open_ms <= 0) {
		vidow_anim_cancel(ls->anim);
		ls->anim = NULL;
		ls->anim_off_x = 0.0;
		ls->anim_off_y = 0.0;
		ls->anim_alpha = 1.0;
		/* 布局刚在上面那趟 arrange 里定过（base_x/base_y），这里只要把
		 * 动画状态摆平 —— 不用再整趟重排、不用再发 configure。 */
		layer_apply_anim(ls);
		return;
	}
	int dx = 0, dy = 0;
	layer_slide_dir(ls, &dx, &dy);

	int w = ls->geom.width > 0 ? ls->geom.width : 64;
	int h = ls->geom.height > 0 ? ls->geom.height : 64;
	ls->anim_from_x = (double)(dx * w);
	ls->anim_from_y = (double)(dy * h);
	ls->anim_off_x = ls->anim_from_x;
	ls->anim_off_y = ls->anim_from_y;
	ls->anim_alpha = cfg->open_alpha_from;

	vidow_anim_cancel(ls->anim);
	ls->anim = vidow_anim_start(ls->server, "layer-open", anim.open_ms,
		cfg->layer_open_easing, layer_anim_update, layer_anim_done, ls);
	if (ls->anim == NULL) {
		ls->anim_off_x = 0.0;
		ls->anim_off_y = 0.0;
		ls->anim_alpha = 1.0;
	}
}

/* The layer surface has no buffer once unmap fires: animate a snapshot. */
static void layer_snapshot_close(struct vidow_layer_surface *ls) {
	struct vidow_server *server = ls->server;
	struct vidow_config *cfg = &server->config;
	struct vidow_rule_anim anim;
	vidow_rule_anim(cfg, &ls->rule_eff, true, &anim);
	if (layer_anim_skip(ls) &&
			!ls->rule_eff.set[VRA_ANIM] &&
			!ls->rule_eff.set[VRA_CLOSE_MS]) {
		anim.close_ms = 0;
	}
	if (ls->last_buffer == NULL || anim.close_ms <= 0 ||
			ls->geom.width <= 0 || ls->geom.height <= 0) {
		/* 「怎么没有关闭动画」在窗口那条路上是收到过的反馈（见 view.c），图层
		 * 这里同样有四条路能跳过它：日志里说清楚是哪条，不然只能靠猜。 */
		wlr_log(WLR_DEBUG, "关闭动画跳过 layer %s：%s",
			ls->layer_surface->namespace != NULL ?
				ls->layer_surface->namespace : "(无 namespace)",
			ls->last_buffer == NULL ? "手里没有最后一帧快照" :
			anim.close_ms <= 0 ? (anim.on ? "close_ms = 0（配置或规则里关了）" :
				"规则写了 anim = false") :
			"还没定下尺寸（geom 是空的）");
		return;
	}
	int dx = 0, dy = 0;
	layer_slide_dir(ls, &dx, &dy);
	int slide = cfg->close_slide_px;

	/* layer surface 没有「窗口几何」和表面之分：整块就是内容。 */
	struct vidow_fx_geo geo = {
		.off_x = 0,
		.off_y = 0,
		.w = ls->geom.width,
		.h = ls->geom.height,
		.src_w = ls->geom.width,
		.src_h = ls->geom.height,
	};
	vidow_closing_start(server, layer_tree(server, ls->layer), ls->last_buffer,
		&geo, ls->geom.x + (int)lround(ls->anim_off_x),
		ls->geom.y + (int)lround(ls->anim_off_y),
		ls->geom.width, ls->geom.height, anim.close_ms,
		dy * slide, false /* 不套窗口装饰：layer 自己就是内容 */, NULL,
		ls->output != NULL && ls->output->wlr_output != NULL ?
			(double)ls->output->wlr_output->scale : 1.0,
		/* layer 的关闭用 layer_close_easing、也不缩放：状态栏「缩着淡出」很怪。 */
		cfg->layer_close_easing, 1.0, &ls->rule_eff,
		false /* layer 没有「全屏」这回事 */);
}

/* --------------------------------------------------------------- events */

static void layer_surface_map(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, map);
	struct vidow_server *server = ls->server;

	wlr_log(WLR_INFO, "layer map %s layer=%u exclusive=%d kb_interactive=%u",
		ls->layer_surface->namespace != NULL ? ls->layer_surface->namespace : "?",
		ls->layer, ls->layer_surface->current.exclusive_zone,
		ls->layer_surface->current.keyboard_interactive);
	layer_pin_scale(ls);

	if (ls->layer_surface->current.keyboard_interactive !=
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE) {
		layer_focus(ls);
	}

	/* 规则先套上：开合时长（anim / open_ms）和不透明度都在 ls->rule_eff 里，
	 * 入场动画那一刻就要用 —— 放在 layer_open_anim 后面的话，规则写了
	 * open_ms = 3000 也会先按全局的 200ms 起一遍动画（改完再重排也来不及，
	 * 动画已经跑上了）。 */
	vidow_rule_apply_layer(server, ls);

	/* 一块 layer surface 上屏：底衬里多了一个东西。
	 *
	 * 标脏挪到 arrange_layers **之后**：那块盒子的几何就是在这儿算出来的，
	 * 之前只有 0 —— 那才是原来只能整屏标脏的原因。 */
	vidow_arrange_layers(server); /* gives ls->geom, used to pick the slide */
	layer_damage_box(ls, &ls->geom);
	layer_open_anim(ls);
	vidow_arrange_layers(server);
	vidow_arrange(server);
	vidow_ipc_emit_layer(server, "layer-mapped", ls);
	background_changed(ls);

	/* **新上屏的 layer 要立刻拿到指针焦点。**
	 *
	 * 指针焦点的 enter 只在指针**移动**时发（见 input.c 的
	 * vidow_pointer_motion），映射本身不触发。于是「鼠标一动不动、有客户端
	 * 新映射一块全屏 layer」时，那块 surface 永远收不到 wl_pointer.enter ——
	 * 客户端要是靠 enter 认「指针在我这块屏上」，就会一直等到自己的超时。
	 *
	 * 用户报的「vshot 截图卡一下、鼠标动一下就不卡」就是这个：vshot 的冻结
	 * 覆盖层上来之后等 pointer enter 才认当前屏（wayland/mod.rs 的
	 * wait_for_current_output），而鼠标不动就没人给它发。Hyprland 那边正常，
	 * 因为它在 layer 映射后会重算一次指针焦点。
	 *
	 * 放在 arrange 之后：命中测试要读刚算好的 ls->geom。 */
	vidow_pointer_motion(server, 0);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, unmap);
	struct vidow_server *server = ls->server;

	layer_snapshot_close(ls);
	layer_unfocus(ls);
	/* 下屏：它原来占的那块底下露出来了。geom 还是下屏前的值（arrange 在下面）。 */
	layer_damage_box(ls, &ls->geom);
	vidow_arrange_layers(server);
	vidow_arrange(server);
	vidow_ipc_emit_layer(server, "layer-unmapped", ls);
	background_changed(ls);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, commit);
	struct vidow_server *server = ls->server;

	layer_pin_scale(ls);
	/* The first configure must reflect the state the client just committed
	 * (anchor, exclusive zone, desired size). */
	if (ls->layer_surface->initial_commit) {
		/* 首次 configure：形状从「站着的地方」变成「该站的地方」，
		 * 两块的并集才是真脏的那一片。 */
		struct wlr_box before = ls->geom;
		vidow_arrange_layers(server);
		layer_box_union(&before, &ls->geom);
		layer_damage_box(ls, &before);
		vidow_arrange(server);
	}

	struct wlr_client_buffer *client = ls->layer_surface->surface->buffer;
	if (client != NULL && client->source != NULL &&
			client->source != ls->last_buffer) {
		wlr_buffer_unlock(ls->last_buffer);
		ls->last_buffer = wlr_buffer_lock(client->source);
	}

	/* A repaint of the wallpaper or a bottom bar changes the backdrop of
	 * every window; the upper layers are not part of it. */
	if (ls->layer <= ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
		if (ls->output != NULL) {
			vidow_backdrop_damage_output(ls->output);
		} else {
			vidow_backdrop_damage(server, true);
		}
	}

	/* 这块 layer 自己的内容刚交了新帧：它背后的模糊拿它的缓冲当 mask
	 * （fx.c 的 layer_blur_frame），不重画就一直是上一版的 alpha / 形状。
	 * 典型现场：状态栏弹出菜单时透明区域变了，底下的模糊还按旧 mask 剔，
	 * 看着就是「层的内容动了、模糊没跟上」。
	 *
	 * 这里只推 blur_ver（「已画的那一版作废」）。底衬要不要重抓是另一件事，
	 * 由 fx.c 的 fresh 判据自己决定（世界代次 + 损伤盒子 + 区域尺寸 + 级数），
	 * 跟这次 invalidate 无关 —— blur_ver 不在 fresh 的任何一项里。 */
	vidow_layer_blur_invalidate(ls);
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, destroy);
	struct vidow_server *server = ls->server;
	/* 这一块是不是画壁纸的：free(ls) 之后还得知道，而且那时候再发事件 ——
	 * 事件载荷里的 clients 是**当场现数**的，在链表摘掉之前发就会数到这块已经
	 * 死掉的 surface（查询和事件对不上）。 */
	bool was_background = ls->layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;

	wlr_log(WLR_DEBUG, "layer closed %s layer=%u",
		ls->layer_surface->namespace ?: "?", ls->layer);
	vidow_ipc_emit_layer(server, "layer-closed", ls);
	layer_unfocus(ls);
	vidow_layer_blur_release(ls);
	vidow_anim_cancel(ls->anim);
	ls->anim = NULL;
	wlr_buffer_unlock(ls->last_buffer);
	ls->last_buffer = NULL;

	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->unmap.link);
	wl_list_remove(&ls->commit.link);
	wl_list_remove(&ls->new_popup.link);
	wl_list_remove(&ls->destroy.link);
	wl_list_remove(&ls->link);
	/* geom 在 free 之后就没了：先抄一份，标脏用 */
	struct wlr_box gone = ls->geom;
	struct vidow_output *gone_out = ls->output;
	free(ls);

	vidow_backdrop_damage_output_box(gone_out, &gone);
	vidow_arrange_layers(server);
	vidow_arrange(server);
	if (was_background) {
		vidow_ipc_emit_background(server, "background-changed");
	}
}

/* 「根表面是这块 layer surface」时，unconstrain 要的可用区：输出盒（布局坐标）
 * 平移到这块 layer 表面自己的坐标系里。
 *
 * 同一个坑（见 view.c 的 vidow_popup_box_to_toplevel_space）：unconstrain 要的是
 * **根表面自己的坐标系**里的可用区，而 wlr_output_layout_get_box() 给的是布局坐标
 * —— 状态栏在屏幕顶端时 y 要减掉它的高度，不然菜单会被摆到屏幕上方外面去。
 *
 * 两个调用点：一级弹出物（layer.c 的 layer_surface_new_popup，父面就是这块 layer
 * 表面）和挂在 layer 表面上的**二级**弹出物（view.c 的 server_new_popup —— 父面是
 * 一级弹出物时 wlroots 只发 xdg_shell 那条 new_popup，见那边的
 * popup_root_layer_surface）。 */
void vidow_popup_box_to_layer_space(struct vidow_server *server,
		const struct vidow_layer_surface *ls, struct wlr_box *box) {
	if (ls == NULL || ls->output == NULL || box == NULL) {
		return;
	}
	wlr_output_layout_get_box(server->output_layout, ls->output->wlr_output,
		box);
	int sx = 0, sy = 0;
	wlr_scene_node_coords(&ls->scene->tree->node, &sx, &sy);
	box->x -= sx;
	box->y -= sy;
}

static void layer_surface_new_popup(struct wl_listener *listener, void *data) {
	struct vidow_layer_surface *ls = wl_container_of(listener, ls, new_popup);
	struct vidow_server *server = ls->server;
	struct wlr_xdg_popup *popup = data;

	/* 挂在**这块 layer 表面**的树里（弹出物得跟着状态栏一起动），但父面本身是
	 * 弹出物时（二级菜单）得挂到**父弹出物**的树上 —— 挑父树这件事在
	 * vidow_popup_scene_create 里，跟窗口那条路共用（漏了这里就是子菜单错位，
	 * 见 server.h 的说明）。 */
	vidow_popup_scene_create(server, ls->scene->tree, popup);
	layer_pin_popup_scale(ls, popup);

	struct wlr_box box = {0};
	vidow_popup_box_to_layer_space(server, ls, &box);
	/* 跟 view.c 一样：unconstrain 要等 popup 自己第一次 commit（wlroots 里
	 * schedule_configure 有 assert(surface->initialized)） */
	vidow_popup_unconstrain_on_commit(server, popup, &box);
}

/* 一块屏拔了：它的 layer surface（状态栏这类）挪到接收它的那块屏上 —— 跟窗口
 * 一样，不能让 ls->output 指着已经被释放的 vidow_output。归属留在记忆里。 */
void vidow_layer_output_gone(struct vidow_server *server,
		struct vidow_output_memory *memory, struct wlr_output *dead,
		struct vidow_output *fallback) {
	size_t moved = 0;
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->home != memory && (ls->output == NULL ||
				ls->output->wlr_output != dead)) {
			continue;
		}
		ls->output = fallback;
		if (fallback != NULL) {
			/* 客户端也要知道它现在画在哪块屏上（anchors 要按新屏的尺寸算） */
			ls->layer_surface->output = fallback->wlr_output;
		}
		moved++;
	}
	if (moved == 0) {
		return;
	}
	vidow_backdrop_damage(server, true);
	vidow_arrange_layers(server);
	wlr_log(WLR_INFO, "hotplug: %zu 个 layer surface 挪到 %s 上住", moved,
		fallback != NULL ? fallback->wlr_output->name : "(没有屏了)");
}

/* 一块屏回来了：归属它的 layer surface 挪回去。 */
void vidow_layer_output_added(struct vidow_server *server,
		struct vidow_output_memory *memory, struct vidow_output *output) {
	size_t moved = 0;
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->output == output) {
			continue;
		}
		if (ls->home != memory && ls->output != NULL) {
			continue; /* 有别的家，也还在住着，别动它 */
		}
		if (ls->home == NULL) {
			ls->home = memory;
		}
		ls->output = output;
		ls->layer_surface->output = output->wlr_output;
		moved++;
	}
	if (moved > 0) {
		vidow_backdrop_damage(server, true);
		vidow_arrange_layers(server);
	}
}

static void server_new_layer_surface(struct wl_listener *listener, void *data) {
	struct vidow_server *server =
		wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	struct vidow_layer_surface *ls = calloc(1, sizeof(*ls));
	if (ls == NULL) {
		return;
	}
	ls->server = server;
	ls->layer_surface = layer_surface;
	ls->id = ++server->next_layer_id;
	ls->layer = layer_surface->pending.layer;
	wlr_log(WLR_DEBUG, "new layer surface %s layer=%u out=%p",
		layer_surface->namespace ?: "?", ls->layer, (void *)layer_surface->output);

	/* Clients may leave the output unset ("pick one for me"). */
	if (layer_surface->output == NULL) {
		layer_surface->output = server->focused_output;
		if (layer_surface->output == NULL && !wl_list_empty(&server->outputs)) {
			struct vidow_output *o =
				wl_container_of(server->outputs.next, o, link);
			layer_surface->output = o->wlr_output;
		}
	}

	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		if (o->wlr_output == layer_surface->output) {
			ls->output = o;
			break;
		}
	}
	if (ls->output == NULL) {
		wlr_log(WLR_ERROR, "layer surface for unknown output, dropping");
		free(ls);
		return;
	}
	/* 归属记在记忆里：这块屏拔了要挪到别的屏上，屏回来再挪回来。 */
	ls->home = vidow_output_memory_for(&server->output_memories,
		layer_surface->output->name);

	ls->anim_alpha = 1.0;
	/* destroy 监听**必须挂在 wlr_scene_layer_surface_v1_create 之前**：
	 * create 里 wlroots 自己也往 events.destroy 上挂了一个（销毁 ls->scene 那棵树
	 * 连同整棵子树，再释放 scene_layer_surface 本身），而 wl_signal_emit 是按挂的
	 * 顺序回调的。挂在后面，轮到我们的时候那棵树和垫在树里的模糊节点（见 fx.c 的
	 * layer_blur_frame）都已经是释放过的内存了 —— vidow_layer_blur_release 拿着
	 * 野指针调 wlr_scene_node_destroy，它顺着野 parent 往上走就段错误（noctalia
	 * 重建一次面板必崩）。挂前面我们就是第一个：先把自己的东西收干净，再让
	 * wlroots 拆树。 */
	ls->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &ls->destroy);

	ls->scene = wlr_scene_layer_surface_v1_create(
		layer_tree(server, ls->layer), layer_surface);
	if (ls->scene == NULL) {
		wl_list_remove(&ls->destroy.link);
		free(ls);
		return;
	}

	wl_list_insert(&server->layers, &ls->link);

	ls->map.notify = layer_surface_map;
	wl_signal_add(&layer_surface->surface->events.map, &ls->map);
	ls->unmap.notify = layer_surface_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &ls->unmap);
	ls->commit.notify = layer_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &ls->commit);
	ls->new_popup.notify = layer_surface_new_popup;
	wl_signal_add(&layer_surface->events.new_popup, &ls->new_popup);
}

bool vidow_layer_init(struct vidow_server *server) {
	server->layer_shell = wlr_layer_shell_v1_create(server->display, 4);
	if (server->layer_shell == NULL) {
		return false;
	}
	server->new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface,
		&server->new_layer_surface);
	return true;
}
