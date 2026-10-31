/* 弹出物（xdg_popup / X11 菜单 / 输入法候选框）背后的模糊。
 *
 * 做法抄 layer 那一套：在弹出物自己的场景树里垫一个只画模糊的节点（blur_only），
 * 底衬按它自己那一小块区域重抓 —— 基座从共享底衬上 1:1 拷一块，窗口 / 别的弹出物
 * 再画上去。区别只有几何来源：
 *   - xdg / X11 的节点挂在弹出物自己的树上，相对坐标恒等 0,0；
 *   - 输入法候选框挂在 layer_im 上，位置每帧按绝对坐标摆。
 * 抓底衬时把弹出物自己藏起来（pb->hide），否则模糊里会浮出它自己的影子。
 *
 * 依赖方向：只用 fx-internal.h 的基础设施（底衬抓取 / 模糊链 / 目标缓冲）和
 * decor_draw 这一个着色器入口；不碰 fx.c 里窗口和 layer 的账本。 */
#include "server.h"
#include "fx-internal.h"
#include "fx-popup.h"

#include <math.h>
#include <stdlib.h>

#include <GLES2/gl2.h>
#include <wlr/render/gles2.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------- 弹出物的背景模糊 */

static void popup_blur_node_destroy(struct wl_listener *listener, void *data);
static void popup_blur_owner_destroy(struct wl_listener *listener, void *data);


/* 窗口的 xdg_popup / X11 菜单提示 / 输入法候选框背后的模糊。
 *
 * 抄的是上面 layer_blur_frame 的做法：在弹出物自己的场景树里垫一个只画模糊的
 * 节点（blur_only），底衬按它自己那一小块区域重抓 —— 基座从共享底衬上 1:1
 * 拷一块，窗口 / 别的弹出物再画上去。区别只有几何来源：
 *   - xdg / X11 的节点挂在弹出物自己的树上，相对坐标恒等 0,0；
 *   - 输入法候选框挂在 layer_im 上，位置每帧按绝对坐标摆。
 * 抓底衬时把弹出物自己藏起来（pb->hide），否则模糊里会浮出它自己的影子。 */

static bool popup_blur_geom(struct vidow_popup_blur *pb, int *x, int *y,
		int *w, int *h) {
	if (pb->tree == NULL || pb->hide == NULL || pb->surface == NULL) {
		return false;
	}
	if (!wlr_scene_node_coords(pb->hide, x, y)) {
		return false;
	}
	if (pb->view != NULL) {
		struct wlr_box g = vidow_view_geometry(pb->view);
		*w = g.width;
		*h = g.height;
	} else {
		/* GTK 的 xdg geometry 不一定等于看得见的窗口体（CSD 阴影 / 主题
		 * 不同都会让 6,5 这种偏移对不上）。输入区域是客户端自己声明的
		 * 「可点的窗口体」，GTK 菜单实测就是 6,5 157x203，比 geometry 更
		 * 可信；拿它当模糊范围，客户端换主题也不会偏。 */
		pixman_box32_t *ir = pixman_region32_extents(
			&pb->surface->input_region);
		if (ir != NULL && ir->x2 > ir->x1 && ir->y2 > ir->y1) {
			*x += ir->x1;
			*y += ir->y1;
			*w = ir->x2 - ir->x1;
			*h = ir->y2 - ir->y1;
		} else {
			*w = pb->surface->current.width;
			*h = pb->surface->current.height;
		}
	}
	if (*w <= 0 || *h <= 0) {
		/* 还没 commit 过 / 几何还没算出来：退回表面尺寸。 */
		*w = pb->surface->current.width;
		*h = pb->surface->current.height;
	}
	return *w > 0 && *h > 0;
}

/* 这块弹出物现在归哪块屏管（中心点落在谁身上）。NULL = 整块在屏外。 */
struct vidow_output *popup_blur_owner(struct vidow_popup_blur *pb) {
	int x, y, w, h;
	if (!popup_blur_geom(pb, &x, &y, &w, &h)) {
		return NULL;
	}
	struct wlr_output *wo = wlr_output_layout_output_at(
		pb->server->output_layout, x + w / 2, y + h / 2);
	if (wo == NULL) {
		return NULL;
	}
	return vidow_output_lookup(pb->server, wo);
}

/* 内容整块不透明（格式里没 alpha）时，底下的模糊一个像素都透不出来 —— 跟
 * layer_blur_visible 同一个判据，只是弹出物没有规则 / 动画要额外看。 */
static bool popup_blur_visible(struct vidow_popup_blur *pb) {
	if (pb->view != NULL &&
			(pb->view->mask_opacity || pb->view->anim_alpha < 1.0)) {
		return true;
	}
	if (pb->surface->buffer == NULL) {
		return true; /* 让 wanted 去报「还没缓冲」 */
	}
	return !wlr_buffer_is_opaque(&pb->surface->buffer->base);
}

static bool popup_blur_bail(struct vidow_popup_blur *pb, int reason,
		const char *what) {
	if (pb->blur_fail != reason) {
		pb->blur_fail = reason;
		wlr_log(WLR_DEBUG, "弹出物模糊没画: %s（%s）", what,
			pb->tag != NULL ? pb->tag : "?");
	}
	return false;
}

static bool popup_blur_wanted(struct vidow_server *server,
		struct vidow_popup_blur *pb) {
	if (!server->shader_ok || !server->config.blur_enabled) {
		return popup_blur_bail(pb, 10, "全局模糊关了 / 着色器没就绪");
	}
	if (!server->config.popup_blur_enabled) {
		return popup_blur_bail(pb, 12, "popup_blur_enabled = false");
	}
	if (pb->tree == NULL || pb->hide == NULL || pb->surface == NULL) {
		return popup_blur_bail(pb, 1, "场景 / 表面已经没了");
	}
	if (!pb->hide->enabled) {
		return popup_blur_bail(pb, 2,
			"弹出物节点关着（没映射 / 在 overview 里 / 工作区藏起来了）");
	}
	if (pb->surface->buffer == NULL) {
		return popup_blur_bail(pb, 3, "表面还没有缓冲");
	}
	if (!popup_blur_visible(pb)) {
		return popup_blur_bail(pb, 4, "整块不透明（缓冲没有 alpha）");
	}
	return true;
}

/* 抓底衬时把弹出物自己藏起来。只藏「本来开着」的，回填时也别把本来就该藏着
 * 的东西点亮（跟 layer_blur_capture_visible 一个规矩）。 */
static void popup_blur_hide(struct vidow_popup_blur *pb, bool hide) {
	if (hide) {
		if (pb->hide != NULL && pb->hide->enabled) {
			wlr_scene_node_set_enabled(pb->hide, false);
			pb->capture_hidden = true;
		}
	} else if (pb->capture_hidden) {
		pb->capture_hidden = false;
		if (pb->hide != NULL) {
			wlr_scene_node_set_enabled(pb->hide, true);
		}
	}
}

/* 这块弹出物该用多大一块区域抓底衬（设备像素，相对输出原点）。跟
 * view_region_rect 同一套：往外放一圈模糊半径、原点对齐网格、尺寸只跟弹出物
 * 大小有关（移动时缓冲不重建），再夹在屏内。 */
static bool popup_region_rect(struct vidow_server *server,
		struct vidow_output *output, int x, int y, int w, int h,
		int *rx, int *ry, int *rw, int *rh, int *levels) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int lv = blur_levels_for(server, server->output_w, server->output_h);
	int margin = blur_reach_px(server, lv);
	int left = (int)lround((double)x * scale) - server->output_x - margin;
	int top = (int)lround((double)y * scale) - server->output_y - margin;
	int span_w = (int)lround((double)w * scale) + 2 * margin;
	int span_h = (int)lround((double)h * scale) + 2 * margin;
	const int grid = VIDOW_BLUR_REGION_GRID;
	int gx1 = (int)floor((double)left / grid) * grid;
	int gy1 = (int)floor((double)top / grid) * grid;
	int size_w = (int)ceil((double)(span_w + grid) / grid) * grid;
	int size_h = (int)ceil((double)(span_h + grid) / grid) * grid;
	if (size_w < 1 || size_h < 1 || size_w > MAX_TARGET_PX ||
			size_h > MAX_TARGET_PX) {
		return false;
	}
	int max_x = server->output_w - size_w;
	int max_y = server->output_h - size_h;
	if (max_x < 0) {
		max_x = 0;
	}
	if (max_y < 0) {
		max_y = 0;
	}
	if (gx1 < 0) {
		gx1 = 0;
	} else if (gx1 > max_x) {
		gx1 = max_x;
	}
	if (gy1 < 0) {
		gy1 = 0;
	} else if (gy1 > max_y) {
		gy1 = max_y;
	}
	*rx = gx1;
	*ry = gy1;
	*rw = size_w;
	*rh = size_h;
	*levels = lv;
	return true;
}

/* 抓「弹出物盖住的那一块」：基座从共享底衬上 1:1 拷一块（壁纸 + background /
 * bottom 层），再把窗口 / 别的弹出物 / top / overlay 层画上去。跟
 * layer_region_build 的区别只有一处：抓的时候藏起来的是弹出物自己。 */
static bool popup_region_build(struct vidow_server *server,
		struct vidow_output *output, struct vidow_popup_blur *pb,
		int rx, int ry, int rw, int rh, int levels) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int x1 = rx < 0 ? 0 : rx, y1 = ry < 0 ? 0 : ry;
	int x2 = rx + rw, y2 = ry + rh;
	if (x2 > server->output_w) {
		x2 = server->output_w;
	}
	if (y2 > server->output_h) {
		y2 = server->output_h;
	}
	if (x2 <= x1 || y2 <= y1) {
		return false; /* 整块在屏外，没东西可抓 */
	}
	if (output->backdrop_tex == NULL) {
		return false;
	}

	if (pb->fx.own_bd == NULL || pb->fx.own_bd_w != rw ||
			pb->fx.own_bd_h != rh) {
		own_bd_drop(&pb->fx);
		pb->fx.own_bd = vidow_alloc_target(server, rw, rh);
		if (pb->fx.own_bd == NULL) {
			return false;
		}
		pb->fx.own_bd_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, pb->fx.own_bd));
		if (pb->fx.own_bd_tex == NULL) {
			own_bd_drop(&pb->fx);
			return false;
		}
		pb->fx.own_bd_w = rw;
		pb->fx.own_bd_h = rh;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, pb->fx.own_bd, NULL);
	if (pass == NULL) {
		return false;
	}
	struct wlr_render_rect_options clear = {
		.box = { .x = 0, .y = 0, .width = rw, .height = rh },
		.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_rect(pass, &clear);

	/* 基座：共享底衬里对应的那一块，1:1 捞过来（别插值）。 */
	float one = 1.0f;
	struct wlr_render_texture_options base = {
		.texture = output->backdrop_tex,
		.src_box = {
			.x = (float)x1, .y = (float)y1,
			.width = (float)(x2 - x1), .height = (float)(y2 - y1),
		},
		.dst_box = {
			.x = x1 - rx, .y = y1 - ry,
			.width = x2 - x1, .height = y2 - y1,
		},
		.alpha = &one,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.filter_mode = WLR_SCALE_FILTER_NEAREST,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_texture(pass, &base);

	/* 弹出物自己不能进自己的底衬。 */
	popup_blur_hide(pb, true);
	struct backdrop_ctx ctx = {
		.pass = pass,
		.server = server,
		.scale = scale,
		.ox = (int)lround((double)output->box.x * scale) + x1,
		.oy = (int)lround((double)output->box.y * scale) + y1,
		.clip_x = x1 - rx,
		.clip_y = y1 - ry,
		.clip_w = x2 - x1,
		.clip_h = y2 - y1,
	};
	/* 只抓**弹出物底下**的那几层。弹出物可能挂在窗口那层（layer_popups），
	 * 也可能挂在某块 layer surface 的树里（状态栏菜单，跟着 layer_top /
	 * overlay 走）；挂在 bottom 层的弹出物底下就是共享底衬，一层都不用补抓。
	 * 多抓上面的层会把「其实盖在弹出物上面」的东西画进它的底衬 —— 半透明的
	 * 上面那层一透，就能看见本不该在背后的内容。 */
	bool under_bottom = false, under_top = false, under_overlay = false;
	bool under_im = false;
	struct wlr_scene_node *branch = NULL;
	for (struct wlr_scene_node *n = &pb->tree->node; n != NULL;
			n = n->parent != NULL ? &n->parent->node : NULL) {
		if (n == &server->layer_bottom->node ||
				n == &server->layer_background->node) {
			under_bottom = true;
			break;
		}
		if (n == &server->layer_top->node) {
			under_top = true;
		}
		if (n == &server->layer_overlay->node) {
			under_overlay = true;
		}
		if (n == &server->layer_im->node) {
			under_im = true;
		}
		branch = n;
	}
	struct wlr_scene_tree *trees[4];
	size_t ntrees = 0;
	if (under_bottom) {
		/* bottom / background 层的弹出物：共享底衬里那块 layer surface 可能
		 * 因为「它自己也要模糊」被藏起来了（见 layer_blur_capture_visible），
		 * 所以只补抓它这一支（父表面 + 弹出物，弹出物已经藏着）。 */
		struct wlr_scene_tree *bt =
			branch != NULL ? wlr_scene_tree_from_node(branch) : NULL;
		if (bt != NULL) {
			trees[ntrees++] = bt;
		}
	} else {
		trees[ntrees++] = server->layer_toplevels;
		trees[ntrees++] = server->layer_popups;
		if (under_top || under_overlay || under_im) {
			trees[ntrees++] = server->layer_top;
		}
		if (under_overlay || under_im) {
			trees[ntrees++] = server->layer_overlay;
		}
	}
	for (size_t i = 0; i < ntrees; i++) {
		if (trees[i] != NULL) {
			wlr_scene_node_for_each_buffer(&trees[i]->node,
				backdrop_iter, &ctx);
		}
	}
	popup_blur_hide(pb, false);
	if (!wlr_render_pass_submit(pass)) {
		return false;
	}

	dmg_clear(&pb->fx.pending_damage); /* 这一版的账已经算清了 */
	/* 旧贴图裹着的缓冲马上要被金字塔复用。 */
	blur_own_drop(&pb->fx);
	if (!blur_set_alloc(server, &pb->fx.blur_set, rw, rh, levels)) {
		return false;
	}
	blur_chain(server, &pb->fx.blur_set, pb->fx.own_bd_tex, rw, rh);
	pb->fx.blur_own_tex = pb->fx.blur_set.up_tex[0];
	if (pb->fx.blur_own_tex == NULL) {
		return false;
	}
	pb->fx.blur_set.valid = true;
	pb->fx.blur_own_id = server->blur_serial++;
	pb->bd_x = rx;
	pb->bd_y = ry;
	pb->bd_w = rw;
	pb->bd_h = rh;
	pb->bd_output = output;
	pb->bd_gen_scene = server->gen_scene;
	pb->bd_gen_layer = server->gen_layer;
	wlr_log(WLR_DEBUG, "blur: popup region %s %dx%d at %d,%d levels=%d",
		pb->tag != NULL ? pb->tag : "?", rw, rh, rx, ry, levels);
	return true;
}

/* 把模糊资源还掉，但保留注册项和场景节点（下一帧还要用）。 */
void popup_blur_release(struct vidow_popup_blur *pb) {
	if (pb == NULL) {
		return;
	}
	/* 上一次抓底衬要是半路退出（正常路径都会回填），这里兜底把弹出物放出来。 */
	popup_blur_hide(pb, false);
	if (pb->fx.node != NULL) {
		/* 只在真的有东西的时候碰节点：这个函数每帧都会被叫（弹出物整块
		 * 不透明 / 还没缓冲时），反复 set_buffer(NULL) 会一直报损伤。 */
		if (pb->fx.node->node.enabled) {
			wlr_scene_node_set_enabled(&pb->fx.node->node, false);
		}
		if (pb->fx.node->buffer != NULL) {
			wlr_scene_buffer_set_buffer(pb->fx.node, NULL);
		}
	}
	blur_target_release(&pb->fx);
	drop_targets(&pb->fx);
	pb->fx.blur_drawn = 0;
	pb->bd_w = 0;
	pb->bd_h = 0;
	pb->bd_x = 0;
	pb->bd_y = 0;
	pb->bd_output = NULL;
	pb->bd_gen_scene = 0;
	pb->bd_gen_layer = 0;
}

void popup_blur_frame(struct vidow_server *server,
		struct vidow_output *output, struct vidow_popup_blur *pb) {
	struct vidow_fx_target *fx = &pb->fx;

	if (!popup_blur_wanted(server, pb)) {
		if (fx->node != NULL && fx->node->node.enabled) {
			wlr_scene_node_set_enabled(&fx->node->node, false);
		}
		popup_blur_release(pb);
		return;
	}

	int x, y, w, h;
	if (!popup_blur_geom(pb, &x, &y, &w, &h)) {
		popup_blur_bail(pb, 5, "几何还是 0 / 节点不在场景里");
		if (fx->node != NULL && fx->node->node.enabled) {
			wlr_scene_node_set_enabled(&fx->node->node, false);
		}
		return;
	}
	/* 归别的屏那一趟管：别在这儿建 / 拆缓冲（两块屏会互相把对方的金字塔丢掉）。 */
	struct vidow_output *owner = popup_blur_owner(pb);
	if (owner != output) {
		if (owner == NULL) {
			if (fx->node != NULL && fx->node->node.enabled) {
				wlr_scene_node_set_enabled(&fx->node->node, false);
			}
			popup_blur_release(pb);
		}
		return;
	}
	if (output->backdrop_tex == NULL) {
		popup_blur_bail(pb, 6, "这块屏的共享底衬还没建");
		return;
	}

	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int px_w = (int)lround((double)w * scale);
	int px_h = (int)lround((double)h * scale);
	if (px_w < 1 || px_h < 1 || px_w > MAX_TARGET_PX ||
			px_h > MAX_TARGET_PX) {
		popup_blur_bail(pb, 7, "设备像素尺寸越界");
		return;
	}

	if (fx->node == NULL) {
		fx->node = wlr_scene_buffer_create(pb->tree, NULL);
		if (fx->node == NULL) {
			return;
		}
		fx->node->point_accepts_input = fx_node_rejects_input;
		/* 垫在弹出物表面（和它的子菜单）下面。 */
		wlr_scene_node_lower_to_bottom(&fx->node->node);
		pb->node_destroy.notify = popup_blur_node_destroy;
		wl_signal_add(&fx->node->node.events.destroy, &pb->node_destroy);
		pb->node_destroy_added = true;
	}
	fx->blur_only = true;
	fx->active = true;
	fx->blur = true;
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;
	/* mask 默认整张表面；xdg popup 用输入区域（窗口体）当 mask 范围，
	 * 把 CSD 阴影剔掉。阈值 0.35 是给半透明菜单体留的余量（GTK 菜单体
	 * alpha ≈ 0.45），比全局 0.15 更不容易把阴影/圆角过渡糊进去。 */
	fx->mask_off_x = 0.0f;
	fx->mask_off_y = 0.0f;
	fx->mask_scale_x = 1.0f;
	fx->mask_scale_y = 1.0f;
	fx->mask_min = 1.0f / 255.0f;
	/* 配置里给了统一阈值就用它（0 = 保持各自动的默认值：普通菜单只剔
	 * alpha=0，输入法候选框 0.35）。 */
	if (server->config.popup_blur_alpha_min > 0.0) {
		fx->mask_min = (float)server->config.popup_blur_alpha_min;
		if (fx->mask_min > 1.0f) {
			fx->mask_min = 1.0f;
		}
	}
	if (pb->view == NULL) {
		int sw = pb->surface->current.width;
		int sh = pb->surface->current.height;
		pixman_box32_t *ir = pixman_region32_extents(
			&pb->surface->input_region);
		if (sw > 0 && sh > 0 && ir != NULL && ir->x2 > ir->x1 &&
				ir->y2 > ir->y1) {
			fx->mask_off_x = (float)ir->x1 / (float)sw;
			fx->mask_off_y = (float)ir->y1 / (float)sh;
			fx->mask_scale_x = (float)(ir->x2 - ir->x1) / (float)sw;
			fx->mask_scale_y = (float)(ir->y2 - ir->y1) / (float)sh;
			if (server->config.popup_blur_alpha_min <= 0.0) {
				fx->mask_min = 0.35f;
			}
		}
	}
	blur_mask_min_changed(fx, fx->mask_min);

	/* 节点相对 tree 的坐标：xdg / X11 的树原点就是弹出物原点（0,0）；候选框
	 * 挂在 layer_im 上，得按绝对坐标摆。 */
	int tx = 0, ty = 0;
	if (wlr_scene_node_coords(&pb->tree->node, &tx, &ty)) {
		wlr_scene_node_set_position(&fx->node->node, x - tx, y - ty);
	}

	if (fx->buf[0] == NULL || fx->w != px_w || fx->h != px_h ||
			fabs(fx->scale - scale) > 1e-3) {
		drop_targets(fx);
		for (int i = 0; i < 2; i++) {
			fx->buf[i] = vidow_alloc_target(server, px_w, px_h);
			if (fx->buf[i] == NULL) {
				drop_targets(fx);
				popup_blur_bail(pb, 8, "分配离屏目标失败");
				return;
			}
		}
		fx->w = px_w;
		fx->h = px_h;
		fx->content_w = px_w;
		fx->content_h = px_h;
		fx->px_pad = 0;
		fx->dst_w = w;
		fx->dst_h = h;
		fx->scale = scale;
		fx->blur_drawn = 0;
	}

	/* 位置 / 尺寸变了：模糊的采样坐标跟着变，节点也得重画。 */
	if (pb->last_x != x || pb->last_y != y || pb->last_w != w ||
			pb->last_h != h) {
		pb->last_x = x;
		pb->last_y = y;
		pb->last_w = w;
		pb->last_h = h;
		fx->blur_ver++;
		blur_ver_hits[5954]++;
	}

	int rx, ry, rw, rh, levels;
	if (!popup_region_rect(server, output, x, y, w, h,
			&rx, &ry, &rw, &rh, &levels)) {
		popup_blur_bail(pb, 9, "区域尺寸越界");
		return;
	}

	/* 右 / 下边界 ceil((rx+rw)/scale) 精确盖住，不多加 1 —— 理由同 fx.c
	 * view_region_build：贴屏边的区域多 1px 会探进隔壁屏的布局范围，被
	 * 隔壁屏的损伤流拖着每帧重抓。 */
	int lg_x = output->box.x + (int)floor((double)rx / scale);
	int lg_y = output->box.y + (int)floor((double)ry / scale);
	int lg_r = output->box.x + (int)ceil((double)(rx + rw) / scale);
	int lg_b = output->box.y + (int)ceil((double)(ry + rh) / scale);
	struct wlr_box region_logical = {
		.x = lg_x,
		.y = lg_y,
		.width = lg_r - lg_x,
		.height = lg_b - lg_y,
	};
	bool key_changed = pb->bd_gen_scene != server->gen_scene;
	bool touched = key_changed && blur_damage_hits(fx, &region_logical);
	/* 同 layer_blur_frame 上面那段：**判据里不看 gen_layer**。
	 * 弹出物（菜单 / 输入法候选框）一样会因为「世界动过」而重抓整块底衬，
	 * 而它真正该看的是「我底下那一块像素变没变」= touched。 */
	bool fresh = fx->own_bd != NULL && fx->blur_own_tex != NULL && !touched &&
		pb->bd_output == output &&
		pb->bd_w == rw && pb->bd_h == rh && pb->bd_x == rx &&
		pb->bd_y == ry &&
		fx->blur_set.w == rw && fx->blur_set.h == rh &&
		fx->blur_set.levels == levels &&
		fabs(fx->blur_set.offset - server->config.blur_offset) < 1e-6;
	if (!fresh) {
		if (!popup_region_build(server, output, pb, rx, ry, rw, rh,
				levels)) {
			(void)popup_blur_bail(pb, 11, "私有底衬抓不出来");
			return; /* 抓不出来就留着上一帧那张，别闪 */
		}
		/* 底衬重抓了 = 内容变了，必须重画这一版。同 layer 那条路：
		 * popup_region_build 不换 blur_own_tex / blur_own_id，blur_assign
		 * 逐字段比对全部命中就早退、不推进 blur_ver，下面
		 * blur_drawn == blur_ver 立刻成立 —— 弹出物背后的模糊会一直停在
		 * 第一次画的那张上。 */
		fx->blur_ver++;
		blur_ver_hits[5990]++;
	}
	blur_assign(fx, NULL, fx->blur_own_tex, false, fx->blur_own_id,
		&(struct wlr_box){ .x = rx, .y = ry,
			.width = rw, .height = rh });
	fx->blur_out_w = output->wlr_output->width;
	fx->blur_out_h = output->wlr_output->height;
	fx->blur_out_x = server->output_x;
	fx->blur_out_y = server->output_y;
	if (pb->blur_fail != 0) {
		pb->blur_fail = 0;
	}

	if (fx->blur_drawn == fx->blur_ver) {
		/* 透明度每帧都对齐：抓帧遮挡（mask_opacity）可能在两帧之间变，不能
		 * 因为「模糊图没变」就把一块不透明的模糊留在截屏里。 */
		wlr_scene_buffer_set_opacity(fx->node,
			(pb->view != NULL && pb->view->mask_opacity) ? 0.0f : 1.0f);
		wlr_scene_node_set_enabled(&fx->node->node, true);
		return; /* 已经画的就是这一版 */
	}

	struct wlr_buffer *target = pick_free(fx->buf);
	if (target == NULL) {
		return;
	}
	/* 元素自己的 alpha 当剔除 mask：模糊只画在弹出物真有内容的地方（圆角
	 * 外面 / 自带的透明阴影边不再透出一圈模糊）。 */
	struct wlr_texture *mask = NULL;
	bool mask_owned = false;
	struct wlr_client_buffer *cbuf = pb->surface->buffer;
	if (cbuf != NULL) {
		mask = client_texture(server->renderer, &cbuf->base, &mask_owned);
	}
	/* Diagnostics only: keep the existing draw path, and bound unexpected
	 * texture reports across all popups so a bad buffer cannot flood logs. */
	static unsigned int mask_target_reports = 0;
	if (mask != NULL && mask_target_reports < 8) {
		if (!wlr_texture_is_gles2(mask)) {
			mask_target_reports++;
			wlr_log(WLR_ERROR, "popup mask: %s is not GLES2 (%u/8)",
				pb->tag, mask_target_reports);
		} else {
			struct wlr_gles2_texture_attribs attrs;
			wlr_gles2_texture_get_attribs(mask, &attrs);
			if (attrs.target != GL_TEXTURE_2D) {
				mask_target_reports++;
				wlr_log(WLR_ERROR,
					"popup mask: %s target=0x%x, expected GL_TEXTURE_2D (%u/8)",
					pb->tag, (unsigned int)attrs.target, mask_target_reports);
			}
		}
	}
	static const float no_border[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	struct vidow_look look = {0};
	look.opacity = 1.0;
	look.blur = true;
	look.decor = false;
	decor_draw(server, fx, target, server->empty_tex, mask, no_border,
		x, y, 1.0, scale, &look);
	if (mask_owned && mask != NULL) {
		wlr_texture_destroy(mask);
	}

	wlr_scene_buffer_set_dest_size(fx->node, w, h);
	wlr_scene_buffer_set_opacity(fx->node,
		(pb->view != NULL && pb->view->mask_opacity) ? 0.0f : 1.0f);
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, w, h);
	wlr_scene_buffer_set_buffer_with_damage(fx->node, target, &damage);
	pixman_region32_fini(&damage);
	wlr_scene_node_set_enabled(&fx->node->node, true);
	fx->blur_drawn = fx->blur_ver;
}

/* 节点跟着场景树一起没了（xdg popup 的树 / X11 的表面树销毁）：把注册项摘掉，
 * 别再拿着一个已经释放的指针。 */
static void popup_blur_node_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_blur *pb =
		wl_container_of(listener, pb, node_destroy);
	pb->node_destroy_added = false;
	wl_list_remove(&pb->node_destroy.link);
	pb->fx.node = NULL;
	vidow_popup_blur_unregister(pb, false);
}

/* hide 那个节点没了。**必挂**（见 server.h 里 hide_destroy 的注释）：
 * 它是弹出物的场景节点，弹出物一关就先没，而 pb 可能还活着。
 *
 * 处理方式跟 node_destroy 一样 —— 整个 pb 摘掉，不要再试图用它：hide 没了就
 * 意味着弹出物本体没了，模糊也就没有意义了（留着只会继续拿野指针去算几何）。
 * 注意 pb->fx.node 是**模糊节点**，跟 hide 是两个东西，交给 unregister 去清。 */
static void popup_blur_hide_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_blur *pb =
		wl_container_of(listener, pb, hide_destroy);
	pb->hide_destroy_added = false;
	wl_list_remove(&pb->hide_destroy.link);
	/* 先自己清指针：unregister 里不会再碰它。 */
	pb->hide = NULL;
	vidow_popup_blur_unregister(pb, false);
}

static void popup_blur_owner_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_blur *pb =
		wl_container_of(listener, pb, owner_destroy);
	pb->owner_destroy_added = false;
	wl_list_remove(&pb->owner_destroy.link);
	vidow_popup_blur_unregister(pb, false);
}

static void popup_blur_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_popup_blur *pb = wl_container_of(listener, pb, commit);
	vidow_popup_blur_invalidate(pb);
}

struct vidow_popup_blur *vidow_popup_blur_register(struct vidow_server *server,
		struct wlr_scene_tree *tree, struct wlr_scene_node *hide,
		struct wlr_surface *surface, struct vidow_view *view, const char *tag,
		struct wl_signal *owner_destroy) {
	if (server == NULL || tree == NULL || hide == NULL || surface == NULL) {
		return NULL;
	}
	struct vidow_popup_blur *pb = calloc(1, sizeof(*pb));
	if (pb == NULL) {
		return NULL;
	}
	pb->server = server;
	pb->tree = tree;
	pb->hide = hide;
	pb->surface = surface;
	pb->view = view;
	pb->tag = tag != NULL ? tag : "?";
	pb->last_w = -1; /* 第一帧一定重画 */
	pb->last_h = -1;
	wl_list_insert(&server->popup_blurs, &pb->link);
	if (owner_destroy != NULL) {
		pb->owner_destroy.notify = popup_blur_owner_destroy;
		wl_signal_add(owner_destroy, &pb->owner_destroy);
		pb->owner_destroy_added = true;
	}
	/* hide 节点的销毁必须盯住：它是弹出物的场景节点，弹出物关掉时先没，
	 * 而 pb 可能还活着（宿主是另一个对象）。不盯就是野指针 —— 见
	 * server.h 里 hide_destroy 的注释和 2026-10-21 那次 kate 崩溃。 */
	pb->hide_destroy.notify = popup_blur_hide_destroy;
	wl_signal_add(&hide->events.destroy, &pb->hide_destroy);
	pb->hide_destroy_added = true;

	pb->commit.notify = popup_blur_commit;
	wl_signal_add(&surface->events.commit, &pb->commit);
	pb->commit_added = true;
	wlr_log(WLR_DEBUG, "popup blur: 登记 %s", pb->tag);
	return pb;
}

void vidow_popup_blur_unregister(struct vidow_popup_blur *pb,
		bool destroy_node) {
	if (pb == NULL) {
		return;
	}
	/* 先把宿主手里的指针摘掉：节点 / 表面先没的时候，宿主之后还会拿它来用。 */
	if (pb->view != NULL && pb->view->popup_blur == pb) {
		pb->view->popup_blur = NULL;
	}
	if (pb->server != NULL && pb->server->im_popup.blur == pb) {
		pb->server->im_popup.blur = NULL;
	}
	if (pb->commit_added) {
		wl_list_remove(&pb->commit.link);
		pb->commit_added = false;
	}
	if (pb->owner_destroy_added) {
		wl_list_remove(&pb->owner_destroy.link);
		pb->owner_destroy_added = false;
	}
	if (pb->hide_destroy_added) {
		wl_list_remove(&pb->hide_destroy.link);
		pb->hide_destroy_added = false;
	}
	if (pb->node_destroy_added) {
		wl_list_remove(&pb->node_destroy.link);
		pb->node_destroy_added = false;
	}
	if (pb->fx.node != NULL) {
		if (destroy_node) {
			wlr_scene_node_destroy(&pb->fx.node->node);
		} else {
			/* 节点归场景树（xdg popup）：只放资源，别碰它 —— 树可能正跟着
			 * 表面一起拆。 */
			pb->fx.node = NULL;
		}
	}
	vidow_fx_release(&pb->fx);
	wl_list_remove(&pb->link);
	free(pb);
}

void vidow_popup_blur_invalidate(struct vidow_popup_blur *pb) {
	if (pb == NULL) {
		return;
	}
	pb->fx.blur_ver++;
	blur_ver_hits[6194]++;
	struct vidow_output *out = popup_blur_owner(pb);
	if (out != NULL) {
		vidow_output_schedule_frame(pb->server, out->wlr_output,
			"弹出物模糊失效");
	}
}
