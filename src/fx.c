#include "server.h"
#include "fx-internal.h"
#include "fx-gl.h"
#include "fx-layer.h"
#include "fx-popup.h"
#include "xtrace.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/egl.h>
#include <wlr/render/gles2.h>
#include <wlr/render/drm_syncobj.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/* wlroots 0.20 exports this symbol but the Arch package ships no prototype. */
extern const struct wlr_drm_format_set *wlr_renderer_get_render_formats(
	struct wlr_renderer *r, uint32_t buffer_caps);

/* wlroots exports these helpers but wlroots-0.20 does not install their
 * unstable prototypes. Keep the declarations local, as with the render
 * format helper above. */
extern EGLSyncKHR wlr_egl_create_sync(struct wlr_egl *egl, int fence_fd);
extern void wlr_egl_destroy_sync(struct wlr_egl *egl, EGLSyncKHR sync);
extern bool wlr_egl_wait_sync(struct wlr_egl *egl, EGLSyncKHR sync);

#define MAX_SHADER_SIZE (256 * 1024)

/* 临时探针：blur_ver++ 是哪一个调用点推的（索引 = 源码行号）。静止桌面
 * 每帧重烘全部装饰时，这几个计数直接指出是「贴图换了」还是「视野变了」。 */
int blur_ver_hits[6400];

void vidow_fx_dump_blur_ver_hits(void) {
	char buf[512];
	size_t off = 0;
	static const int watch[] = {2665, 2686, 2695, 3276, 4415, 4431, 5360, 5954,
		5990, 6194, 6347};
	for (size_t i = 0; i < sizeof(watch) / sizeof(watch[0]); i++) {
		int n = blur_ver_hits[watch[i]];
		if (n == 0) {
			continue;
		}
		off += snprintf(buf + off, sizeof(buf) - off, "%d:%d ", watch[i], n);
		blur_ver_hits[watch[i]] = 0;
	}
	buf[off] = '\0';
	if (off > 0) {
		wlr_log(WLR_DEBUG, "BLURVER %s", buf);
	}
}

/* ---- 渲染统计（VIDOW_RENDER_STATS=1，见 output.c）：每帧各阶段耗时 + 重建次数。
 * 计数是机器无关的硬指标 —— 「有背景时拖拽卡」这类问题，先在真机上跑
 * `VIDOW_RENDER_STATS=1 vidow` 看每帧干了多少活，再谈优化。
 *
 * tex_new/tex_del 数的是 vidow **自己那些目标缓冲**的贴图导入/销毁：真后端上
 * （GBM/dmabuf）每次都是一次 EGLImage 创建 + BO 映射，每帧十几次就够呛。客户端
 * 缓冲走的是 wlr_client_buffer 自己那张，不算在内。 */
struct vidow_frame_stats vidow_stats;
struct wlr_texture *stat_tex_new(struct wlr_texture *t) {
	if (t != NULL) {
		vidow_stats.tex_new++;
	}
	return t;
}
void stat_tex_del(struct wlr_texture *t) {
	if (t != NULL) {
		vidow_stats.tex_del++;
	}
	wlr_texture_destroy(t);
}
/* ------------------------------------------------------------ scene alpha */

/* 透明度一律夹在 [0,1]：wlroots 那边是 assert(opacity >= 0 && opacity <= 1)，
 * 直接 abort。缓动曲线是**会过冲**的（easing = "out_back 0.18" 冲到 1.18，
 * out_elastic 更夸张），把过冲的进度喂进透明度就当场炸 —— 2026-10-13 20:39 那次
 * core dump 就是这条路：#6 alpha_iter ← #8 vidow_fx_set_alpha ← #9 layer_apply_anim
 * （layer 的入场/消失动画把缓动进度当 alpha 用）。透明度本身不该过冲。 */
float clamp01(double v) {
	if (v < 0.0) return 0.0f;
	if (v > 1.0) return 1.0f;
	return (float)v;
}

static void alpha_iter(struct wlr_scene_buffer *buffer, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	wlr_scene_buffer_set_opacity(buffer, clamp01(*(double *)data));
}

/* Multiply every buffer of a subtree by `alpha`. Scene-graph opacity is a
 * compositor-side property, so this never disturbs what clients draw. */
void vidow_fx_set_alpha(struct wlr_scene_node *node, double alpha) {
	if (alpha < 0.0) alpha = 0.0;
	if (alpha > 1.0) alpha = 1.0;
	wlr_scene_node_for_each_buffer(node, alpha_iter, &alpha);
}



static void view_bake_for_output(struct vidow_server *server,
		struct vidow_output *output);

int vidow_fx_pad(struct vidow_server *server) {
	struct vidow_config *cfg = &server->config;
	if (!cfg->decor_enabled || !server->shader_ok) {
		return 0;
	}
	int off = abs(cfg->shadow_offset_x) > abs(cfg->shadow_offset_y) ?
		abs(cfg->shadow_offset_x) : abs(cfg->shadow_offset_y);
	double radius = cfg->shadow_radius;
	/* 窗口规则可以把某个窗口的阴影调大：内边距按「所有规则里最大的那个」留
	 * （多留一点只是离屏缓冲大一圈，少留就是阴影被裁掉）。规则没写 shadow_*
	 * 的时候这一步等于没做。 */
	const struct vidow_rule *rule;
	wl_list_for_each(rule, &server->rules, link) {
		if (rule->aset[VRA_SHADOW_RADIUS] &&
				rule->aval[VRA_SHADOW_RADIUS].d > radius) {
			radius = rule->aval[VRA_SHADOW_RADIUS].d;
		}
		if (rule->aset[VRA_SHADOW_OFFSET_X]) {
			long long v = rule->aval[VRA_SHADOW_OFFSET_X].i;
			if (labs(v) > off) {
				off = (int)labs(v);
			}
		}
		if (rule->aset[VRA_SHADOW_OFFSET_Y]) {
			long long v = rule->aval[VRA_SHADOW_OFFSET_Y].i;
			if (labs(v) > off) {
				off = (int)labs(v);
			}
		}
	}
	int pad = (int)(radius + off + 2);
	if (pad < 0) {
		pad = 0;
	}
	if (pad > 96) {
		pad = 96;
	}
	return pad;
}

/* 这一帧的序号：共享的 layer 底衬「同一帧只抓一次」。每跑一遍 blur update（每块屏
 * 每帧一次）加一。 */
uint64_t blur_frame_seq = 0;

static void layer_under_drop(struct vidow_output *output) {
	if (output->under_tex != NULL) {
		stat_tex_del(output->under_tex);
		output->under_tex = NULL;
	}
	if (output->under_bd != NULL) {
		wlr_buffer_drop(output->under_bd);
		output->under_bd = NULL;
	}
	output->under_seq = 0;
	output->under_box = (struct wlr_box){0};
	output->under_share = false;
}

void blur_drop(struct vidow_output *output) {
	layer_under_drop(output);
	if (output->backdrop_tex != NULL) {
		stat_tex_del(output->backdrop_tex);
		output->backdrop_tex = NULL;
	}
	if (output->backdrop != NULL) {
		wlr_buffer_drop(output->backdrop);
		output->backdrop = NULL;
		output->backdrop_w = 0;
		output->backdrop_h = 0;
	}
}

void blur_set_drop(struct vidow_blur_set *set) {
	for (int i = 0; i < MAX_BLUR_LEVELS; i++) {
		if (set->down[i] != NULL) {
			wlr_buffer_drop(set->down[i]);
			set->down[i] = NULL;
		}
		if (set->up[i] != NULL) {
			wlr_buffer_drop(set->up[i]);
			set->up[i] = NULL;
		}
		if (set->down_tex[i] != NULL) {
			stat_tex_del(set->down_tex[i]);
			set->down_tex[i] = NULL;
		}
		if (set->up_tex[i] != NULL) {
			stat_tex_del(set->up_tex[i]);
			set->up_tex[i] = NULL;
		}
	}
	set->w = 0;
	set->h = 0;
	set->levels = 0;
	set->gen = 0;
	set->offset = 0.0;
	set->valid = false;
}

bool blur_set_alloc(struct vidow_server *server,
		struct vidow_blur_set *set, int w, int h, int levels) {
	/* 形状没变就**复用**那几块缓冲：拖窗口时重叠的窗口每帧都要重建金字塔
	 * （另一个窗口在它的底衬里动），而 vidow_alloc_target() 走的是
	 * wlr_allocator_create_buffer() —— 真后端上每次都是内核层的 GBM 分配。
	 * 每帧重新分十来块，光分配就够卡了（用户报的「有背景时拖拽卡顿」）。
	 * 复用是安全的：整块重跑时每一趟都会把每块缓冲整个写一遍（begin_pass 先
	 * clear），局部重跑（blur_chain_rect）时**故意**不动那一块以外的像素 ——
	 * 那正是「其余从上一版拷过来」的省法。调用方重画之前已经放掉了攥着 up[0]
	 * 的那张贴图。 */
	if (set->w == w && set->h == h && set->levels == levels &&
			set->down[0] != NULL) {
		set->offset = server->config.blur_offset;
		return true;
	}
	blur_set_drop(set);
	for (int i = 0; i < levels; i++) {
		set->down[i] = vidow_alloc_target(server, w >> (i + 1), h >> (i + 1));
		if (set->down[i] == NULL) {
			blur_set_drop(set);
			return false;
		}
		set->down_tex[i] = stat_tex_new(wlr_texture_from_buffer(
			server->renderer, set->down[i]));
		if (set->down_tex[i] == NULL) {
			blur_set_drop(set);
			return false;
		}
	}
	/* up[i] 和 down[i] 同尺寸：up[i] 是「把 up[i+1]（或者最底层 down[levels-1]）
	 * 放大一圈」的结果，up[0]（半分辨率）就是最终产物，也是给装饰着色器采样的
	 * 那张。down[0..levels-2] 只是链条中途的暂存。 */
	for (int i = 0; i < levels - 1; i++) {
		set->up[i] = vidow_alloc_target(server, w >> (i + 1), h >> (i + 1));
		if (set->up[i] == NULL) {
			blur_set_drop(set);
			return false;
		}
		set->up_tex[i] = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, set->up[i]));
		if (set->up_tex[i] == NULL) {
			blur_set_drop(set);
			return false;
		}
	}
	set->w = w;
	set->h = h;
	set->levels = levels;
	set->offset = server->config.blur_offset;
	return true;
}

/* 一趟 kawase。rect == NULL：整块目标都画（u_rect = 整个目标）；rect != NULL：
 * 只画目标里这一块（别处的像素一个都不碰，见 blur_chain_rect）。 */
static void kawase_pass_rect(struct vidow_server *server,
		struct vidow_gl_prog *prog, struct wlr_buffer *dst, int dst_w, int dst_h,
		struct wlr_texture *src, int src_w, int src_h,
		const struct wlr_box *rect) {
	if (prog->program == 0 || src == NULL) {
		return;
	}
	struct wlr_egl *egl = NULL;
	if (begin_pass_rect(server, dst, dst_w, dst_h, rect, &egl) == 0) {
		return;
	}
	struct wlr_gles2_texture_attribs sa;
	wlr_gles2_texture_get_attribs(src, &sa);

	glUseProgram(prog->program);
	bind_tex(0, sa.target, sa.tex);
	glUniform1i(prog->u_tex, 0);
	glUniform2f(prog->u_texel, 1.0f / (float)src_w, 1.0f / (float)src_h);
	glUniform1f(prog->u_offset, (float)server->config.blur_offset);
	/* 模糊金字塔在线性光里算（color.linear_blur）。底衬是屏幕编码（gamma22），
	 * 所以每趟的源和目标都用同一条传输函数 —— 中间每一级写回时编码，链条
	 * 本身仍然是「屏幕编码」，下游（装饰 / 底衬拼贴 / 果冻）不用改。 */
	glUniform1f(prog->u_linear,
		server->config.color_linear_blur ? 1.0f : 0.0f);
	glUniform1f(prog->u_tf, 2.0f); /* gamma2.2 */
	glUniform2f(prog->u_dst, (float)dst_w, (float)dst_h);
	if (rect != NULL) {
		glUniform4f(prog->u_rect, (float)rect->x, (float)rect->y,
			(float)rect->width, (float)rect->height);
	} else {
		glUniform4f(prog->u_rect, 0.0f, 0.0f, (float)dst_w, (float)dst_h);
	}
	draw_quad(prog->program);
	glUseProgram(0);
	end_pass(egl);
}

/* 把 src 坐标系里的矩形换算到 dst 坐标系（尺寸比就是缩放比，这里是 2 或 1/2），
 * 往外取整、再放 margin 一圈、夹进目标里。往外取整和 margin 都是「宁可多算一点，
 * 不能少算」：少算一像素就是新旧模糊值之间一道缝。 */
static struct wlr_box kawase_rect_scale(const struct wlr_box *r,
		int sw, int sh, int dw, int dh, int margin) {
	if (sw <= 0 || sh <= 0) {
		return (struct wlr_box){0};
	}
	double fx = (double)dw / (double)sw;
	double fy = (double)dh / (double)sh;
	int x1 = (int)floor((double)r->x * fx) - margin;
	int y1 = (int)floor((double)r->y * fy) - margin;
	int x2 = (int)ceil((double)(r->x + r->width) * fx) + margin;
	int y2 = (int)ceil((double)(r->y + r->height) * fy) + margin;
	if (x1 < 0) {
		x1 = 0;
	}
	if (y1 < 0) {
		y1 = 0;
	}
	if (x2 > dw) {
		x2 = dw;
	}
	if (y2 > dh) {
		y2 = dh;
	}
	if (x2 <= x1 || y2 <= y1) {
		return (struct wlr_box){0};
	}
	return (struct wlr_box){
		.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1,
	};
}

/* 跑一遍链条。rect == NULL：整块（老行为）。rect != NULL：只有这块区域里的
 * **像素内容变了**，别处保持上一版 —— 每一级只重画「上一级变了的那一块缩一半再
 * 放一圈」的矩形，其余像素不动。
 *
 * margin 用的是整条链的采样半径按级别折算（blur_reach_px >> (i+1)）+ 4：真实需要
 * 的半径小得多（u_offset 个源 texel，1.5 个像素级），这是**够用且宽松**的上界 ——
 * 局部重跑绝不能因为margin 算少了而在新旧模糊值之间留一道缝。 */
void blur_chain_rect(struct vidow_server *server,
		struct vidow_blur_set *set, struct wlr_texture *backdrop_tex,
		int w, int h, const struct wlr_box *rect, int reach) {
	int levels = set->levels;
	if (levels < 2 || backdrop_tex == NULL) {
		return;
	}
	vidow_stats.chains++;
	struct wlr_box r = rect != NULL ? *rect : (struct wlr_box){0};
	struct wlr_texture *tex = backdrop_tex;
	for (int i = 0; i < levels; i++) {
		int sw = i == 0 ? w : (w >> i), sh = i == 0 ? h : (h >> i);
		int dw = w >> (i + 1), dh = h >> (i + 1);
		struct wlr_box dst_rect = {0};
		if (rect != NULL) {
			dst_rect = kawase_rect_scale(&r, sw, sh, dw, dh,
				(reach >> (i + 1)) + 4);
			if (dst_rect.width <= 0 || dst_rect.height <= 0) {
				return; /* 越缩越小：下面几级也没东西可算 */
			}
		}
		kawase_pass_rect(server, &server->kawase_down, set->down[i], dw, dh,
			tex, sw, sh, rect != NULL ? &dst_rect : NULL);
		/* 这一趟的结果就是下一趟的源：贴图是常驻的（见 vidow_blur_set），
		 * 直接换指针 —— 以前这里每趟导入一张、用完销毁，真后端上每帧十几二十次
		 * EGLImage + BO 映射，拖重叠窗口时的卡顿就是这个。 */
		tex = set->down_tex[i];
		r = dst_rect;
	}

	/* 往上走：每一级只把上一级的结果放大一圈（niri 的 blur_up）。目标必须比源大一
	 * 倍，u_texel 传的是**源**的尺寸 —— 放大倍数全靠它算。每一级都是同一趟着色器，
	 * 写 up[0]（半分辨率、给装饰着色器采样的那张）也不例外。 */
	struct wlr_texture *lower = set->down_tex[levels - 1];
	for (int i = levels - 2; i >= 0; i--) {
		int dw = w >> (i + 1), dh = h >> (i + 1);
		int sw = w >> (i + 2), sh = h >> (i + 2);
		struct wlr_box dst_rect = {0};
		if (rect != NULL) {
			dst_rect = kawase_rect_scale(&r, sw, sh, dw, dh,
				(reach >> (i + 1)) + 4);
			if (dst_rect.width <= 0 || dst_rect.height <= 0) {
				return;
			}
		}
		kawase_pass_rect(server, &server->kawase_up, set->up[i], dw, dh,
			lower, sw, sh, rect != NULL ? &dst_rect : NULL);
		lower = set->up_tex[i];
		r = dst_rect;
	}
}

void blur_chain(struct vidow_server *server, struct vidow_blur_set *set,
		struct wlr_texture *backdrop_tex, int w, int h) {
	blur_chain_rect(server, set, backdrop_tex, w, h, NULL, 0);
}

/* Render the layer trees below the toplevels into the backdrop buffer. Doing
 * this by hand instead of via wlr_scene_output_build_state() keeps the output's
 * damage ring untouched and lets us pick exactly which trees contribute. */

/* 局部更新要求「只画一块」这件事对每个要抓的 buffer 都成立：转过的客户端缓冲
 * （wl_surface.set_buffer_transform）得把 dst 子矩形映射回转置过的 src 框，不值得
 * 为它写一套。碰到就整块重跑。 */
void backdrop_clip_check(struct wlr_scene_buffer *sb, int sx, int sy,
		void *data) {
	(void)sx;
	(void)sy;
	bool *ok = data;
	if (sb->buffer != NULL && sb->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		*ok = false;
	}
}

void backdrop_iter(struct wlr_scene_buffer *sb, int sx, int sy,
		void *data) {
	struct backdrop_ctx *ctx = data;
	if (sb->buffer == NULL) {
		return;
	}
	/* 抓某一层的底衬时，只要「它底下」的层（见 backdrop_ctx.below_layer）。 */
	if (ctx->below_layer >= 0) {
		struct vidow_layer_surface *ls;
		const struct vidow_layer_surface *owner = NULL;
		wl_list_for_each(ls, &ctx->server->layers, link) {
			if (ls->scene == NULL) {
				continue;
			}
			/* 这个 buffer 是不是挂在这块 layer surface 的树底下（一路往上
			 * 找父节点直到那棵树）。场景图里子表面 / 装饰都挂在下面。 */
			for (struct wlr_scene_node *n = &sb->node; n != NULL;
					n = n->parent != NULL ? &n->parent->node : NULL) {
				if (n == &ls->scene->tree->node) {
					owner = ls;
					break;
				}
			}
			if (owner != NULL) {
				break;
			}
		}
		/* 挂在这一层或更高的层上 → 不画（它自己也是「更高或等于」）。 */
		if (owner != NULL && (int)owner->layer >= ctx->below_layer) {
			return;
		}
	}
	bool skip_self = false;
	if (ctx->skip != NULL && ctx->skip->scene_tree != NULL) {
		for (struct wlr_scene_node *n = &sb->node; n != NULL;
				n = n->parent != NULL ? &n->parent->node : NULL) {
			if (n == &ctx->skip->scene_tree->node) {
				skip_self = true;
				break;
			}
		}
	}
	if (ctx->skip != NULL && (skip_self ||
			sb == ctx->skip->fx.node ||
			(ctx->skip->scene_surface != NULL &&
			 sb == ctx->skip->scene_surface->buffer))) {
		return;
	}
	/* 节点尺寸优先用场景给的目标尺寸（逻辑像素）；客户端缓冲自己带倍率的
	 * 情况下（缓冲 1280x720、逻辑 640x360）退到 surface 的逻辑尺寸，
	 * 别把设备像素当逻辑像素用。 */
	int w = sb->dst_width > 0 ? sb->dst_width : 0;
	int h = sb->dst_height > 0 ? sb->dst_height : 0;
	if (w <= 0 || h <= 0) {
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
		if (ss != NULL) {
			w = ss->surface->current.width;
			h = ss->surface->current.height;
		} else {
			w = sb->buffer->width;
			h = sb->buffer->height;
		}
	}
	if (w <= 0 || h <= 0) {
		return;
	}
	/* 按**边**取整（而不是位置 + 宽度各自取整），相邻的两块 buffer 之间就不会
	 * 因为各自四舍五入而露出一像素缝。 */
	int x1 = (int)lround((double)sx * ctx->scale) - ctx->ox;
	int y1 = (int)lround((double)sy * ctx->scale) - ctx->oy;
	int x2 = (int)lround((double)(sx + w) * ctx->scale) - ctx->ox;
	int y2 = (int)lround((double)(sy + h) * ctx->scale) - ctx->oy;
	if (x2 <= x1 || y2 <= y1) {
		return;
	}
	if (ctx->clip_w > 0 &&
			(x1 >= ctx->clip_x + ctx->clip_w || x2 <= ctx->clip_x ||
			 y1 >= ctx->clip_y + ctx->clip_h || y2 <= ctx->clip_y)) {
		return; /* 整块都在区域外：省下一次建贴图 */
	}
	/* 也是客户端/别人的缓冲：同样借 wrapper 的贴图（见 client_texture 的说明，
	 * dmabuf 客户端对 source 另建的那张会一直是旧内容）。 */
	bool tex_owned = true;
	struct wlr_texture *tex = client_scene_texture(ctx->server, sb, &tex_owned);
	if (tex == NULL) {
		return;
	}
	float alpha = sb->opacity;
	/* 只采客户端声明的有效区域（wp_viewport 的 src 框，没设就是整块缓冲）。
	 * 不采的话，缓冲比有效内容大一圈的客户端（chromium 分数缩放）会把没画过的
	 * 那圈像素也画进底衬 —— 后面模糊采到的就是那圈黑。 */
	struct wlr_fbox sbox = sb->src_box;
	if (sbox.width <= 0.0 || sbox.height <= 0.0) {
		sbox = (struct wlr_fbox){0, 0, (double)sb->buffer->width,
			(double)sb->buffer->height};
	}
	/* 只画 clip 里那一块。局部更新（clip = 受损那一小块）时必须真的夹住：底衬
	 * 缓冲里 clip 外面是上一版的内容、没清过，整块重画一遍压在它上面，半透明的
	 * 东西（窗口自己的阴影、薄纱窗口）就会被叠第二次 —— 屏幕上就是「局部更新之后
	 * 那一圈变深了」。夹 dst 框就得按同样的比例夹 src 框，不然画面会跟着缩。 */
	if (ctx->clip_w > 0 &&
			(x1 < ctx->clip_x || y1 < ctx->clip_y ||
			 x2 > ctx->clip_x + ctx->clip_w ||
			 y2 > ctx->clip_y + ctx->clip_h)) {
		int cx1 = x1 > ctx->clip_x ? x1 : ctx->clip_x;
		int cy1 = y1 > ctx->clip_y ? y1 : ctx->clip_y;
		int cx2 = x2 < ctx->clip_x + ctx->clip_w ? x2 : ctx->clip_x + ctx->clip_w;
		int cy2 = y2 < ctx->clip_y + ctx->clip_h ? y2 : ctx->clip_y + ctx->clip_h;
		if (cx2 <= cx1 || cy2 <= cy1) {
			if (tex_owned) {
				wlr_texture_destroy(tex);
			}
			return;
		}
		double fx1 = (double)(cx1 - x1) / (double)(x2 - x1);
		double fx2 = (double)(cx2 - x1) / (double)(x2 - x1);
		double fy1 = (double)(cy1 - y1) / (double)(y2 - y1);
		double fy2 = (double)(cy2 - y1) / (double)(y2 - y1);
		sbox.x += fx1 * sbox.width;
		sbox.y += fy1 * sbox.height;
		sbox.width *= fx2 - fx1;
		sbox.height *= fy2 - fy1;
		x1 = cx1;
		y1 = cy1;
		x2 = cx2;
		y2 = cy2;
	}
	struct wlr_render_texture_options opts = {
		.texture = tex,
		.src_box = sbox,
		.dst_box = { .x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1 },
		.alpha = &alpha,
		.transform = sb->transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_texture(ctx->pass, &opts);
	if (tex_owned) {
		wlr_texture_destroy(tex);
	}
}

bool backdrop_render(struct vidow_server *server,
		struct vidow_output *output, int w, int h) {
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, output->backdrop, NULL);
	if (pass == NULL) {
		return false;
	}
	struct wlr_render_rect_options clear = {
		.box = { .x = 0, .y = 0, .width = w, .height = h },
		.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_rect(pass, &clear);

	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	struct backdrop_ctx ctx = {
		.pass = pass,
		.server = server,
		.scale = scale,
		.ox = (int)lround((double)output->box.x * scale),
		.oy = (int)lround((double)output->box.y * scale),
		/* -1 = 不按层号筛：窗口的底衬要整屏（含壁纸那一层）。 */
		.below_layer = -1,
	};
	struct wlr_scene_tree *trees[] = {
		server->layer_background, server->layer_bottom, server->layer_toplevels,
	};
	for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		wlr_scene_node_for_each_buffer(&trees[i]->node, backdrop_iter, &ctx);
	}
	return wlr_render_pass_submit(pass);
}

/* 窗口的内容是不是**整块不透明**。两种确定的情形：
 *   1. 格式里没 alpha（XRGB / RGBX）：每个像素的 alpha 都是 1；
 *   2. 客户端声明了盖住可见窗口那一块的 opaque region（GTK / Qt 那套）：opaque
 *      region 就是「这里 alpha == 1」的承诺，坐标是表面局部（wlroots 在
 *      types/scene/surface.c 里按表面尺寸设的），所以拿窗口在表面里的那块比。
 * 两个地方要问这个：直通（vidow_view_scanout_ok —— 底下画什么一个像素都透不出来）
 * 和模糊（view_blur_visible —— 模糊被 content.a 乘成 0）。 */
static bool view_content_opaque(struct vidow_view *view,
		const struct vidow_fx_geo *geo) {
	struct wlr_scene_buffer *src = view->scene_surface != NULL ?
		view->scene_surface->buffer : NULL;
	if (src == NULL || src->buffer == NULL) {
		return false;
	}
	if (wlr_buffer_is_opaque(src->buffer)) {
		return true;
	}
	pixman_box32_t want = {
		.x1 = geo->off_x, .y1 = geo->off_y,
		.x2 = geo->off_x + geo->w, .y2 = geo->off_y + geo->h,
	};
	return pixman_region32_contains_rectangle(&src->opaque_region, &want) ==
		PIXMAN_REGION_IN;
}

/* A window only needs blur when something can show through it. Floating
 * windows always can; tiled ones only when blur_all is set, because their
 * backdrop is the wallpaper and the layer surfaces. */
static bool view_wants_blur_ever(struct vidow_view *view) {
	struct vidow_look look;
	vidow_view_look(&view->server->config, &view->rule_eff, view->fullscreen,
		&look);
	/* blur 那条规则在这里生效：false = 这个窗口背后不模糊（视频、终端那种
	 * 「糊了更难看」的），true = 平铺也模糊（等于给这一个窗口开 blur_all）。 */
	if (!look.blur || !view->mapped) {
		return false;
	}
	/* overview 里不采样背景模糊：UV 是按窗口自己的位置算的，窗口缩小之后对不上。
	 * 注意是「这一刻不给贴图」而不是把 fx->blur 关掉 —— 那个标志是 fx_begin()
	 * 按配置设的，改它的话退出 overview 之后就再也恢复不了（踩过：overview 一趟
	 * 回来，所有窗口的装饰底衬都不模糊了）。 */
	if (view->rule_eff.set[VRA_BLUR] ||
			view->server->config.blur_all ||
			view->float_mode != VIDOW_FLOAT_NONE) {
		return true;
	}
	/* 客户端自己开口要的（ext-background-effect），blur_all 关着也得给 */
	return view->fx.blur_region_on;
}

/* 给 drag.c 用：这个窗口会不会有模糊底衬。拖拽时模糊窗口保持 alpha 1.0 ——
 * 降到 0.75 会让烘好的模糊和未模糊的场景混成「一层雾」（用户报过）。
 * 判据不能只看 fx.blur_tex：刚 map / 刚换屏时那张贴图可能还没分配，浮窗
 * 拖动正好会踩到，所以按配置 / 规则算。 */
bool vidow_view_blur_wanted(struct vidow_view *view) {
	return view != NULL && view->fx.active && view_wants_blur_ever(view);
}

/* 这笔模糊看得见吗 —— 看不见就别算（整屏 Kawase 金字塔 + 私有底衬抓取 +
 * 「底衬版本变了就重烘这个窗口」全是白花的钱）。
 *
 * 装饰着色器最后两句是 out_px = content + out_px * (1 - content.a)，而模糊那一项
 * 还乘着 inside 和 step(u_mask_min, elem_a)：**内容不透明的地方（content.a == 1）
 * 模糊被乘成 0，内容全透明的地方又被剔除**（模糊只画在元素真有内容的地方）。所以
 * 模糊只在**内容半透明**的窗口上看得见 —— 终端背景 0.8、播放器控制条、半透明
 * 圆角窗口那些才是真的要看模糊的。
 *
 * 判据保守：**拿不准就当看得见**（照旧算模糊，行为不变）。判成看不见的只有
 * view_content_opaque() 那两种确定情形（XRGB/RGBX，或整块 opaque region）—— 它们
 * 保证窗口里每个像素 alpha == 1。客户端只声明了部分 opaque region 的（圆角窗口
 * 四个角、CSD 阴影留白）照旧算模糊：那几处其实也是全透明、模糊照样被剔掉，但不读
 * 像素就没法确认，不值得为它冒险。 */
static bool view_blur_visible(struct vidow_view *view) {
	/* 形状被改过（果冻网格）/ 抓图正压着：看不清内容是不是不透明。 */
	if (view->jelly != NULL || view->mask_opacity) {
		return true;
	}
	/* 自定义 shader（开 / 关 / idle）可能把内容画淡，装饰也归它画。 */
	if (view->fx.shader != NULL) {
		return true;
	}
	/* 动画期间内容是快照 / 被缩放，alpha 也未必是 1。 */
	if (view->anim_open != NULL || view->anim_move != NULL) {
		return true;
	}
	/* alpha 恰好 0 = 一个像素都画不出来：整条抓取 + 链条都是白烧的。
	 * 0 和 0.001 不一样 —— 后者还要留（模糊会从里面透出来一点）。 */
	if (view->anim_alpha <= 0.0) {
		return false;
	}
	if (view->anim_alpha < 1.0) {
		return true;
	}
	struct vidow_look look;
	vidow_view_look(&view->server->config, &view->rule_eff, view->fullscreen,
		&look);
	/* 整个窗口被画淡了（window_opacity）：模糊从里面透出来。 */
	if (look.opacity <= 0.0) {
		return false;
	}
	if (look.opacity < 1.0) {
		return true;
	}
	if (view->scene_surface == NULL || view->scene_surface->buffer == NULL) {
		return true;
	}
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	return !view_content_opaque(view, &geo);
}

/* 这个窗口现在要不要采样模糊底衬。上面那份不看 fx 开没开 —— 全屏直通的判据要在
 * 「fx 已经被收掉」的时候也能问出答案（见 vidow_view_scanout_ok），所以分开。 */
/* 被一个**内容不透明**的窗口整块盖住了吗（z 序在上、把它的框完全盖住）。
 *
 * 完全盖住 = 它的模糊一个像素都透不出来：装饰着色器只在内容 alpha < 1 的地方
 * 采样底衬，而那些地方上面那个不透明的窗口挡着。最有用的现场是「全屏游戏盖住
 * 底下一排浮窗」—— 以前那些窗口的私有底衬每帧照抓、kawase 照跑，重叠的 N 个
 * 窗口各抓一份本来就是 O(n²)，这条把「根本看不见的那些」直接摘掉。
 *
 * 判据宁严：不透明 + 完全覆盖 + z 序在上，三条都成立才算。判错的代价是「本该
 * 模糊的地方不糊」，拿不准一律当没被盖住。 */
static struct vidow_view *view_from_scene_node(struct wlr_scene_node *node) {
	if (node == NULL || node->data == NULL) {
		return NULL;
	}
	struct vidow_view_tag *tag = node->data;
	if (tag->magic != VIDOW_VIEW_TAG_MAGIC) {
		return NULL;
	}
	return tag->view;
}

static bool view_fully_occluded(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	if (!view->mapped || !view->placed || view->popup ||
			view->scene_tree == NULL || view->mini != VIDOW_MINI_NONE) {
		return false;
	}
	if (server->layer_toplevels == NULL) {
		return false;
	}
	struct wlr_box mine = vidow_view_screen_box(view);
	struct wlr_scene_node *n;
	/* wlroots 的 children 是「先画在下、后画在上」（raise_to_top 是 append），
	 * 所以倒着走就是从最上面往下：**碰到自己之前**走过的那些才在自己上层。
	 * 碰到自己就停 —— 后面都是被自己挡住的，盖不盖住自己都看不见。 */
	wl_list_for_each_reverse(n, &server->layer_toplevels->children, link) {
		if (n == &view->scene_tree->node) {
			break;
		}
		if (!n->enabled) {
			continue;
		}
		struct vidow_view *o = view_from_scene_node(n);
		if (o == NULL || !o->mapped || !o->placed ||
				o->mini != VIDOW_MINI_NONE || o->anim_alpha < 1.0 ||
				o->mask_opacity || o->jelly != NULL) {
			continue;
		}
		struct wlr_box b = vidow_view_screen_box(o);
		if (b.x > mine.x || b.y > mine.y ||
				b.x + b.width < mine.x + mine.width ||
				b.y + b.height < mine.y + mine.height) {
			continue;
		}
		struct vidow_fx_geo geo = vidow_view_fx_geo(o);
		if (view_content_opaque(o, &geo)) {
			return true;
		}
	}
	return false;
}

/* 这个窗口现在要不要采样模糊底衬。上面那份不看 fx 开没开 —— 全屏直通的判据要在
 * 「fx 已经被收掉」的时候也能问出答案（见 vidow_view_scanout_ok），所以分开。 */
static bool view_wants_blur(struct vidow_view *view) {
	return view->fx.active && view_wants_blur_ever(view) &&
		view_blur_visible(view) && !view_fully_occluded(view);
}

/* 直通诊断：环境变量 VIDOW_SCANOUT_TRACE=1，或者 /tmp/vidow-scanout-trace 这个
 * 文件出现（文件方式不用重启会话，见 xtrace.c 那一套）。开着的时候每秒给每个
 * 窗口一行：判据卡在哪一项。游戏到底有没有直通扫屏，看这个就知道 —— 没直通
 * 就是每帧一次整屏合成 + 一次装饰离屏渲染，那才是游戏里真正的浪费。 */
static bool scanout_trace_on(void) {
	static int64_t last_ns = 0;
	static bool on = false;
	int64_t now = vidow_now_ns();
	if (last_ns == 0 || now - last_ns > 1000 * 1000000LL) {
		last_ns = now;
		on = getenv("VIDOW_SCANOUT_TRACE") != NULL ||
			access("/tmp/vidow-scanout-trace", F_OK) == 0;
	}
	return on;
}

/* VIDOW_NO_SCANOUT=1：一律走合成那条路（探针，见 vidow_view_scanout_ok）。 */
bool scanout_off(void) {
	static int off = -1;
	if (off < 0) {
		off = getenv("VIDOW_NO_SCANOUT") != NULL ? 1 : 0;
	}
	return off == 1;
}

/* 这个窗口这一帧能不能「直通」：屏幕上该显示的像素就是客户端缓冲自己 —— 全屏、
 * 装饰参数全零、内容整块不透明、没有动画 / 果冻 / 遮罩、没有裁剪。满足的话 view.c 就
 * 把装饰节点收起来（不烘那一趟离屏渲染），场景里只剩客户端表面这一个节点，
 * wlroots 的 scene 就有机会把它的缓冲直接扫上屏（见 wlr_scene.c 的
 * scene_entry_try_direct_scanout：要求这一帧的 render list 正好一项、且它是
 * 盖住整块输出的 buffer）。
 *
 * **判据宁严不宽**：漏判只是没省下那两笔 GPU（每帧一次整屏离屏渲染 + 一次输出
 * 合成），误判会让画面变样 —— 装饰、模糊、CSD 留白、半透明、动画里任何一项在
 * 直通下都会露出来（合成那条路把它们画进了离屏缓冲，直通扫的是原始缓冲）。
 *
 * 切换本身是无感的：判据成立时「合成出来的像素」和「客户端缓冲本身」一样，
 * 所以进出直通两个方向都看不出变化（细节见 view.c 的 view_scanout_switch）。 */
bool vidow_view_scanout_ok(struct vidow_view *view) {
	if (view == NULL || !view->mapped || view->popup) {
		return false;
	}
	/* 关掉直通的开关（和 VIDOW_RENDER_STATS 一样的探针）：怀疑「全屏画面和
	 * 以前不一样」时先拿它排除直通这条路 —— 两个方向应该一个像素都不差。 */
	if (scanout_off()) {
		return false;
	}
	/* 小窗（mini window，S3）：一律走合成。直通的定义是「屏幕上该显示的像素就是
	 * 缓冲自己」，而小窗要的正是「把原生 buffer 缩进一个小盒子」—— 两者互斥。
	 *
	 * **必须放在 fullscreen 判断之前**：小窗态刻意保留 fullscreen 标志（不重配
	 * 客户端、不发 set_fullscreen(false)，见 float.c 的 on_commit），全屏游戏进
	 * 小窗之后这条判据照样成立 —— 不挡住的话场景会把 2560x1440 的原生 buffer
	 * 整块扫上屏，小窗化完全失效。 */
	if (view->mini != VIDOW_MINI_NONE) {
		return false;
	}
	/* 只做全屏：只有它铺满整块输出（wlroots 要求节点的框正好等于输出框）。 */
	if (!view->fullscreen || view->scene_parked) {
		return false;
	}
	/* 全景里窗口是被缩小的（ov 那个系数写在 view_anim_place 的缩放里，而那条路
	 * 挂在 fx.active 上）—— 直通里没人给它缩放，全屏窗口会以原尺寸糊在全景上。 */
	if (vidow_overview_active(view->server)) {
		return false;
	}
	if (view->scene_tree == NULL || view->scene_surface == NULL) {
		return false;
	}
	struct wlr_scene_buffer *src = view->scene_surface->buffer;
	if (src == NULL || src->buffer == NULL) {
		return false;
	}
	/* 动画 / 果冻 / 遮罩期间内容不是「缓冲自己」，得走合成那条路。 */
	if (view->anim_open != NULL || view->anim_move != NULL ||
			view->jelly != NULL || view->mask_opacity) {
		return false;
	}
	if (view->anim_alpha < 1.0 || view->anim_scale != 1.0) {
		return false;
	}
	if (view->fx.shader != NULL) {
		return false;
	}
	/* 裁过（跨屏那一半 / overview 里缩放）：这条**不能删**，但也不能像
	 * 以前那样一律退回合成管线 —— 无装饰的窗口（Steam 主窗口就是）退回
	 * 去以后 fx 那条路拿不到 decorated 图，整块画不出来，屏幕上就是黑的。
	 *
	 * 实测：Steam 首开 clip_on=0 走直通、画面正常；关掉再开 clip_on=1
	 * 被踢到合成管线，纹理有 3808x2050 内容（x11-scene 里 tex 非空、
	 * dst 也对），屏幕上却什么都没有 —— 而且 XWayland 只交 1 帧就停，
	 * 因为没人给它 frame_done。
	 *
	 * 直通其实能裁：把裁剪框写到场景节点的裁剪属性上就行，不用绕离屏
	 * 缓冲。下面 crop 那段就是干这个的。 */
	/* 只在**真的要裁掉一块**时才拒绝直通（窗口整块在屏内时裁不裁无所谓，
	 * 硬退回合成管线纯属自找黑屏）。 */
	if (view->fx.clip_on && view->fx.clip.width > 0 &&
			view->fx.clip.height > 0) {
		struct wlr_box vis = {0};
		struct wlr_box node_box = {
			.x = view->geom.x, .y = view->geom.y,
			.width = view->geom.width,
			.height = view->geom.height,
		};
		if (wlr_box_intersection(&vis, &node_box, &view->fx.clip) &&
				(vis.width < node_box.width ||
					vis.height < node_box.height)) {
			if (vidow_xtrace_on()) {
				wlr_log(WLR_INFO, "scanout: %s 拒绝直通 —— 真被切掉一块 "
					"vis=%d,%d %dx%d node=%d,%d %dx%d clip=%d,%d %dx%d",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					vis.x, vis.y, vis.width, vis.height,
					node_box.x, node_box.y,
					node_box.width, node_box.height,
					view->fx.clip.x, view->fx.clip.y,
					view->fx.clip.width, view->fx.clip.height);
			}
			return false; /* 真的被切掉一块：直通画不了 */
		}
	}
	struct vidow_look look;
	vidow_view_look(&view->server->config, &view->rule_eff, view->fullscreen,
		&look);
	if (look.corner_radius > 0.0 || look.border_width > 0.0 ||
			look.shadow_radius > 0.0 || look.noise > 0.0) {
		return false;
	}
	if (look.opacity < 1.0) {
		return false;
	}
	/* 客户端自己画装饰（GTK 那套 CSD 阴影）时表面比窗口大一圈：合成那条路按
	 * 窗口几何裁掉留白，直通扫的是整块缓冲 —— 那圈留白会露出来。 */
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	if (geo.off_x != 0 || geo.off_y != 0) {
		return false;
	}
	if (geo.w != geo.src_w || geo.h != geo.src_h) {
		return false;
	}
	/* 缓冲的设备像素得和这块输出对上（1:1 上屏）。客户端没吃下输出缩放（1x 的
	 * 客户端放在 2x 屏上）时，合成那条路是**着色器**把 1x 贴图放大到 2x、直通
	 * 那条路是**场景**自己放大 —— 两条路的采样未必一模一样，这一步不省。 */
	struct wlr_output *out = view->output;
	if (out == NULL) {
		return false;
	}
	int ow = out->width, oh = out->height;
	if (out->transform % 2 == 1) {
		int t = ow;   /* 90 / 270：长宽对调 */
		ow = oh;
		oh = t;
	}
	if (src->buffer->width != ow || src->buffer->height != oh) {
		return false;
	}
	/* 内容得**整块不透明**：全屏窗口盖住整块屏，只有内容真的不透明，底衬和模糊
	 * 才一个像素都透不出来 —— 合成那条路也只是拿 content 盖住它（装饰着色器里
	 * content.a == 1 时 blurred 那一项被乘成 0），所以直通少了模糊那一趟也看不
	 * 出差别。默认配置 blur_all 开着（平铺窗口也糊壁纸），全屏窗口照样被算成
	 * 「要模糊」，可那笔模糊一个像素都透不出来，为它留着合成不值。
	 *
	 * 两种「不透明」：格式里就没 alpha（XRGB / RGBX，播放器和游戏的常规选择），
	 * 或者客户端自己声明了盖住整块表面的 opaque region（GTK 那套：格式带 alpha，
	 * 但声明整块不透明）。后者 wlroots 判「这个节点盖住了下面的东西」用的也是它
	 * （见 wlr_scene.c 的 scene_node_opaque_region）—— 它不认，直通也轮不到。 */
	bool ok = view_content_opaque(view, &geo);
	if (scanout_trace_on()) {
		static int64_t last_ns = 0;
		int64_t now = vidow_now_ns();
		if (now - last_ns > 1000 * 1000000LL) {
			last_ns = now;
			wlr_log(WLR_INFO, "直通: %s -> %s | 全屏=%d mini=%d 停放=%d "
				"overview=%d 动画=%d 果冻=%d 遮罩=%d shader=%d 裁剪=%d "
				"look(r=%.1f bw=%.1f sr=%.1f noise=%.1f op=%.2f) "
				"留白=%d,%d 几何=%dx%d 缓冲=%dx%d 输出=%s %dx%d 不透明=%d",
				vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
				ok ? "直通" : "合成",
				view->fullscreen, (int)view->mini, view->scene_parked,
				vidow_overview_active(view->server),
				view->anim_open != NULL || view->anim_move != NULL,
				view->jelly != NULL, view->mask_opacity,
				view->fx.shader != NULL, view->fx.clip_on,
				look.corner_radius, look.border_width, look.shadow_radius,
				look.noise, look.opacity, geo.off_x, geo.off_y,
				geo.w, geo.h, src->buffer->width, src->buffer->height,
				out->name != NULL ? out->name : "?",
				out->width, out->height, ok);
		}
	}
	if (vidow_xtrace_on()) {
		wlr_log(WLR_INFO, "scanout: %s 结果=%d（走%s）clip_on=%d "
			"look: r=%.1f bw=%.1f sr=%.1f noise=%.1f",
			vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "(无题)", ok,
			ok ? "直通" : "合成管线", view->fx.clip_on,
			look.corner_radius, look.border_width,
			look.shadow_radius, look.noise);
	}
	return ok;
}

/* Hide/show a toplevel for a backdrop capture. A decorated window is drawn
 * from its fx node and its real surface already sits at zero opacity, so
 * hiding the fx node is enough; an undecorated one draws from the surface. */
static void view_set_capture_visible(struct vidow_view *view, bool on) {
	if (view->fx.active && view->fx.node != NULL) {
		wlr_scene_node_set_enabled(&view->fx.node->node,
			on && view->fx.blur_slot == NULL);
		for (struct vidow_blur_slot *sl = view->fx.blur_slot; sl; sl = sl->next) {
			if (sl->node == NULL) continue;
			if (!on) sl->capture_enabled = sl->node->node.enabled;
			wlr_scene_node_set_enabled(&sl->node->node,
				on && sl->capture_enabled);
		}
	} else if (view->scene_tree != NULL) {
		/* 恢复时要尊重停泊状态：停着的窗口子树本来就该是关的（见
		 * vidow_view_set_parked），抓帧这一趟临时点亮，抓完得关回去 ——
		 * 以前无条件 on && mapped 会把停泊窗口的树永久点亮。 */
		wlr_scene_node_set_enabled(&view->scene_tree->node,
			on && view->mapped &&
			(!view->scene_parked || vidow_view_capture_held(view)));
	}
	/* 拖着果冻的时候，窗口的内容由果冻节点画 —— 它也得跟着藏，否则抓出来的
	 * 底衬里带着这个窗口自己上一帧的形变图（透明窗口一拖：背景里错位的竖线、
	 * 重影，模糊看着像没了）。 */
	vidow_jelly_capture_visible(view, on);
}

/* A window whose box intersects another window cannot use the shared
 * wallpaper-only backdrop: it would lose the window underneath it. Those get
 * a private capture instead. Tiled windows never overlap, so the shared one
 * covers the whole scrolling layout. */
/* 重叠判据的实测成本（VIDOW_RENDER_STATS 里报）。
 *
 * 只统计 + 只在读的时候清零，不改变任何行为。为什么要有它：这个判据每帧
 * 每个走模糊的窗口各一次，历史上被当成「可能很贵」反复讨论，但一直没有
 * 数字。有了 ns/次 和 次/秒，谈优化才有依据。 */
uint64_t ov_ns;
uint64_t ov_n;

/* 只统计开着统计时才量：每次 clock_gettime 两回，量级和判据本身同阶（实测
 * 判据 ~20-40ns，两次 clock_gettime 再加 ~40ns），常态挂着就是白付一倍。 */
bool ov_stats_on(void) {
	static int on = -1;
	if (on < 0) {
		on = getenv("VIDOW_RENDER_STATS") != NULL ? 1 : 0;
	}
	return on != 0;
}

void vidow_fx_take_overlap_cost(uint64_t *ns, uint64_t *n) {
	*ns = ov_ns;
	*n = ov_n;
	ov_ns = 0;
	ov_n = 0;
}

static bool view_overlaps_other(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	/* **判据走 place**：只有「在屏上」的窗口才需要私有底衬（I1：不在屏上的不
	 * 参与任何渲染流程）。以前这里自己查 mapped + placed，和 should_park 是两
	 * 份判断 —— 停泊掉的窗口几何框还在场景坐标系里，拿它判重叠，正在屏上的
	 * 那个窗口就凭空多出一份私有底衬，糊的是停泊窗口的旧内容。 */
	struct vidow_place mine;
	vidow_place_of_view(server, view, &mine);
	if (!mine.resident) {
		return false;
	}
	struct wlr_box a = mine.world;
	/* **重叠判「看得见的部分」，不判几何框**（2026-11-01）：
	 *
	 * 平铺窗口裁到自己那块屏（view_clip_box），滚出视口的窗口 world 框可以
	 * 横到隔壁屏的布局范围里 —— 但越界的部分一个像素都没画。拿 world 判的
	 * 后果：贴着 DP-3 右缘只剩 3px 的窗口，world 框和 DP-2 的全屏游戏相交，
	 * 被判「压着别人」→ 走私有底衬 → 邻居每交一帧它就重抓+重烘+推
	 * blur_ver（实测 QQ 一小时里带崩一套格子链：每秒上百次重烘）。它 3px
	 * 的可见条和游戏的可见区域根本不相交，私有底衬属于白养。
	 *
	 * 浮动 / 拖拽中的窗口 clip_on = false（浮窗是屏级之上的一层，允许骑在
	 * 两块屏上），这里拿到的还是完整 world 框，语义不变。 */
	if (view->fx.clip_on) {
		struct wlr_box va;
		if (!wlr_box_intersection(&va, &a, &view->fx.clip)) {
			return false; /* 整个可见部分都被裁光：不压任何东西 */
		}
		a = va;
	}
	struct vidow_view *other;
	wl_list_for_each(other, &server->views, link) {
		if (other == view) {
			continue;
		}
		struct vidow_place theirs;
		vidow_place_of_view(server, other, &theirs);
		if (!theirs.resident) {
			continue;
		}
		/* **不能按「归属哪块屏」筛**：跨屏拖动时窗口已经画在对面那块屏上了，
		 * 而 view->output 还是原来那块（布局归属要等松手才换）。按归属筛的话，
		 * 它压在对面那块屏的窗口上时，对面这些窗口全被跳过，判定成「不重叠」，
		 * 于是切去共享的整屏壁纸底衬 —— 模糊采不到对方的内容，只剩壁纸
		 * （用户报的「拖拽背景模糊跨显示器没有采样另一个显示器的内容」）。
		 *
		 * vidow_view_screen_box() 给的本来就是**场景全局坐标**，跨屏天然可比，
		 * 直接判相交就够了。 */
		struct wlr_box b = theirs.world;
		if (other->fx.clip_on) {
			struct wlr_box vb;
			if (!wlr_box_intersection(&vb, &b, &other->fx.clip)) {
				continue; /* 对面整个都被裁光了：看不见就不用算 */
			}
			b = vb;
		}
		if (a.x < b.x + b.width && b.x < a.x + a.width &&
				a.y < b.y + b.height && b.y < a.y + a.height) {
			return true;
		}
	}
	return false;
}

/* Point a window at a blurred backdrop. Nothing is redrawn while the texture
 * identity stays the same, which is what keeps a static screen free.
 *
 * region 是这张贴图盖住的那一块（设备像素，相对输出原点），NULL = 整屏那张
 * （fx->blur_tex_w == 0 就是「整屏」的记号，装饰着色器按它算采样 UV，见
 * decor_draw 里 u_origin / u_output_size 那两行）。
 *
 * 区域必须**跟着贴图一起换**：窗口从「和别的窗口重叠」（贴图是它自己那块
 * 768x768 的私有区域）变成「不重叠」（贴图换成共享的整屏那张）时只换贴图不换
 * 区域，着色器就会拿整屏那张贴图按 768x768 的区域去采样 —— 屏幕上是一块被
 * 缩放 + 错位的壁纸模糊。用户报的「窗口不重叠时模糊背景是错的」就是这个：
 * 拖动 / 滚动 / 开窗动画期间重叠过一下，之后就一直是错的，直到它再次重叠。
 * （layer surface 那条路本来就在切回共享贴图时把 blur_tex_w/h 清 0，窗口这条路
 * 漏了 —— 现在两边都从这一个入口走。） */
/* 找这块屏对应的那一格；need=true 时找不到就占一格空的。
 *
 * 两格里按「哪块屏」认：跨屏期间同一块屏每一帧来的都是同一个 struct
 * wlr_output *，占住就不会被另一块屏挤掉。两格都占满又来了第三块屏（三屏拼
 * 角那种极端布局）就退回第一格 —— 那时宁可让它多糊一下，也不能让它去改别人
 * 正在用的那一格。 */
static void instance_node_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct vidow_blur_slot *sl = wl_container_of(listener, sl, node_destroy);
	wl_list_remove(&sl->node_destroy.link);
	sl->node = NULL;
}

void instance_drop(struct vidow_blur_slot *sl) {
	if (sl->node != NULL) {
		/* 先摘缓冲再销毁节点：节点销毁本身会放掉它挂着的 buffer，但显式
		 * 先 set NULL 让那把锁在最前面就还掉 —— dec/anim 里任何一块还挂着
		 * 的目标都能立刻归还（n_locks 归零），不等节点析构的顺序。 */
		wlr_scene_buffer_set_buffer(sl->node, NULL);
		wlr_scene_node_destroy(&sl->node->node);
	}
	for (int i = 0; i < 2; i++) {
		if (sl->dec[i]) wlr_buffer_drop(sl->dec[i]);
		if (sl->anim[i]) wlr_buffer_drop(sl->anim[i]);
	}
	if (sl->owned && sl->tex) wlr_texture_destroy(sl->tex);
	blur_set_drop(&sl->blur_set);
	if (sl->own_bd_tex) stat_tex_del(sl->own_bd_tex);
	if (sl->own_bd) wlr_buffer_drop(sl->own_bd);
	free(sl);
}

struct vidow_blur_slot *blur_slot_for(struct vidow_fx_target *fx,
		struct wlr_output *out, bool need) {
	if (out == NULL) {
		return NULL;
	}
	for (struct vidow_blur_slot *sl = fx->blur_slot; sl; sl = sl->next) {
		if (sl->out == out) {
			return sl;
		}
	}
	if (!need) {
		return NULL;
	}
	struct vidow_blur_slot *sl = calloc(1, sizeof(*sl));
	if (sl == NULL) {
		return NULL;
	}
	sl->out = out;
	sl->next = fx->blur_slot;
	fx->blur_slot = sl;
	return sl;
}

/* 这一次烘该用哪张底衬贴图：优先取**这块屏那一格**（跨屏时两块屏各一格），
 * 没占格子再退回单份的老字段（layer / 预览 / 关闭快照那些路没有输出上下文）。 */
static struct wlr_texture *fx_blur_tex_now(struct vidow_fx_target *fx) {
	struct vidow_blur_slot *sl = blur_slot_for(fx, fx->bake_output, false);
	if (sl != NULL && sl->tex != NULL) {
		return sl->tex;
	}
	return fx->blur_tex;
}

void blur_assign(struct vidow_fx_target *fx, struct wlr_output *out,
		struct wlr_texture *tex, bool owned, uint64_t id,
		const struct wlr_box *region) {
	int rx = region != NULL ? region->x : 0;
	int ry = region != NULL ? region->y : 0;
	int rw = region != NULL ? region->width : 0;
	int rh = region != NULL ? region->height : 0;
	/* 写进**这块屏自己的那一格**（见 server.h 的 struct vidow_blur_slot）：
	 * 窗口骑在两块屏中间时，两块屏的采样原点和区域都不一样，共用一份字段就是
	 * 互相覆盖 —— 先渲染的那块屏拿到后渲染那块屏的坐标，屏幕上一半正常、一半
	 * 被拉成横条。 */
	/* **只有真有「这次烘给哪块屏」上下文的目标才分格。**
	 *
	 * 格子是给窗口装饰用的：ensure_targets 按 bake_output 选缓冲，fx_blur_tex_now
	 * 也按 bake_output 取贴图 —— 两边必须是同一个判据。layer 的底衬、overview
	 * 那张糊壁纸、关闭快照这些路没有 bake_output（它们一次只用一份贴图），
	 * 写成格子的话 fx_blur_tex_now 永远找不到它（查的是 bake_output 那一格），
	 * 着色器拿不到贴图 → u_blur_strength 归零 → 整块画成全透明
	 * （实测 overview_blur 开了和没开一个像素都不差）。
	 *
	 * **out 参数优先于 bake_output**（第 6 步）：拖拽中 bake_output 不再被
	 * vidow_blur_update 钉死（见那里），而调用方 vidow_blur_update 明确知道
	 * 「这一趟是给哪块屏更新的」—— 就按 out 写格子。否则拖一半时两块屏的
	 * 更新都写进 bake_output 那一格，另一格永远停在旧底衬（用户报的
	 * 「拖一半 DP-3 的背景不画、还从 DP-2 错位」）。layer / 预览那些没有
	 * bake_output 的路两者都是 NULL，行为不变。 */
	struct vidow_blur_slot *slot = (fx->bake_output != NULL || out != NULL) ?
		blur_slot_for(fx, out != NULL ? out : fx->bake_output, true) : NULL;
	if (slot != NULL) {
		if (slot->id == id && slot->rx == rx && slot->ry == ry &&
				slot->rw == rw && slot->rh == rh &&
				slot->tex == tex) {
			return; /* 这一格已经是这一版了 */
		}
		if (slot->owned && slot->tex != NULL && slot->tex != tex) {
			wlr_texture_destroy(slot->tex);
		}
		slot->tex = tex;
		slot->owned = owned;
		slot->id = id;
		slot->rx = rx;
		slot->ry = ry;
		slot->rw = rw;
		slot->rh = rh;
		fx->blur_ver++;
		blur_ver_hits[2665]++;
		return;
	}
	/* 兜底（没有输出上下文的路：layer、快照、预览）：走老的单份字段。 */
	/* 区域也算进身份：同一张贴图换了区域，装饰里烘的还是上一版的 UV，
	 * 得让它重烘（blur_ver++）。 */
	if (fx->blur_tex_id == id && fx->blur_tex_x == rx &&
			fx->blur_tex_y == ry && fx->blur_tex_w == rw &&
			fx->blur_tex_h == rh) {
		return;
	}
	if (fx->blur_owned && fx->blur_tex != NULL) {
		wlr_texture_destroy(fx->blur_tex);
	}
	fx->blur_tex = tex;
	fx->blur_owned = owned;
	fx->blur_tex_id = id;
	fx->blur_tex_x = rx;
	fx->blur_tex_y = ry;
	fx->blur_tex_w = rw;
	fx->blur_tex_h = rh;
	fx->blur_ver++;
	blur_ver_hits[2686]++;
	if (getenv("VIDOW_DMG_WHY") != NULL) {
		dmg_why_probe("blur_assign-legacy", region,
			(uintptr_t)__builtin_return_address(0));
	}
}

/* 剔除阈值（blur_alpha_min）只进装饰着色器，不进模糊金字塔：它变了得让帧循环知道
 * 「已经烘进那张缓冲里的那一版」过期了，否则运行时改配置（重载 / set-config）看着
 * 就是「没反应」（这条坑的底衬版本在 README 的 ⓪ 里）。 */
void blur_mask_min_changed(struct vidow_fx_target *fx, double alpha_min) {
	if (fabs((double)fx->mask_min_used - alpha_min) > 1e-6) {
		fx->mask_min_used = (float)alpha_min;
		fx->blur_ver++;
		blur_ver_hits[2695]++;
	}
}

/* 私有底衬那块区域缓冲 + 包着它的贴图一起收（跟 layer 的 layer_bd_drop 一个
 * 套路）。金字塔那几块缓冲归 blur_set，这里只放区域底衬。 */
void own_bd_drop(struct vidow_fx_target *fx) {
	if (fx->own_bd_tex != NULL) {
		stat_tex_del(fx->own_bd_tex);
		fx->own_bd_tex = NULL;
	}
	if (fx->own_bd != NULL) {
		wlr_buffer_drop(fx->own_bd);
		fx->own_bd = NULL;
	}
	fx->own_bd_w = 0;
	fx->own_bd_h = 0;
}

/* Release an owned texture before its pyramid buffer is reused. */
void blur_own_drop(struct vidow_fx_target *fx) {
	blur_assign(fx, NULL, NULL, false, 0, NULL);
	fx->blur_own_tex = NULL; /* 贴图归 blur_set，上面那句只放指针 */
	fx->blur_own_id = 0;
}

/* Drop every blur resource of a target. blur_assign() only frees what the
 * target owns, so the server's shared wallpaper texture survives. */
void blur_target_release(struct vidow_fx_target *fx) {
	while (fx->blur_slot != NULL) {
		struct vidow_blur_slot *sl = fx->blur_slot;
		fx->blur_slot = sl->next;
		instance_drop(sl);
	}
	fx->cur_slot = NULL;
	fx->bake_output = NULL;
	blur_own_drop(fx);
	blur_set_drop(&fx->blur_set);
	own_bd_drop(fx);
}

/* layer surface 背后的模糊（实现在下面「draw」之后）：先声明，抓底衬的时候
 * 要用它把节点藏起来。 */
/* layer 那块区域底衬（缓冲 + 包着它的贴图）一起收。贴图是包的缓冲、不是包的像素，
 * 复用缓冲的时候贴图也跟着复用，只有尺寸变了才重建 —— 链条要拿它当第一趟的源，
 * 以前每趟导入一次、用完销毁，真后端上每帧就是十几次 EGLImage + BO 映射。 */
void layer_bd_drop(struct vidow_layer_surface *ls) {
	if (ls->bd_tex != NULL) {
		stat_tex_del(ls->bd_tex);
		ls->bd_tex = NULL;
	}
	if (ls->bd != NULL) {
		wlr_buffer_drop(ls->bd);
		ls->bd = NULL;
	}
	ls->bd_w = 0;
	ls->bd_h = 0;
	ls->bd_gen_scene = 0;
	ls->bd_gen_layer = 0;
}



bool decor_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, struct wlr_texture *mask,
		const float border[4], int ox, int oy,
		double alpha, double scale, const struct vidow_look *look);
static bool anim_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, int ox, int oy, double alpha, double scale);

/* 没画成的原因出口（定义在下面，layer_blur_wanted 也要用）。 */
/* 抓底衬时要不要把这个 layer surface 自己藏起来（见下面的定义）。 */
bool vidow_fx_frame(struct vidow_server *server, struct vidow_fx_target *fx,
		struct wlr_scene_buffer *src, const struct vidow_fx_geo *geo,
		int x, int y, int ox, int oy,
		double alpha, bool focused, bool fullscreen, double scale, double ov,
		const struct vidow_rule_eff *eff) {
	if (!fx->active || fx->node == NULL || src == NULL || src->buffer == NULL) {
		return false;
	}
	/* 内容贴图这次是按什么编码采的：源 scene buffer 的声明（wlroots 在
	 * surface commit 时已经写在它上面）。0 = 没标 = 默认 gamma22 + srgb。
	 * color.linear_blend 打开时装饰着色器按这个解码、做色域转换。 */
	fx->src_tf = src->transfer_function != 0 ? src->transfer_function :
		WLR_COLOR_TRANSFER_FUNCTION_GAMMA22;
	fx->src_primaries = src->primaries != 0 ? src->primaries :
		WLR_COLOR_NAMED_PRIMARIES_SRGB;
	/* 这个窗口 / layer 现在该长什么样：全局配置压上窗口规则（见 rule.c 的
	 * vidow_view_look / vidow_rule_look）。eff = NULL 就是纯全局。 */
	struct vidow_blur_slot *instance = NULL;
	struct wlr_scene_buffer *node = fx->node;
	if (fx->view != NULL && fx->bake_output != NULL) {
		instance = blur_slot_for(fx, fx->bake_output, true);
		if (instance == NULL) {
			return false;
		}
		if (instance->node == NULL) {
			instance->node = wlr_scene_buffer_create(fx->node->node.parent, NULL);
			if (instance->node == NULL) {
				return false;
			}
			instance->node->point_accepts_input = fx_node_rejects_input;
			instance->node_destroy.notify = instance_node_destroy;
			wl_signal_add(&instance->node->node.events.destroy,
				&instance->node_destroy);
		}
		node = instance->node;
		wlr_scene_node_set_enabled(&fx->node->node, false);
		wlr_scene_node_set_enabled(&node->node, true);
		double output_scale = fx->bake_output->scale;
		if (output_scale > 0.0) {
			scale = output_scale * ov;
		}
	}
	struct vidow_look look;
	vidow_view_look(&server->config, eff, fullscreen, &look);
	/* 预览（缩略图：overview 的格子 / MRU 卡片）里**不叠那层模糊**。
	 *
	 * ov 就是「这块缩略图相对原尺寸的缩放」：overview 那一趟传的是格子缩放
	 * （1 → 0.46 的动画值，见 view.c 的 vidow_view_anim_bake），MRU 卡片那一趟
	 * 传的是卡片的 zoom（见 mru.c 的 mru_card_thumb），桌面那条路恒等于 1。
	 * 缩略图里画的是窗口的**真实缓冲**（客户端自己那份，原样采过来），内容已经
	 * 按 0.4x 缩过一道；再在它底下垫一层按**屏幕**坐标抓的模糊底衬，看着就是
	 * 「预览里的窗口内容是糊的」（用户报的「overview 和 mru 的预览不应该导致
	 * 窗口内容变模糊」）。模糊是给屏幕上的窗口用的，缩略图不该有。
	 *
	 * 只关**这一次渲染**的 look.blur（decor_draw 按它决定 u_blur_strength），
	 * **别动 fx->blur** —— 那个标志是 vidow_fx_begin() 按配置设的、
	 * vidow_blur_update() 也要读它，改了退出预览就再也恢复不了（README 1723）。
	 *
	 * 阈值取 0.98 而不是「< 1」：overview 进出场动画的头尾几帧 ov 是 0.999 /
	 * 0.998 这种值，那几帧窗口还是原尺寸、模糊是对的，不该跳一下。 */
	/* **但 overview 里的窗口要保留模糊。**
	 *
	 * 上面那条阈值原本一刀切掉所有 ov < 0.98 的渲染，overview 的窗口正好
	 * 落在这个范围（ov = 0.46）—— 于是 overview 里窗口背后那圈模糊直接没有
	 * 了。用户 2026-10-26：「窗口模糊效果没关掉 但对不上了 几乎消失」。
	 *
	 * 两种情况得分开：
	 *   - MRU / overview 的**缩略图**（mru.c 的 mru_card_thumb）：画的是窗口
	 *     缓冲缩过一道的副本，垫一层按屏幕坐标抓的模糊就是「预览内容发糊」，
	 *     该关；
	 *   - overview 里**窗口自己**：它就是屏幕上那扇窗口的缩小版，背后本来就
	 *     该糊。采样坐标已经按 ov 修好了（见下面 blur_k 那段），所以能给。
	 *
	 * 判据用 fx->view：缩略图那条路的 target 没有挂 view（mru.c 自己建
	 * vidow_fx_target，只设 anim_scale / content_box），窗口那条路一定有。 */
	bool is_thumb = fx->view == NULL;
	if (ov < 0.98 && (is_thumb || !vidow_overview_active(server))) {
		look.blur = false;
	}
	/* 果冻期间内容图里**不烘模糊**：模糊由 jelly.c 的网格单独一层按屏幕坐标
	 * 采样，跟着形状弯但不被拉长。贴图还没建好时保留烘进去的那一版，别让窗口
	 * 在起手第一帧突然没模糊。 */
	if (fx->jelly && fx->blur && fx->blur_tex != NULL &&
			!fx->blur_region_on && look.blur) {
		look.blur = false;
	}
	/* 果冻期间投影单独画（jelly.c 的 shadow 节点），内容图里不烘投影：
	 * 投影跟着网格拉伸就会变成「描边外面一圈模糊」。 */
	/* 果冻期间投影仍然烘在内容图里：它和窗口共用同一张网格，形状完全
	 * 跟着窗口弯；模糊由 jelly 网格单独一层按窗口区域裁，不会把投影圈进去。 */
	if (geo == NULL || geo->w <= 0 || geo->h <= 0) {
		wlr_log(WLR_DEBUG, "fx-frame ret: geo0");
		return false;
	}

	/* 内容尺寸 = **可见窗口**，不是整块表面：客户端自己画装饰时（GTK 那套 CSD
	 * 阴影）缓冲比窗口大一圈，边框/圆角/阴影围着缓冲画就离窗口差出那圈留白 ——
	 * 屏幕上正是「边框 / 一圈背景 / 窗口」。niri 的 LayoutElement::size() 用的
	 * 也是窗口几何，原因一样（见 struct vidow_fx_geo）。 */
	int cw = geo->w, ch = geo->h;
	/* 源缓冲里「可见窗口」在哪（归一化）。窗口几何 == 表面时是 0 / 1，采出来的
	 * 内容和以前逐像素一样。
	 *
	 * 优先用 geo->tex_*（vidow_view_fx_geo 按「客户端缓冲像素 / wp_viewport 的
	 * 有效区域」算好的）：逻辑尺寸和缓冲像素在分数缩放 + viewporter 下不是一回事，
	 * 按逻辑尺寸算出来的是整块缓冲 —— 客户端没画过的那圈会一起铺进窗口（见
	 * struct vidow_fx_geo 的说明）。全 0 的时候（层表面、关闭快照这些自己拼几何的
	 * 地方）退回老公式。 */
	if (geo->tex_du > 0.0f && geo->tex_dv > 0.0f) {
		fx->tex_off_x = geo->tex_u0;
		fx->tex_off_y = geo->tex_v0;
		fx->tex_scale_x = geo->tex_du;
		fx->tex_scale_y = geo->tex_dv;
	} else {
		fx->tex_off_x = geo->src_w > 0 ? (float)geo->off_x / (float)geo->src_w : 0.0f;
		fx->tex_off_y = geo->src_h > 0 ? (float)geo->off_y / (float)geo->src_h : 0.0f;
		fx->tex_scale_x = geo->src_w > 0 ? (float)geo->w / (float)geo->src_w : 1.0f;
		fx->tex_scale_y = geo->src_h > 0 ? (float)geo->h / (float)geo->src_h : 1.0f;
	}
	int pad = vidow_fx_pad(server);
	double px_w = ((double)cw + 2.0 * pad) * (scale > 0.0 ? scale : 1.0);
	double px_h = ((double)ch + 2.0 * pad) * (scale > 0.0 ? scale : 1.0);
	if (px_w > MAX_TARGET_PX || px_h > MAX_TARGET_PX) {
		wlr_log(WLR_DEBUG, "fx-frame ret: toobig");
		return false;
	}
	/* 输出缩放 = 设备像素 / 逻辑像素。scale 是烘图用的倍率（= 输出缩放 × ov），
	 * ov <= 0 时按 1 算（调用方在这之前已经把 ov 归一到 > 0，这里只是兜底）。 */
	double out_scale = ov > 0.0 ? scale / ov : scale;
	if (!(out_scale > 0.0)) {
		out_scale = 1.0;
	}
	if (vidow_xtrace_on()) {
		wlr_log(WLR_INFO, "fx-trace: scale=%.2f ov=%.2f out_scale=%.2f "
			"fx->w=%d cw=%d -> dst=%d view=%s", scale, ov, out_scale,
			fx->w, cw, (int)lround((double)fx->w / out_scale),
			fx->view != NULL && vidow_view_title(fx->view) != NULL ?
				vidow_view_title(fx->view) : "(非窗口)");
	}
	if (!ensure_targets(server, fx, cw, ch, scale, out_scale)) {
		wlr_log(WLR_DEBUG, "fx-frame ret: ensure");
		return false;
	}
	/* 开窗动画的缩放只作用在场景节点上（离屏缓冲照旧原尺寸烘）。模糊已经
	 * 烘在缓冲里，节点一缩，背景也跟着缩 —— 看起来是「桌面背景在弹」。
	 * 把采样坐标按同一倍率展开：p 是缓冲像素，节点把缓冲映射到屏幕时乘了
	 * anim_scale，所以模糊 UV 里的 p 也乘 anim_scale，再补上中心缩放的
	 * 平移量。静止时 scale=1 / bias=0，一个像素都不变。 */
	/* **模糊 UV 要跟着「缓冲怎么映射到屏幕」走。**
	 *
	 * 着色器算的是 o = (borg - content_off + p * blur_scale + blur_bias) / bsize
	 * —— p 是**缓冲像素**，borg 是窗口在屏幕上的原点。缓冲里这个像素最终落在
	 * 屏幕的哪个点，取决于节点把它缩放到多大：anim_scale（开合动画）和 ov
	 * （overview / 小窗 / 预览的缩略倍率）都会缩。
	 *
	 * 以前只乘了 anim_scale。于是 overview 里窗口缩到 0.46、位置也换了，采样
	 * 坐标却还按原尺寸算 —— 采到的模糊跟窗口现在盖住的那块屏幕完全不是一回
	 * 事（窗口缩小后大多采到贴图边缘之外，clamp 之后几乎全透明）。用户
	 * 2026-10-26 的原话：「窗口模糊效果没关掉 但对不上了 几乎消失」。
	 *
	 * 把 ov 一起乘进去：节点按 ov 缩小，采样半径就按 1/ov 展开 —— 窗口左边
	 * 那个像素去采屏幕上它现在真正盖住的位置。bias 用同一条式子：中心缩放
	 * 的平移量。
	 *
	 * 注意 ov 是**调用方传进来的那个**（view.c 的 fx_ov = ov * mini_scale），
	 * 和 out_scale 用的 ov 是同一个值 —— out_scale = scale / ov 里两者同乘，
	 * 输出倍率一点没变，所以这里只影响采样坐标，不影响烘图分辨率。 */
	double blur_as = fx->anim_scale > 0.0 ? fx->anim_scale : 1.0;
	double blur_ov = ov > 0.0 ? ov : 1.0;
	double blur_k = blur_as * blur_ov;
	fx->blur_scale_x = (float)blur_k;
	fx->blur_scale_y = (float)blur_k;
	fx->blur_bias_x = 0.0f;
	fx->blur_bias_y = 0.0f;
	if (fabs(blur_k - 1.0) > 1e-4) {
		fx->blur_bias_x = (float)((1.0 - blur_k) *
			((double)fx->px_pad + (double)fx->w * 0.5));
		fx->blur_bias_y = (float)((1.0 - blur_k) *
			((double)fx->px_pad + (double)fx->h * 0.5));
	}

	bool tex_owned = true;
	struct wlr_texture *tex = client_scene_texture(server, src, &tex_owned);
	if (tex == NULL) {
		wlr_log(WLR_DEBUG, "fx-frame ret: texnull");
		return false;
	}

	/* 动画那一趟要的缓冲先占好：它决定了装饰那趟要不要自己乘 alpha（见下）。
	 * 占不到就只画装饰，这一帧照旧自己淡出，不会白闪一下。
	 *
	 * 两块都得要：上屏的那块被场景锁着，只有一块的话动画着色器隔一帧才跑得起来
	 * —— 屏幕上就是「缩放过的窗口」和「没缩放只有淡入的窗口」交替闪，看着正
	 * 好像一块大矩形一闪一闪。 */
	struct wlr_buffer *anim = NULL;
	if (fx->shader != NULL) {
		struct wlr_buffer **abufs = instance != NULL ?
			instance->anim : fx->anim_buf;
		for (int i = 0; i < 2; i++) {
			/* 尺寸跟不上（跨屏换了倍率、dec 那对刚按新尺寸重建）：旧的先放。
			 * 不放的话旧尺寸那块一直攥在手里，新分配的又按 fx->w/h 来 ——
			 * 缓冲尺寸和烘图尺寸对不上，画出来错位，旧的也永远还不掉。 */
			if (abufs[i] != NULL && (abufs[i]->width != fx->w ||
					abufs[i]->height != fx->h)) {
				wlr_buffer_drop(abufs[i]);
				abufs[i] = NULL;
			}
			if (abufs[i] == NULL) {
				abufs[i] = vidow_alloc_target(server, fx->w, fx->h);
			}
		}
		anim = pick_free(abufs);
	}

	vidow_stats.bakes++;
	/* **这块屏自己那一对**装饰目标（见 server.h 的 struct vidow_blur_slot
	 * 的 dec*）：跨屏时两块屏各有一对，各按自己的倍率烘。没有 bake_output
	 * 的路（layer / 预览）用老的那一对。 */
	struct vidow_blur_slot *dsl = fx->cur_slot;
	struct wlr_buffer **dbufs = dsl != NULL ? dsl->dec : fx->buf;
	struct wlr_buffer *displayed = NULL;
	struct wlr_buffer *decorated = NULL;
	const float *border = NULL; /* 重画装饰用（动画那趟失败时） */
	if (look.decor && server->decor.program != 0) {
		struct wlr_buffer *target = pick_free(dbufs);
		if (target != NULL) {
			border = focused ? look.border_color_focus :
				look.border_color;
			/* 有动画那趟时装饰按满 alpha 画：整块的淡入淡出由动画着色器统一乘
			 * 一次，两边各乘一次就成了 alpha²（淡得比设定的快）。 */
			double decor_alpha = anim != NULL ? 1.0 : alpha;
			if (!decor_draw(server, fx, target, tex, NULL, border, ox, oy,
					decor_alpha, scale, &look)) {
				if (tex_owned) {
					wlr_texture_destroy(tex);
				}
				return false;
			}
			displayed = target;
			decorated = target;
		} else {
			/* 两块装饰目标都还在飞（上一两帧还没上完屏）：这一帧干脆不画，
			 * 让上帧的画面多停一帧，别拿没装饰的内容顶上去。 */
			if (tex_owned) {
				wlr_texture_destroy(tex);
			}
			return false;
		}
	}

	/* 动画着色器跑在**装饰结果**上，不是原始内容：渲染目标是「窗口 + 阴影留白」
	 * 那么大一块画布，而内容贴图只占中间那块 —— 直接把它铺满整张画布，放大缩小
	 * 的就是一张被拉伸、没有圆角没有边框没有模糊的硬边大矩形，比窗口大一圈，动
	 * 画一结束装饰才接上，看着就是「先冒出一块大矩形再缩回窗口」。喂装饰结果
	 * 才是 1:1：整块带边框/圆角/模糊的窗口跟着缩放淡入淡出，收尾正好落在窗口
	 * 自己该在的位置上。 */
	if (anim != NULL) {
		struct wlr_texture *atex = NULL;
		if (decorated != NULL) {
			atex = wlr_texture_from_buffer(server->renderer, decorated);
		}
		if (atex == NULL) {
			/* 装饰结果当不成贴图（导入失败）：退回原始内容。装饰那趟已经是按满
			 * alpha 画的，这一帧要是不跑动画，窗口就会「啪」地全亮一下。 */
			atex = tex;
		}
		if (anim_draw(server, fx, anim, atex, ox, oy, alpha, scale)) {
			displayed = anim;
		} else if (decorated != NULL) {
			/* 动画那趟没画成（FBO 拿到就没了）：装饰还在，把它按真正的 alpha
			 * 重画一遍补上淡入淡出，别让窗口整块亮一帧。 */
			if (!decor_draw(server, fx, decorated, tex, NULL, border, ox, oy,
					alpha, scale, &look)) {
				if (tex_owned) {
					wlr_texture_destroy(tex);
				}
				return false;
			}
		}
		if (atex != tex) {
			wlr_texture_destroy(atex);
		}
	}
	if (tex_owned) {
		wlr_texture_destroy(tex);
	}
	tex_owned = false; /* 借来的那张已经还了，后面别再碰 */

	if (displayed == NULL) {
		wlr_log(WLR_DEBUG, "fx-frame ret: displayed");
		return false;
	}

	/* ov < 1 = 在 overview 里：整块装饰（连内边距）跟着缩，画到屏幕上的尺寸
	 * 正好等于渲染它用的设备像素（不糊也不浪费）。 */
	if (!(ov > 0.0)) {
		ov = 1.0;
	}
	/* 动画缩放：整块（连内边距）按 anim_scale 缩，围绕窗口中心 —— 所以位置
	 * 要挪回一半的差值，不然看着是从左上角往右下缩。静止时 anim_scale 恒为
	 * 1，这两项正好是 0，一个像素都不动。 */
	double as = fx->anim_scale > 0.0 ? fx->anim_scale : 1.0;
	/* 节点尺寸 = 烘图那张缓冲的**设备像素数** ÷ 输出缩放，正好 1:1 落到设备
	 * 像素上（ensure_targets 已经把缓冲取成输出缩放的整数倍，所以除得尽）。
	 * 别再写回 lround(fx->dst_w * ov)：那中间隔着两次 lround，会漂一个逻辑
	 * 像素 —— 差这一个像素，wlroots 就得把烘好的图按 1.00x 之外的倍率再双
	 * 线性采一次，预览里那层糊就是这么来的（见 ensure_targets 上面的注释）。 */
	int dst_w0 = (int)lround((double)fx->w / out_scale);
	int dst_h0 = (int)lround((double)fx->h / out_scale);
	int pad_ov = (int)lround((double)pad * ov * as);
	int dst_w = (int)lround((double)dst_w0 * as);
	int dst_h = (int)lround((double)dst_h0 * as);
	if (dst_w < 1) {
		dst_w = 1;
	}
	if (dst_h < 1) {
		dst_h = 1;
	}
	int node_x = x - pad_ov + (dst_w0 - dst_w) / 2;
	int node_y = y - pad_ov + (dst_h0 - dst_h) / 2;
	/* 越界裁剪：这块窗口只有一部分在自己那块屏里（另一半跨过了屏边界，场景图是
	 * 全局坐标，不裁就会被隔壁那块屏画出来）。裁的是**显示**：离屏缓冲照旧按整块
	 * 窗口烘，只是这张图上只采「框里的那一块」、铺到框里的那点地方。 */
	bool cropped = false;
	struct wlr_fbox crop_src = {0};
	/* overview 开着：**在这儿**裁到 overview 那块屏的工作区。
	 *
	 * 为什么不靠 view.c 的 fx->clip_on/clip：实测那条管线在 overview 里根本
	 * 不被调用（日志锚点 `overview clip:` 0 行），所以条带比屏宽的那些列一直
	 * 照画到隔壁屏上（用户：「overview 窗口太多会跨屏」）。裁剪真正生效的地方
	 * 就是下面这段（crop_src / node_x），在这儿算最可靠。
	 *
	 * 只裁画：不碰 wlr_scene_subsurface_tree_set_clip（它连**输入**一起裁，裁了
	 * 预览就命不中 —— test-overview 的「点某个窗口 = 选中它」当场红）。 */
	bool ov_clip = false;
	struct wlr_box ov_clip_box = {0};
	if (fx->view != NULL && vidow_overview_active(fx->view->server) &&
			fx->view->mini == VIDOW_MINI_NONE && fx->view->mapped) {
		/* 裁到**显示器**，不是行矩形。
		 *
		 * 用户要的语义（2026-10-25）：「工作区内窗口可以溢出工作区，但不会
		 * 溢出显示器」。overview 把每块工作区画成一块缩小的屏，行矩形只是
		 * 那块「缩小屏」的边框 —— 拿它当裁剪框等于把窗口裁在工作区里，
		 * 窗口稍微探出自己那一行就整块被切掉，而屏幕上明明还有地方。
		 *
		 * 以前这里裁的是行矩形（更早是整块屏），两种都试过。行矩形那版的
		 * 问题就是上面这条：行与行之间本来有间隙，间隙属于显示器、不属于
		 * 任何工作区，窗口探进间隙应该照常显示（就像正常桌面里窗口探出
		 * 工作区边界一样），不该被切。
		 *
		 * 所以裁剪边界回到显示器：行内随便溢出，出屏才切。隔壁显示器上
		 * 不会画到 —— 场景图是全局坐标的，这块屏的节点裁在自己屏框里，
		 * 就落不到别人那块上。
		 *
		 * 行矩形仍然有用（命中测试、动画目标），只是不再当裁剪框。 */
		/* ⚠️ 用**显示器**的框，不是 ov->out_×。
		 *
		 * ov->out_× 存的是 vidow_work_area()（工作区 = 屏框扣掉 panel /
		 * gap 之后的可用区）。拿它当裁剪框，窗口探进 panel 那一条还是会被
		 * 切 —— 而用户要的是「溢出工作区可以，溢出显示器不行」，panel
		 * 那块属于显示器、不属于工作区，应该照常显示。
		 *
		 * 屏框是布局坐标里的真值，直接问 output_layout。 */
		/* 用**窗口自己所在那块屏**的框（每块屏都进 overview，各自裁自己
		 * 那块屏的范围）。以前固定用 overview.output，别的屏上的窗口被裁到
		 * 主屏矩形里。 */
		/* **不再在这里裁。**
		 *
		 * 这里原本把裁剪框设成「窗口归属那块显示器的布局框」，可 overview
		 * 里节点画的是**行坐标**（每块工作区缩成一条，见 server.h 的
		 * vidow_ov_row：screen = 行的 content_x/y + 局部 * scale），
		 * 跟布局里的显示器框压根不是一个坐标系。两者一相减，窗口只要探出
		 * 显示器一点点，交集就空了 —— 下面那条「整个身子都在框外」的分支
		 * 把节点甩到 -1000000，窗口在 overview 里整个消失（用户 2026-10-27
		 * 报的「overview 的窗口超出一点显示器就消失」）。
		 *
		 * 不裁也不会画到隔壁屏上：场景图是全局坐标，每块屏自己的节点被
		 * wlroots 裁在那块屏的框里，落不到别人那块上。这本来就是上面注释
		 * 里写的语义 —— 「行内随便溢出，出屏才切」，而出屏那一刀由输出
		 * 自己的裁剪天然完成，不需要我们在这儿再算一遍。
		 *
		 * 行矩形（row->rect_*）仍然有用：命中测试、动画目标都靠它，
		 * 只是不再当裁剪框。 */
		(void)ov_clip_box;
	}
	/* **果冻（拖拽形变）期间不裁这一份。**
	 *
	 * 这个节点的显示范围被裁到「这块屏里那一半」时，裁剪会把**节点位置**
	 * 改成可见部分的起点（下面 `node_x = inter.x`）：窗口往左拖出屏幕，
	 * 节点就被钉在这块屏的左边框上。可果冻（jelly.c）正是拿这个节点的
	 * 坐标当「窗口左上角」去摆它那张整窗的形变图 —— 于是整块窗口被画到
	 * 屏的左边缘、不再跟着光标（用户 2026-10-30 报的「第一次完全跨过去
	 * 之前会卡在 DP-2」，探针实测：tree=1287 而 src=1920 = DP-2 左边缘，
	 * rel=633 全是这一刀裁出来的偏移）。
	 *
	 * 果冻期间这一份本来就不显示：`vidow_fx_frame` 末尾把它的透明度压成 0
	 * （内容由果冻节点画）。所以这里跳过裁剪只影响「节点坐标」这一个用途，
	 * 显示的按屏裁仍然由果冻那一层 + 输出自己的裁剪完成 —— 两块屏各画
	 * 自己那一半的效果不变。 */
	if ((instance != NULL || fx->clip_on || ov_clip) && !fx->jelly) {
		/* fx->clip 是**布局坐标**里的那块屏；这个节点的位置是相对父树的
		 * （父树就是窗口的场景树，摆在窗口的位置上）—— 减掉父树的原点才
		 * 在同一个坐标系里。 */
		struct wlr_box clip = ov_clip ? ov_clip_box : fx->clip;
		if (instance != NULL && !ov_clip) {
			/* 这块屏的布局框：vidow_blur_update() 本帧开头刚写进
			 * instance->layout_box（它先于渲染趟跑），直接拿来用。
			 * 别再查 wlr_output_layout_get_box —— 那是链表遍历，
			 * 而这个函数每帧 × 每个窗口都要走一遍。 */
			struct wlr_box output_clip = instance->layout_box;
			if (output_clip.width <= 0 || output_clip.height <= 0) {
				/* 这个槽位还没被 vidow_blur_update 填过（刚建 / 模糊关着）：
				 * 现查一次，别拿空框去裁 —— 空框会让下面判成「整个身子都在
				 * 屏外」，节点被甩到 -1000000，窗口整块消失。 */
				wlr_output_layout_get_box(server->output_layout,
					instance->out, &output_clip);
			}
			/* **每块屏只画自己那一半**（照 Hyprland 的 renderMonitor）。
			 *
			 * 装饰缓冲是按屏各一份的（slot->dec*），每份都用**那块屏自己**
			 * 的模糊底衬烘好，所以这里的裁剪框必须是**这一格自己的屏框**，
			 * 不是布局总框。
			 *
			 * 用布局总框的话两份装饰会整窗重叠在同一个位置、只是倍率不同：
			 * 最后画的那份把另一份盖住 —— 2x 屏上看到 1x 烘的图（放大糊），
			 * 1x 屏上看到 2x 烘的图（缩下去倒是清楚）。两块屏轮流重烘时，
			 * 盖在上面的那一份每帧换人，就是「高分辨率那块屏背景一直闪」。
			 *
			 * 拖拽中也一样：另一半由**对面那块屏**自己那份节点画（它裁在
			 * 对面那块屏里），拼起来才是完整窗口。以前这条豁免是给「只有
			 * 一份装饰缓冲」的旧模型留的，现在每屏一份，豁免反而把两份叠
			 * 在一起。 */
			if (fx->clip_on) {
				struct wlr_box visible = {0};
				wlr_box_intersection(&visible, &clip, &output_clip);
				clip = visible;
			} else {
				clip = output_clip;
			}
		}
		if (node->node.parent != NULL) {
			clip.x -= node->node.parent->node.x;
			clip.y -= node->node.parent->node.y;
		}
		struct wlr_box node_box = {.x = node_x, .y = node_y,
			.width = dst_w, .height = dst_h};
		struct wlr_box inter;
		if (wlr_box_intersection(&inter, &node_box, &clip) &&
				(inter.width < dst_w || inter.height < dst_h)) {
			double kx = (double)fx->w / (double)dst_w;
			double ky = (double)fx->h / (double)dst_h;
			crop_src = (struct wlr_fbox){
				.x = (double)(inter.x - node_x) * kx,
				.y = (double)(inter.y - node_y) * ky,
				.width = (double)inter.width * kx,
				.height = (double)inter.height * ky,
			};
			node_x = inter.x;
			node_y = inter.y;
			dst_w = inter.width;
			dst_h = inter.height;
			cropped = true;

			if (ov_clip) {
				wlr_log(WLR_DEBUG,
					"overview clip: view %p 裁到 %d,%d %dx%d（源 %d,%d %dx%d → 显示 %dx%d）",
					(void *)fx->view, clip.x, clip.y,
					clip.width, clip.height,
					(int)crop_src.x, (int)crop_src.y,
					(int)crop_src.width, (int)crop_src.height,
					inter.width, inter.height);
			}
		} else if (!wlr_box_intersection(&inter, &node_box, &clip)) {
			/* 这一帧整个身子都在自己那块屏外面：一个像素都不该画（画了就是
			 * 画到隔壁屏上 —— 场景图是全局坐标，那边正画着这块框）。把落点
			 * 甩到所有输出框外面去：整块缓冲照旧传进去（空的源框 wlroots 会
			 * 当成「整块」，那是另一个意思），但没有任何一块屏的框里有它。
			 * 别指望「没停到屏外就一定有交集」：判据比的是**这一帧画在哪儿**，
			 * 这里用的是节点自己的落点，两者在动画帧里可能错开一拍 —— 错过
			 * 一拍就是一块窗口闪在隔壁屏上（用户报的「回来的瞬间」）。 */
			node_x = -1000000;
			node_y = -1000000;
			dst_w = 1;
			dst_h = 1;
			/* 跟手探针（touch /tmp/vidow-x11-trace 开）：拖拽期间把这一步的决定
			 * 打出来 —— 用的是哪块屏自己的那一格（instance）、裁剪到哪个框、
			 * 最后把装饰节点摆到哪儿、有没有被裁成 1x1 甩到屏外。 */
			if (vidow_xtrace_on() && fx->view != NULL &&
					(fx->view->dragging ||
					fx->view->server->grab_view == fx->view)) {
				vidow_gtrace("fx v=%d out=%s node=%d,%d %dx%d inst=%s "
					"clip_on=%d clip=%d,%d %dx%d outclip=%d,%d %dx%d "
					"crop=%d jelly=%d op=%.2f",
					fx->view->id,
					instance != NULL && instance->out != NULL ?
						instance->out->name : "-",
					node_x, node_y, dst_w, dst_h,
					instance != NULL ? "yes" : "no",
					fx->clip_on, fx->clip.x, fx->clip.y,
					fx->clip.width, fx->clip.height,
					clip.x, clip.y, clip.width, clip.height,
					cropped ? 1 : 0, fx->jelly ? 1 : 0,
					(fx->masked || fx->jelly) ? 0.0 : 1.0);
			}
		}
	}
	wlr_scene_node_set_position(&node->node, node_x, node_y);

	wlr_scene_buffer_set_dest_size(node, dst_w, dst_h);
	/* 被抓帧遮挡压着的窗口，这一帧也照样压着（抓帧随时可能在两帧之间渲染）。
	 * 正在被拖出果冻形变的窗口也一样：内容由 jelly.c 那个节点画（同一块缓冲，
	 * 只是弯了），这一块必须让位，否则两张图叠在一起。 */
	wlr_scene_buffer_set_opacity(node,
		(fx->masked || fx->jelly) ? 0.0f : 1.0f);

	/* Full-buffer damage: the node is re-rendered every frame of the effect. */
	/* 损伤按**缓冲像素**记（wlroots 的约定），裁过之后 dst_w/dst_h 只是源框那
	 * 一块的尺寸 —— 整块缓冲都可能变，就照整块缓冲报。 */
	pixman_region32_t damage;
	if (cropped) {
		pixman_region32_init_rect(&damage, 0, 0, fx->w, fx->h);
	} else {
		pixman_region32_init_rect(&damage, 0, 0, dst_w, dst_h);
	}
	wlr_scene_buffer_set_buffer_with_damage(node, displayed, &damage);
	pixman_region32_fini(&damage);
	/* 刚烘的这张是**源内容 + 装饰**的拷贝：gles2 不做转换，像素编码还是源的
	 * 那套（PQ 内容烘出来仍是 PQ）。不把标签抄过来，wlroots 会把它当「没标
	 * 色彩」—— 直通判据误放行，第 6 步以后还会按 sRGB 再转一次。 */
	vidow_color_inherit_tags(node, src);
	if (node != fx->node) {
		vidow_color_inherit_tags(fx->node, src);
	}
	/* 源框要**最后**设：换缓冲（这一路每帧在两张目标之间来回换）会把它抹掉。 */
	wlr_scene_buffer_set_source_box(node, cropped ? &crop_src : NULL);

	/* This buffer now contains this blur version: the frame loop only has to
	 * redraw when a later frame hands the window a different backdrop. */
	fx->blur_drawn = fx->blur_ver;
	if (instance != NULL) {
		instance->dec_valid = true;
		instance->dec_revision = fx->bake_revision;
		instance->dec_blur_ver = fx->blur_ver;
		instance->dec_output_scale = instance->out->scale;
		wlr_output_layout_get_box(server->output_layout, instance->out,
			&instance->dec_output_box);
	}

	/* dump_target 自己记得「dump 过几份、上限多少」（VIDOW_FX_DUMP 没设就立刻返回），
	 * 这里不再每帧问一次 getenv。 */
	dump_target(server, fx, displayed);
	return true;
}
/* 区域之类「只影响着色器输出、贴图没换」的变化要显式失效一次，
 * 否则帧循环看到 blur_drawn == blur_ver 就不重画了。 */
void vidow_view_blur_invalidate(struct vidow_view *view) {
	if (!view->fx.active) {
		return;
	}
	view->fx.blur_ver++;
	blur_ver_hits[3276]++;
	if (view->output != NULL) {
		vidow_output_schedule_frame(view->server, view->output,
			"装饰模糊失效");
	}
}

void vidow_fx_end(struct vidow_server *server, struct vidow_fx_target *fx) {
	(void)server;
	if (fx->node != NULL) {
		wlr_scene_node_set_enabled(&fx->node->node, false);
		wlr_scene_buffer_set_buffer(fx->node, NULL);
	}
	fx->shader = NULL;
	fx->phase = VIDOW_SHADER_NONE;
	fx->active = false;
	fx->anim_scale = 1.0;
	blur_target_release(fx);
	drop_targets(fx);
}

/* Drop everything except the scene node, which the parent tree owns. Use this
 * when the node may already have been destroyed with its tree. */
void vidow_fx_release(struct vidow_fx_target *fx) {
	drop_targets(fx);
	blur_target_release(fx);
	fx->shader = NULL;
	fx->phase = VIDOW_SHADER_NONE;
	fx->active = false;
	fx->anim_scale = 1.0;
	fx->node = NULL;
}

/* --------------------------------------------------------- closing windows */

/* 三份消费者的每帧入口都在各自的文件里：layer（fx-layer.h）、弹出物
 * （fx-popup.h）；关窗快照不经过这里（view.c / layer.c 直接调
 * vidow_closing_start）。这里只剩调度：遍历、决定谁画、按什么顺序画。 */

int blur_levels_for(struct vidow_server *server, int w, int h) {
	int levels = server->config.blur_levels;
	if (levels < 2) {
		levels = 2;
	}
	if (levels > MAX_BLUR_LEVELS) {
		levels = MAX_BLUR_LEVELS;
	}
	while (levels > 2 && ((w >> levels) < 4 || (h >> levels) < 4)) {
		levels--;
	}
	return levels;
}

static void shared_blur_drop(struct vidow_output *output) {
	/* 这两张贴图现在归别人管：blur_tex 是金字塔 up[0] 那张常驻贴图（blur_set_drop
	 * 收），backdrop_tex 跟着底衬缓冲走（blur_drop 收）。这里只把指针放下，
	 * 顺手把共享金字塔收掉。 */
	output->blur_tex = NULL;
	output->backdrop_tex = NULL;
	blur_set_drop(&output->blur_set);
	output->blur_id = 0;
	output->blur_gen = 0;
	output->blur_levels = 0;
}

/* Capture the scene with every toplevel hidden and run the kawase chain on it.
 * The result depends only on the wallpaper and the layer surfaces, so it stays
 * valid across client repaints, window moves and viewport scrolling. */
static bool shared_blur_rebuild(struct vidow_server *server,
		struct vidow_output *output, int w, int h, int levels) {
	struct vidow_view *view;
	struct vidow_closing *closing;

	wl_list_for_each(view, &server->views, link) {
		view_set_capture_visible(view, false);
	}
	wl_list_for_each(closing, &server->closing, link) {
		if (closing->fx.node != NULL) {
			wlr_scene_node_set_enabled(&closing->fx.node->node, false);
		}
	}
	/* layer 的模糊节点也得藏：bottom 层的那个就在被抓的场景里，不藏就是
	 * 「模糊结果里混进上一帧的模糊」。 */
	layer_blur_capture_visible(server, false);
	bool ok = backdrop_render(server, output, w, h);
	layer_blur_capture_visible(server, true);
	wl_list_for_each(view, &server->views, link) {
		view_set_capture_visible(view, true);
	}
	wl_list_for_each(closing, &server->closing, link) {
		if (closing->fx.node != NULL) {
			wlr_scene_node_set_enabled(&closing->fx.node->node, true);
		}
	}
	if (!ok) {
		return false;
	}

	/* 底衬贴图（backdrop_tex）跟着底衬缓冲一起建、一起收（见 vidow_blur_update），
	 * 金字塔的贴图跟着金字塔缓冲走（见 blur_set_alloc）：这里一张都不用导入，
	 * blur_tex 就是 up[0] 那张常驻贴图。 */
	/* blur_set_alloc() 在形状变化时会先 blur_set_drop()，旧的 up[0] 贴图
	 * 随之销毁；output->blur_tex 还指着它。这里先把指针放下，分配失败时
	 * 后面就不会拿一张已释放的贴图去采样。 */
	output->blur_tex = NULL;
	output->blur_id = 0;
	output->blur_gen = 0;
	output->blur_levels = 0;
	if (!blur_set_alloc(server, &output->blur_set, w, h, levels)) {
		return false;
	}
	blur_chain(server, &output->blur_set, output->backdrop_tex, w, h);
	output->blur_tex = output->blur_set.up_tex[0];
	if (output->blur_tex == NULL) {
		return false;
	}
	static int dumped = 0;
	if (dumped < blur_dump_limit()) {
		dump_buffer(server, output->backdrop, w, h, "vidow-bd", dumped);
		if (output->blur_set.up[0] != NULL) {
			dump_buffer(server, output->blur_set.up[0], w >> 1, h >> 1,
				"vidow-blur", dumped);
		}
		dumped++;
	}
	vidow_stats.shared++;
	output->blur_id = server->blur_serial++;
	output->blur_gen = output->backdrop_revision;
	output->blur_levels = levels;
	output->blur_offset = server->config.blur_offset;
	wlr_log(WLR_DEBUG, "blur: shared rebuild %s %dx%d levels=%d offset=%.2f",
		output->wlr_output->name, w, h, levels, output->blur_offset);
	return true;
}

/* 链条采样到底能摸到多远（原始像素）：降采样每一趟的抽样半径是 blur_offset 个
 * **源** texel，源每降一级尺寸减半，换算回原图每级翻倍（offset * (2^levels - 1)）；
 * 往上那几趟同理，只是从第 2 级起（offset * (2^(levels+1) - 4)）。两者相加就是
 * offset * (3 * 2^levels - 5)。区域要在窗口外面留出这么一圈，不然窗口边缘附近的
 * 模糊值是被区域边界 clamp 出来的（看着就是边上发暗）。 */
int blur_reach_px(struct vidow_server *server, int levels) {
	double off = server->config.blur_offset;
	if (!(off > 0.0)) {
		off = 0.0;
	}
	if (levels < 2) {
		levels = 2;
	}
	double span = 3.0 * (double)(1u << levels) - 5.0;
	int reach = (int)ceil(off * span) + 8;
	return reach < 16 ? 16 : reach;
}

/* 这块窗口的私有底衬区域（设备像素，相对这块屏的原点）。levels 出参是这块区域
 * 该用几级金字塔 —— 跟整屏那套**同一个数**：级数决定模糊半径（物理像素），各算
 * 各的话小窗口会比大窗口糊得轻。 */
static bool view_region_rect(struct vidow_server *server,
		struct vidow_output *output, struct vidow_view *view,
		int *rx, int *ry, int *rw, int *rh, int *levels) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int pad = vidow_fx_pad(server);
	/* **模糊的那一块 = 这块屏上看得见的那一块**（place 的 hit），不是整个几何框：
	 * 窗口骑在两块屏中间时，各自的区域只算自己那一半（以前按整框算，区域跨过
	 * 屏边界、被下面的夹取截成半条 —— 跨屏那一半就只有一条横道）；滚出屏 /
	 * 藏起来的工作区更是连一块都没有，压根不该走到这儿。 */
	struct vidow_place place;
	vidow_place_of_view(server, view, &place);
	const struct vidow_place_hit *hit = NULL;
	for (int i = 0; i < place.n; i++) {
		if (place.hits[i].out == output->wlr_output) {
			hit = &place.hits[i];
			break;
		}
	}
	if (!place.resident || hit == NULL) {
		return false;
	}
	struct wlr_box box = hit->box;
	int lv = blur_levels_for(server,
		output->wlr_output->width, output->wlr_output->height);
	int margin = blur_reach_px(server, lv);
	/* **这块屏自己的**布局原点（设备像素），不是 server->output_x/y。
	 *
	 * server->output_* 是「最后渲染的那块屏」留下的全局量：两屏同帧刷新时后
	 * 渲染的会把前面的覆盖掉，先渲染的那块屏上的窗口就拿错了原点 —— 区域整体
	 * 横移一屏宽，底衬采到的是隔壁屏的内容 / 屏外的空像素。用户 2026-10-23
	 * 报的「跨屏拖拽时右边跨过去的部分没有采样右边屏幕背景、还按左屏采样」
	 * 里，至少有一半是这个：窗口压在边界上时，两趟重烘互相把对方的原点写进
	 * server->output_*，谁后写谁赢。
	 *
	 * 下面 blur 那一段（decor_pass 之前的 origin_x 那几行）早就改成按
	 * output->wr_output 现算了，注释里也写着「不是 server->output_x/y」——
	 * 这里是同一个坑漏掉的一处。 */
	struct wlr_box obox = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &obox);
	int my_out_x = (int)lround((double)obox.x * scale);
	int my_out_y = (int)lround((double)obox.y * scale);
	int my_out_w = output->wlr_output->width;
	int my_out_h = output->wlr_output->height;
	/* 装饰矩形 = 可见窗口 ± pad（逻辑单位），再往外放一圈模糊半径。 */
	int left = (int)lround((double)(box.x - pad) * scale) - my_out_x - margin;
	int top = (int)lround((double)(box.y - pad) * scale) - my_out_y - margin;
	int span_w = (int)lround((double)(box.width + 2 * pad) * scale) + 2 * margin;
	int span_h = (int)lround((double)(box.height + 2 * pad) * scale) + 2 * margin;
	const int grid = VIDOW_BLUR_REGION_GRID;
	/* 原点对齐到网格（见上面那个宏）：窗口在格子里挪动时区域一个字都不变。 */
	int gx1 = (int)floor((double)left / grid) * grid;
	int gy1 = (int)floor((double)top / grid) * grid;
	/* 尺寸**只跟窗口自己的大小有关，跟位置无关**（多留一格给「窗口在格子里挪
	 * 动」）：拖窗口时区域整体平移，缓冲尺寸不变 —— 金字塔那几块缓冲才能一直
	 * 复用（尺寸一变就得整批重建 + 重新导入贴图，那正是上一版修掉的卡顿）。 */
	int size_w = (int)ceil((double)(span_w + grid) / grid) * grid;
	int size_h = (int)ceil((double)(span_h + grid) / grid) * grid;
	if (size_w < 1 || size_h < 1 || size_w > MAX_TARGET_PX ||
			size_h > MAX_TARGET_PX) {
		return false;
	}
	/* 原点再夹一下，让整块区域留在屏内：屏外没有像素可抓（那就是透明的），
	 * 贴边窗口的边缘会被链条混进黑。夹的是原点、不是尺寸 —— 尺寸照样只跟窗口
	 * 大小有关，缓冲不会因为窗口移动而重建。窗口比屏幕还大的极端情况下夹不住，
	 * 那圈就还是透明的（layer 那一路一直是这个行为）。
	 *
	 * 夹的边界是**这块屏自己的**尺寸（my_out_w/h），不是 server->output_w/h：
	 * 后者是最后渲染的那块屏的，窗口跨在边界上时两趟互相覆盖，先渲染的那趟
	 * 会拿隔壁屏的尺寸来夹 —— 区域被截到屏边缘，模糊就只剩一条（用户报的
	 * 「跨屏那一半是横条」）。 */
	int max_x = my_out_w - size_w;
	int max_y = my_out_h - size_h;
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

/* 抓「世界减掉这个窗口」，只抓它自己那一小块区域，然后跑一遍链条。
 *
 * 基座是共享底衬（壁纸 + background / bottom 层）里对应的那一块，1:1 拷过来，
 * 别的窗口再画上去 —— 这就是「模糊里得有窗口」；而这个窗口自己在抓的时候是藏起来
 * 的（它不该出现在自己的底衬里）。以前这里是整屏重画一遍（连壁纸一起），每帧每块
 * 重叠的窗口都要来一次；现在代价只跟这块区域的面积成正比。 */
static bool view_region_build(struct vidow_server *server,
		struct vidow_output *output, struct vidow_view *view,
		int rx, int ry, int rw, int rh, int levels,
		const struct wlr_box *partial) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}

	/* partial != NULL：只有这块（**区域局部设备像素**）的像素内容变了，其余保持
	 * 上一版 —— 抓也只抓这一块、链条也只重跑这一块（见 blur_chain_rect）。
	 * partial == NULL：整块重跑（第一次 / 形状变了 / 损伤盖满整块）。 */
	int px = 0, py = 0, pw = rw, ph = rh;
	if (partial != NULL) {
		px = partial->x < 0 ? 0 : partial->x;
		py = partial->y < 0 ? 0 : partial->y;
		int px2 = partial->x + partial->width;
		int py2 = partial->y + partial->height;
		if (px2 > rw) {
			px2 = rw;
		}
		if (py2 > rh) {
			py2 = rh;
		}
		if (px2 <= px || py2 <= py) {
			return false;
		}
		pw = px2 - px;
		ph = py2 - py;
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
	if (partial != NULL) {
		/* 要抓的那一块也得夹进屏内（区域原点可能被夹过，右/下边可能是屏外）。 */
		int cx1 = rx + px, cy1 = ry + py;
		int cx2 = cx1 + pw, cy2 = cy1 + ph;
		if (cx1 < x1) {
			cx1 = x1;
		}
		if (cy1 < y1) {
			cy1 = y1;
		}
		if (cx2 > x2) {
			cx2 = x2;
		}
		if (cy2 > y2) {
			cy2 = y2;
		}
		if (cx2 <= cx1 || cy2 <= cy1) {
			return false;
		}
		px = cx1 - rx;
		py = cy1 - ry;
		pw = cx2 - cx1;
		ph = cy2 - cy1;
	}

	struct vidow_fx_target *fx = &view->fx;
	struct vidow_blur_slot *sl = blur_slot_for(fx, output->wlr_output, true);
	if (sl == NULL) return false;
	/* 这一版是被哪一块损伤触发的（只给下面那行日志用：pending_damage 这里就要
	 * 清掉了）。修「客户端只动了一小块，整片模糊却重抓」时，看的就是这个数。 */
	struct wlr_box dmg = dmg_bounds(&fx->pending_damage);
	if (sl->own_bd == NULL || sl->own_bd_w != rw || sl->own_bd_h != rh) {
		if (sl->own_bd_tex) stat_tex_del(sl->own_bd_tex);
		sl->own_bd_tex = NULL;
		if (sl->own_bd) wlr_buffer_drop(sl->own_bd);
		sl->own_bd = NULL;
		sl->own_bd = vidow_alloc_target(server, rw, rh);
		if (sl->own_bd == NULL) {
			return false;
		}
		sl->own_bd_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, sl->own_bd));
		if (sl->own_bd_tex == NULL) {
			wlr_buffer_drop(sl->own_bd);
			sl->own_bd = NULL;
			return false;
		}
		sl->own_bd_w = rw;
		sl->own_bd_h = rh;
	}

	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, sl->own_bd, NULL);
	if (pass == NULL) {
		return false;
	}
	struct wlr_render_rect_options clear = {
		.box = { .x = px, .y = py, .width = pw, .height = ph },
		.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_rect(pass, &clear);

	/* 基座：共享底衬（壁纸 + background / bottom 层）里对应的那一块，1:1 捞过来
	 * （不插值、不混合 —— 这块缓冲刚清成全透明，直接覆盖写最便宜）。源框必须
	 * 夹在底衬里面：wlroots 的 render pass 会断言源框越界（伸到屏外的那圈只能
	 * 留透明，所以 view_region_rect 把区域原点夹在屏内）。 */
	int bx1 = rx + px, by1 = ry + py;
	if (bx1 < x1) {
		bx1 = x1;
	}
	if (by1 < y1) {
		by1 = y1;
	}
	int bx2 = bx1 + pw, by2 = by1 + ph;
	if (bx2 > x2) {
		bx2 = x2;
	}
	if (by2 > y2) {
		by2 = y2;
	}
	float one = 1.0f;
	struct wlr_render_texture_options base = {
		.texture = output->backdrop_tex,
		.src_box = {
			.x = (float)bx1, .y = (float)by1,
			.width = (float)(bx2 - bx1), .height = (float)(by2 - by1),
		},
		.dst_box = {
			.x = (float)(bx1 - rx), .y = (float)(by1 - ry),
			.width = (float)(bx2 - bx1), .height = (float)(by2 - by1),
		},
		.alpha = &one,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.filter_mode = WLR_SCALE_FILTER_NEAREST,
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_texture(pass, &base);

	/* 窗口画在基座上：这个窗口自己藏起来，别的照画。只画 toplevels —— 弹出物那
	 * 一层在窗口**上面**，不该进窗口的底衬（共享底衬那趟也是这么做的）。 */
	/* 私有底衬抓的是「世界减掉这个窗口」：整棵窗口树一起关，比只关装饰 /
	 * 果冻节点更硬 —— 客户端表面、阴影、任何漏网的子节点都不会混进来。 */
	bool tree_on = view->scene_tree != NULL &&
		view->scene_tree->node.enabled;
	if (tree_on) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, false);
	}
	view_set_capture_visible(view, false);
	layer_blur_capture_visible(server, false);
	struct backdrop_ctx ctx = {
		.pass = pass,
		.server = server,
		.scale = scale,
		.ox = server->output_x + rx,
		.oy = server->output_y + ry,
		/* 只画要抓的那一块（partial 时就是损伤那一小块）：底衬别的像素一个都
		 * 不碰，其余部分还是上一版的内容。 */
		.clip_x = bx1 - rx,
		.clip_y = by1 - ry,
		.clip_w = bx2 - bx1,
		.clip_h = by2 - by1,
		.skip = view,
		/* -1 = 不按层号筛。 */
		.below_layer = -1,
	};
	wlr_scene_node_for_each_buffer(&server->layer_toplevels->node, backdrop_iter,
		&ctx);
	layer_blur_capture_visible(server, true);
	view_set_capture_visible(view, true);
	if (view->scene_tree != NULL) {
		/* 恢复抓之前的状态，而不是一律 true：隐藏工作区里的窗口本来就是
		 * 关着的，抓完不能把它点亮。 */
		wlr_scene_node_set_enabled(&view->scene_tree->node, tree_on);
	}
	if (!wlr_render_pass_submit(pass)) {
		return false;
	}

	vidow_stats.own++;
	dmg_clear(&fx->pending_damage); /* 这一版的账已经算清了 */
	sl->tex = NULL;
	sl->blur_own_tex = NULL;
	if (!blur_set_alloc(server, &sl->blur_set, rw, rh, levels)) {
		return false;
	}
	if (partial != NULL) {
		blur_chain_rect(server, &sl->blur_set, sl->own_bd_tex, rw, rh,
			&(struct wlr_box){ .x = px, .y = py,
				.width = pw, .height = ph },
			blur_reach_px(server, levels));
	} else {
		blur_chain(server, &sl->blur_set, sl->own_bd_tex, rw, rh);
	}
	sl->blur_own_tex = sl->blur_set.up_tex[0];
	if (sl->blur_own_tex == NULL) {
		return false;
	}
	/* 缓存键是「世界减掉这个窗口」的代次（见 vidow_backdrop_damage_view）。 */
	sl->blur_set.gen = server->gen_scene - view->self_gen;
	sl->blur_set.valid = true;
	sl->blur_own_id = server->blur_serial++;
	/* 这块区域本身（blur_tex_x/y/w/h）不在这儿登记：它跟着贴图一起由
	 * blur_assign() 换（见那里的注释），一处管一处，免得两边不一致。 */

	static int dumped = 0;
	if (dumped < blur_dump_limit()) {
		dump_buffer(server, sl->own_bd, rw, rh, "vidow-bd", dumped);
		/* 测试要知道这份底衬是谁的：文件名保持 vidow-bd-<n>（别的测试按它
		 * 排序取最后一份），owner 单独一个 sidecar，不碰 .txt 的格式。 */
		char opath[128];
		snprintf(opath, sizeof(opath), "/tmp/vidow-bd-%d.owner", dumped);
		FILE *of = fopen(opath, "w");
		if (of != NULL) {
			fprintf(of, "%s\n", vidow_view_title(view) != NULL ?
				vidow_view_title(view) : "?");
			fclose(of);
		}
		if (sl->blur_set.up[0] != NULL && rw > 1 && rh > 1) {
			dump_buffer(server, sl->blur_set.up[0], rw >> 1, rh >> 1,
				"vidow-blur", dumped);
		}
		dumped++;
	}
	/* 这一行每次模糊重跑一行（滚动 / 动画里每帧每个窗口都可能触发）。
	 * 日常使用不该刷，但排查「模糊重跑为什么这么频繁 / 损伤框算错」时它是
	 * 唯一的线索 —— 用 VIDOW_BLUR_TRACE=1 开，别常驻。 */
	if (blur_trace_on()) {
		wlr_log(WLR_DEBUG, "blur: own region %s %dx%d at %d,%d levels=%d"
			"（损伤 %dx%d at %d,%d，%s %dx%d）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			rw, rh, rx, ry, levels,
			dmg.width, dmg.height, dmg.x, dmg.y,
			partial != NULL ? "局部重跑" : "整块重跑", pw, ph);
	}
	return true;
}

/* Free everything this output owns. Called when the output goes away or its
 * size/scale changes.
 *
 * 借出去的贴图也在这里销毁（共享的那份是所有不重叠窗口一起用的），所以每个借过它
 * 的窗口都必须松手：热插拔把窗口挪到另一块屏时，紧接着就会重摆 + 重画，手里要是
 * 还攥着已经销毁的贴图，GLES2 那边直接是 `wlr_texture_is_gles2` 断言崩溃。
 * 下一帧 blur_update() 会把该借的重新借回来，所以这里清空不丢东西。 */
void vidow_blur_output_finish(struct vidow_output *output) {
	shared_blur_drop(output);
	blur_drop(output);

	struct vidow_view *view;
	wl_list_for_each(view, &output->server->views, link) {
		struct vidow_blur_slot **link = &view->fx.blur_slot;
		while (*link != NULL) {
			struct vidow_blur_slot *sl = *link;
			if (sl->out != output->wlr_output) {
				link = &sl->next;
				continue;
			}
			*link = sl->next;
			if (view->fx.cur_slot == sl) view->fx.cur_slot = NULL;
			instance_drop(sl);
		}
		if (view->fx.bake_output == output->wlr_output)
			view->fx.bake_output = NULL;
		if (view->fx.blur_owned) {
			continue;
		}
		blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
	}
}

/* 这块窗口的私有模糊底衬归哪块屏。
 *
 * **必须和装饰离屏缓冲的倍率用同一个判据**（view.c 的 view_scale 走的是
 * view_home_output：窗口 -> 工作区 -> 屏，拖动中也不破例 —— 窗口还没换工作区就
 * 还在原来那块屏上）。底衬的尺寸和原点（blur_out_*）是按这块屏的设备像素记的，
 * 装饰着色器再拿它算采样 UV；两边判据一旦不一致：
 *
 *   - 倍率按旧屏（1x，离屏缓冲 2076 宽）、底衬按新屏（2x，区域按 3840 记），
 *     UV 的除数和实际贴图对不上 —— 屏幕上是一条一条的横道（用户报的
 *     「跨屏拖拽背景全是横条」）；
 *   - 反过来（倍率按新屏、底衬按旧屏）就是整块底衬被拉成中间一小块。
 *
 * 以前这里用的是 vidow_view_display_output（「看得见的那块屏」，会给正在跨屏
 * 拖的窗口换成对面那块屏），注释里写着「和 view_scale 用同一个判据」，可实际上
 * 两者根本不是一回事。现在改成取同一个来源。 */
static struct wlr_output *view_blur_owner(struct vidow_view *view) {
	struct wlr_output *out = vidow_view_display_output(view);
	return out != NULL ? out : view->output;
}

/* 窗口和这块屏有没有交集 —— 也就是「这块屏这一趟要不要管它的底衬」。
 *
 * 照 Hyprland 的 shouldRenderWindow(pWindow, pMonitor)（Renderer.cpp）：它没有
 * 「跨屏的窗口」这个概念，主循环对每块显示器各问一次「窗口和我的交集是不是
 * 空的」，非空就在这块屏上画一次、用这块屏自己的模糊纹理（m_blurFB 每屏一张）。
 *
 * 以前这里只认 view_blur_owner（窗口**归属**的那一块屏），于是窗口骑在两块屏
 * 中间时，跨过去的那半边没有对面屏的底衬可用 —— 屏幕上就是用户 2026-10-23
 * 报的「一半正常圆斑、另一半被拉成横条」。
 *
 * 判据用 screen_box（含动画偏移，和裁剪 / 倍率同一份几何）。 */
/* ob = 这块屏的布局框，由调用方算好传进来。
 *
 * **别在这儿自己查**：这是个每帧 × 每个窗口都会走到的判据，
 * wlr_output_layout_get_box 是一次输出布局链表遍历。以前它就在函数体里，
 * N 个窗口每帧 N 次同样的遍历 —— 而调用方（vidow_blur_update）本来就要
 * 这个值（它要按屏的原点算底衬 UV），算一遍传下来就行。 */
static bool view_blur_hits_output(struct vidow_view *view,
		struct vidow_output *output, const struct wlr_box *ob) {
	/* **判据走 place**：在这块屏上有没有像素，看 place 的 hits 里有没有这一块
	 * （不是「归属是不是这一块」—— 浮窗骑在边界上时两块屏各有一半，各自都要
	 * 管自己那半的底衬）。resident 为假（停泊 / 藏起来 / 最小化）时直接落到
	 * 下面的归属收尾那一条。 */
	struct vidow_place place;
	vidow_place_of_view(view->server, view, &place);
	if (place.resident) {
		for (int i = 0; i < place.n; i++) {
			if (place.hits[i].out == output->wlr_output) {
				return true;
			}
		}
	}
	(void)ob;
	/* 不相交的窗口仍旧归它自己那块屏管（收尾 / 释放），否则跨屏结束那一下
	 * 没人来把已经用不上的底衬收掉。 */
	struct wlr_output *own = view_blur_owner(view);
	if (own == output->wlr_output) {
		/* 别在这儿打日志：这是每帧 × 每个窗口都会走到的判据，之前留了一行
		 * DEBUG 在这儿，日常使用直接刷出 8.6GB 日志（实测占全部日志的 93%，
		 * 而且经常同一毫秒重复好几行）。要查跨屏模糊归属就用 blur 那条统计行
		 * 或者临时加回这一行，别常驻。 */
		return true;
	}
	return false;
}

/* Called once per frame before anything is drawn, so the decorations can
 * sample this frame's backdrop. Everything is cached: a frame where nothing
 * moved behind a window touches no GPU buffer at all. */
void vidow_blur_update(struct vidow_output *output) {
	struct vidow_server *server = output->server;
	struct vidow_view *view;

	/* 这一帧的序号（共享抓取按它判断「同一帧只抓一次」）。放在最前面：早退
	 * 的那一帧也往前走一格，下一帧真要画时就不会复用上一帧的共享图。 */
	blur_frame_seq++;

	/* 这块屏的布局框：只跟 output 有关，算一次给下面所有窗口共用
	 * （以前每个窗口每帧各查一遍，见 view_blur_hits_output 的注释）。 */
	struct wlr_box out_box = {0};
	wlr_output_layout_get_box(server->output_layout,
		output->wlr_output, &out_box);

	int w = output->wlr_output->width, h = output->wlr_output->height;
	bool usable = server->shader_ok && server->config.blur_enabled && w > 0 &&
		h > 0 && w <= MAX_TARGET_PX && h <= MAX_TARGET_PX;

	/* **这一帧什么都没变就直接走人。**
	 *
	 * gen_scene 是「世界里有多少东西动过」的计数器（damage_bump 每标一次
	 * +1）：窗口交帧、拖动、配置重载、layer 上下屏、输出重排全都会让它前进。
	 * 它没动 = 上一帧算好的账这一帧还成立 —— 下面那一长串遍历（views ×
	 * layers × popups，每一项都要解几何、查槽位、比 mask）纯属每帧白跑。
	 *
	 * 只在 usable 这条路上早退：!usable 那条是「收回已经建出来的资源」，
	 * 配置刚把模糊关掉的那一刻 gen_scene 不一定动过，必须每帧都能跑到。
	 *
	 * 记账放在判断**之后**、遍历之前：遍历中途 return 掉也已经记上了，因为
	 * 这一帧的结论对整帧都成立。 */
	if (usable && output->blur_seen_scene == server->gen_scene &&
			output->blur_seen_layer == server->gen_layer) {
		return;
	}
	if (usable) {
		output->blur_seen_scene = server->gen_scene;
		output->blur_seen_layer = server->gen_layer;
	}

	if (!usable) {
		wl_list_for_each(view, &server->views, link) {
			if (!view_blur_hits_output(view, output, &out_box)) {
				continue; /* 别的屏上的窗口归那一趟收 */
			}
			blur_target_release(&view->fx);
		}
		struct vidow_layer_surface *off;
		wl_list_for_each(off, &server->layers, link) {
			/* 只收**这块屏**的：上面的 width/height 是 per-output 的，
			 * 一块屏尺寸越界（超过 MAX_TARGET_PX）不代表别的屏也不能画。
			 * 不筛的话，那块超尺寸的屏一帧就能把另一块屏上正常工作的
			 * layer 模糊节点销毁掉，而另一块屏的 usable 是 true、根本不会
			 * 走到这个分支来重建 —— 那块屏的模糊就此消失。 */
			if (off->output != output) {
				continue;
			}
			vidow_layer_blur_release(off);
		}
		struct vidow_popup_blur *pb;
		wl_list_for_each(pb, &server->popup_blurs, link) {
			struct vidow_output *owner = popup_blur_owner(pb);
			if (owner == output || owner == NULL) {
				popup_blur_release(pb);
			}
		}
		shared_blur_drop(output);
		blur_drop(output);
		return;
	}

	/* Keep caches allocated, but do not capture/blur a wallpaper nobody can
	 * sample. Be conservative for popups and overview; stale generations are
	 * retained so the first returning consumer rebuilds before sampling. */
	bool consumer = vidow_overview_active(server);
	wl_list_for_each(view, &server->views, link) {
		if (view_wants_blur(view) && view_blur_hits_output(view, output, &out_box)) {
			consumer = true;
			break;
		}
	}
	struct vidow_layer_surface *consumer_layer;
	wl_list_for_each(consumer_layer, &server->layers, link) {
		if (consumer_layer->output == output &&
				layer_blur_wanted(server, consumer_layer)) {
			consumer = true;
			break;
		}
	}
	struct vidow_popup_blur *consumer_popup;
	wl_list_for_each(consumer_popup, &server->popup_blurs, link) {
		if (popup_blur_owner(consumer_popup) == output) {
			consumer = true;
			break;
		}
	}
	bool shared_ok = false;
	if (!consumer) {
		/* Still run consumer cleanup and decoration updates below. */
		goto update_consumers;
	}

	int levels = blur_levels_for(server, w, h);

	if (output->backdrop == NULL || output->backdrop_w != w ||
			output->backdrop_h != h) {
		blur_drop(output);
		shared_blur_drop(output); /* the pyramid is sized for the old output */
		output->backdrop = vidow_alloc_target(server, w, h);
		if (output->backdrop == NULL) {
			return;
		}
		/* 未模糊的底衬也留一张贴图：被窗口遮住的 layer surface 拿它当自己那一
		 * 小块区域底衬的基座（比每次重抓一遍壁纸便宜太多，见 layer_region_build）。
		 * 贴图是包的缓冲，不包像素 —— 底衬每帧重画也不影响它，建一次就够。 */
		output->backdrop_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, output->backdrop));
		if (output->backdrop_tex == NULL) {
			blur_drop(output);
			return;
		}
		output->backdrop_w = w;
		output->backdrop_h = h;
	}

	/* 缓存键必须把所有影响这张贴图的参数都算上：级数、采样步长。
	 * 漏了 blur_offset 的时候，运行时改它看着「没反应」—— 贴图是旧参数算的，谁也
	 * 没让它重算。（blur_alpha_min 原来也在这儿，现在它只进装饰着色器、不进金字塔，
	 * 改它由 blur_mask_min_changed() 直接让帧循环重画。） */
	shared_ok = output->blur_tex != NULL &&
		output->blur_gen == output->backdrop_revision &&
		output->blur_levels == levels &&
		fabs(output->blur_offset - server->config.blur_offset) < 1e-6 &&
		output->blur_set.w == w && output->blur_set.h == h;
	if (!shared_ok) {
		shared_ok = shared_blur_rebuild(server, output, w, h, levels);
	}

update_consumers:
	/* **这块屏自己的**布局原点（设备像素），不是 server->output_x/y。
	 * 那两个是「最后渲染的那块屏」留下的全局量：两屏同帧刷新时，
	 * 后渲染的屏会把前面的覆盖掉，先渲染的屏那批窗口就拿错原点 ——
	 * 底衬整块横向错位，屏幕上就是用户报的「跨屏模糊一直是一条条横纹」。
	 * 布局原点按设备像素算：output->wlr_output 的布局框是逻辑坐标，
	 * 乘自己的倍率才是底衬贴图所在的那个像素空间。
	 *
	 * 提到循环外：这组值只跟 output 有关、跟 view 无关，以前放在窗口循环体内，
	 * 每个窗口每帧都重算一次 wlr_output_layout_get_box（那是一次输出布局链表
	 * 遍历）。N 个窗口就是 N 次同样的遍历，纯浪费。 */
	double oscale_o = output->wlr_output->scale > 0.0 ?
		output->wlr_output->scale : 1.0;
	/* 布局框在函数开头已经查过一次了（out_box），别再查一遍。 */
	const struct wlr_box *obox = &out_box;
	int my_out_x = (int)lround((double)obox->x * oscale_o);
	int my_out_y = (int)lround((double)obox->y * oscale_o);

	wl_list_for_each(view, &server->views, link) {
		/* 私有底衬是「**这块屏**上的这一块区域」，别的屏上的窗口不归这一趟管。
		 *
		 * 少了这一句，两块屏会轮流给同一个窗口建金字塔：每块屏都按自己的
		 * scale / 尺寸算区域（1 倍屏把 280x380 的窗口算成 640x768，2 倍屏算成
		 * 896x1152），而金字塔是挂在窗口身上的（view->fx.blur_set），于是两趟
		 * 每帧互相把对方的缓冲丢掉重建 —— 一次十几块 GBM 缓冲（最大那块
		 * 2304x2560 = 23 MB），拖窗口时每秒几千次分配，屏幕上一顿一顿的。
		 * 顺带也修了「区域和贴图归哪块屏」：以前最后写进 fx 的是哪一趟算的
		 * 就归谁，跨屏的窗口可能采到另一块屏的底衬。
		 *
		 * 归属判据和装饰离屏缓冲的倍率是**同一个**（view_blur_owner ——
		 * 「窗口现在画在哪块屏上」）：两个地方各算各的话，拖到对面那块屏上时
		 * 倍率对不上，模糊贴图的采样坐标就落错地方（那块区域是按底衬这块屏的
		 * 设备像素记的）。 */
		/* 窗口归哪块屏就用哪块屏的底衬。
		 *
		 * 试过「和这块屏相交就建」（照 Hyprland 的 per-monitor 模型改）——
		 * 不行：窗口的私有底衬是**一份**（view->fx 里 blur_tex_x/y/w/h 那几个
		 * 字段是单值），两块屏同帧各建一份、后写的把先写的覆盖掉，结果整窗
		 * 都变成横条，比改之前更糟。真要走那条路，得把 fx 那组字段改成每屏
		 * 一份，是另一层重构（见 server.h 的 blur_set 注释）。 */
		if (!view_blur_hits_output(view, output, &out_box)) {
			continue;
		}
		/* 这张贴图代表的是**这块屏**的视野：尺寸和布局原点跟着贴图一起记在 fx
		 * 上（见 server.h 那组 blur_out_*）。采样只认这一组，不现场读
		 * server->output_*：重烘那趟会给别的屏上的窗口也烘一次，那一刻的
		 * server->output_w 是那块屏的，按它算 UV 就是「只取中间一块铺满整窗」。 */
		/* 视野几何变了就得重烘：UV 是在**烘的那一刻**按这一组算的（见下面
		 * decor_pass 之前那几个 origin_x / out_size_x），烘好之后光改字段
		 * 不会让已经烘进去的 UV 跟着变。
		 *
		 * 跨屏拖动正是「贴图身份没变、视野换了」的那一类：同倍率的两块屏
		 * 之间拖，模糊贴图的尺寸和内容只差一个原点，blur_assign() 判
		 * blur_id 相同就早退、blur_ver 不动 —— 装饰拿着旧屏的 origin 去采
		 * 新屏的贴图，整块底衬错位 / 拉花（用户报的「跨屏后背景模糊纹理
		 * 会出问题」）。所以视野这四个数变了就自己把版本号推一格。 */
		/* **这块屏自己的**布局原点（设备像素），不是 server->output_x/y。
		 * 那两个是「最后渲染的那块屏」留下的全局量：两屏同帧刷新时，
		 * 后渲染的屏会把前面的覆盖掉，先渲染的屏那批窗口就拿错原点 ——
		 * 底衬整块横向错位，屏幕上就是用户报的「跨屏模糊一直是一条条横纹」。
		 *
		 * 值在循环外算好了（见 update_consumers 上面那一段）：它只跟 output
		 * 有关，跟 view 无关，放循环里就是每个窗口重算一遍。 */
		/* 视野写进**这块屏自己的那一格**（跨屏时另一格是另一块屏的，不能碰）。 */
		struct vidow_blur_slot *oslot = blur_slot_for(&view->fx,
			output->wlr_output, true);
		if (oslot != NULL) {
			if (oslot->ow != output->wlr_output->width ||
					oslot->oh != output->wlr_output->height ||
					oslot->ox != my_out_x || oslot->oy != my_out_y) {
				view->fx.blur_ver++;
				blur_ver_hits[4415]++;
			}
			oslot->layout_box = *obox;
			oslot->ow = output->wlr_output->width;
			oslot->oh = output->wlr_output->height;
			oslot->ox = my_out_x;
			oslot->oy = my_out_y;
		}
		/* Per-output instances already checked their own geometry above.
		 * Updating legacy fallback fields must not invalidate both instances
		 * whenever two different outputs take turns rendering this view.
		 *
		 * **拖拽中这四个单值字段干脆不写**（第 6 步）：窗口两半各压一块屏，
		 * 两趟更新每帧互相覆盖。读取端（decor_draw / jelly）在 cur_slot /
		 * rpass 里取的都是每屏那格，单值只在「没有格子上下文」的兜底路上
		 * 用 —— 拖拽中把单值钉在最后写的那块屏上，兜底路（jelly 单份模式）
		 * 就会用错屏的原点。松手后归属稳定，照旧写回。 */
		if (!(view->dragging || server->grab_view == view)) {
			if (oslot == NULL &&
					(view->fx.blur_out_w != output->wlr_output->width ||
					view->fx.blur_out_h != output->wlr_output->height ||
					view->fx.blur_out_x != my_out_x ||
					view->fx.blur_out_y != my_out_y)) {
				view->fx.blur_ver++;
				blur_ver_hits[4431]++;
			}
			view->fx.blur_out_w = output->wlr_output->width;
			view->fx.blur_out_h = output->wlr_output->height;
			view->fx.blur_out_x = my_out_x;
			view->fx.blur_out_y = my_out_y;
		}
		/* **bake_output 归属：每屏一格模型下的「这一格跟谁烘」。**
		 *
		 * 只在**归属真的变了**的时候写：判据用 view_blur_owner（跟倍率、
		 * 底衬归属同一个真值，见它的注释）。跨屏那一半的临时借用由
		 * view_bake_for_output 自己存/恢复，不经过这里。
		 *
		 * **例外：正被拖的窗口不写**（第 6 步）。拖一半时窗口画在两块屏
		 * 上，两块屏的格子都在用；把它钉到 owner（= 布局归属，要等松手
		 * 才换）上，DP-3 那一趟的 ensure_targets / fx_blur_tex_now 就都查
		 * 到 DP-2 的格子 —— DP-3 画的还是 DP-2 的底衬，正是「拖一半 DP-3
		 * 背景不画、还从 DP-2 错位」。拖拽期间 bake_output 由
		 * view_bake_for_output 的借用机制按屏切换，这里不碰。 */
		if (view->dragging || server->grab_view == view) {
			/* 拖拽中且从未有过归属（浮窗起拖）：给它按当前屏起一格。 */
			if (view->fx.bake_output == NULL) {
				view->fx.bake_output = output->wlr_output;
			}
		} else {
			struct wlr_output *owner_out = view_blur_owner(view);
			if (owner_out != NULL) {
				view->fx.bake_output = owner_out;
			} else if (view->fx.bake_output == NULL) {
				/* 没有归属（浮窗 / 弹窗）：退回「这一趟画的这块屏」——
				 * 但只在从没设过时写一次，不跟着每帧翻。 */
				view->fx.bake_output = output->wlr_output;
			}
		}
		if (!view_wants_blur(view) || view->fx.node == NULL) {
			if (view->fx.blur_own_tex != NULL) {
				blur_own_drop(&view->fx);
			}
			own_bd_drop(&view->fx); /* 这块屏上不再需要私有底衬了 */
			blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
			continue;
		}

		blur_mask_min_changed(&view->fx, server->config.blur_alpha_min);

		struct vidow_look look;
		vidow_view_look(&server->config, &view->rule_eff,
			view->fullscreen, &look);
		bool overlaps = true;
		if (ov_stats_on()) {
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			overlaps = view_overlaps_other(view);
			clock_gettime(CLOCK_MONOTONIC, &t1);
			ov_ns += (uint64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull +
				(uint64_t)(t1.tv_nsec - t0.tv_nsec);
			ov_n++;
		} else {
			overlaps = view_overlaps_other(view);
		}
		if (look.blur_xray || !overlaps) {
			/* Tiled and non-overlapping floating windows all share the
			 * wallpaper backdrop: one capture and one chain for all of them.
			 * Only drop a private pyramid if there really is one, otherwise
			 * the identity would change every frame and force a redraw.
			 *
			 * blur_xray 也走这条路：重叠时照样用整屏那份（壁纸 + layer 那层），
			 * 忽略别的窗口 —— 糊的是壁纸而不是压着的那个窗口（Hyprland 的
			 * blur:xray 同款）。私有底衬一块不建：重叠场景每帧的趟数从
			 * N×(1+2·级数−1) 掉到 ~3，那块显存也全免。 */
			if (view->fx.blur_own_tex != NULL) {
				blur_own_drop(&view->fx);
			}
			own_bd_drop(&view->fx); /* 不再重叠：私有底衬那块区域可以还了 */
			blur_assign(&view->fx, output->wlr_output, output->blur_tex, false, output->blur_id, NULL);
			continue;
		}

		if (!shared_ok) {
			blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
			continue;
		}

		if (oslot == NULL) continue;
		int rx, ry, rw, rh, rlevels;
		if (!view_region_rect(server, output, view, &rx, &ry, &rw, &rh,
				&rlevels)) {
			blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
			continue;
		}
		/* 抓的时候这个窗口是藏起来的，所以它自己的位置和内容都不进这张图：
		 * 世界代次只是粗筛，真正的判据是「这次损伤有没有碰到我这一块」—— 别的
		 * 窗口在屏幕另一头重画（视频 / 浏览器每帧都在交帧）不该让我重抓。
		 * 区域本身已经把模糊半径算进去了（见 view_region_rect）。 */
		double oscale = output->wlr_output->scale;
		if (!(oscale > 0.0)) {
			oscale = 1.0;
		}
		/* 换算成布局逻辑坐标跟损伤账本比对（账本记的是逻辑坐标）。
		 *
		 * 右 / 下边界用 ceil((rx+rw)/scale) 精确盖住，**别多加 1**：贴着
		 * 屏边缘的区域多 1 个逻辑像素就越进隔壁屏的布局范围，隔壁屏上
		 * 全屏游戏每交一帧，这边静止窗口的底衬就被判「受损」重抓一遍
		 * （2026-10-31 实测：DP-3 右缘露 3px 的 btop，被 DP-2 的游戏拖着
		 * 每秒重抓 120 次 + 重烘装饰 + 把整块屏拉起来陪跑）。 */
		int lg_x = output->box.x + (int)floor((double)rx / oscale);
		int lg_y = output->box.y + (int)floor((double)ry / oscale);
		int lg_r = output->box.x +
			(int)ceil((double)(rx + rw) / oscale);
		int lg_b = output->box.y +
			(int)ceil((double)(ry + rh) / oscale);
		struct wlr_box region_logical = {
			.x = lg_x,
			.y = lg_y,
			.width = lg_r - lg_x,
			.height = lg_b - lg_y,
		};
		bool key_changed =
			oslot->blur_set.gen != server->gen_scene - view->self_gen;
		bool touched = key_changed &&
			blur_damage_hits(&view->fx, &region_logical);
		/* 形状（区域位置尺寸 / 级数 / 半径）没变：那张金字塔就是上一版的，可以
		 * 只更新受损的那一块（见 view_region_build 的 partial）。 */
		bool shape_ok = oslot->blur_set.valid &&
			oslot->blur_own_tex != NULL &&
			oslot->blur_set.w == rw && oslot->blur_set.h == rh &&
			oslot->blur_set.levels == rlevels &&
			fabs(oslot->blur_set.offset - server->config.blur_offset) < 1e-6 &&
			oslot->rx == rx && oslot->ry == ry &&
			oslot->rw == rw && oslot->rh == rh &&
			oslot->own_bd != NULL && oslot->own_bd_w == rw &&
			oslot->own_bd_h == rh;
		bool fresh = shape_ok && !touched;
		if (!fresh) {
			struct wlr_box upd;
			bool partial = shape_ok && !blur_no_partial() &&
				region_update_rect(view->server, &view->fx, output,
					rx, ry, rw, rh, &upd);
			if (!view_region_build(server, output, view, rx, ry, rw, rh,
					rlevels, partial ? &upd : NULL)) {
				blur_assign(&view->fx, NULL, NULL, false, 0, NULL);
				continue;
			}
		}
		/* owned = false：这张贴图归 oslot->blur_set（blur_set_drop 收），
		 * 这里只是指过去采样。区域跟着一起报（见 blur_assign）。 */
		blur_assign(&view->fx, output->wlr_output, oslot->blur_own_tex, false,
			oslot->blur_own_id,
			&(struct wlr_box){ .x = rx, .y = ry, .width = rw, .height = rh });
	}

	/* layer surface 背后的模糊（状态栏 / 面板那层）。bottom 层用共享的那份底衬
	 * （壁纸 + background 层），一次抓取都不用；top / overlay 层底下有窗口，各
	 * 自抓自己那一小块（见 layer_region_build）。 */
	/* 这一帧要不要把「layer 之下」抓一次给所有层共用（只有区域互相压住才
	 * 值得，见 layer_under_plan）。 */
	layer_under_plan(server, output);
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->output != output) {
			continue;
		}
		if (!layer_blur_wanted(server, ls)) {
			vidow_layer_blur_release(ls);
			continue;
		}
		/* overview 里 top / overlay 那几棵树整体被关掉了，这层模糊没人看得见：
		 * 别每帧白抓一份（overview 的动画每帧都在动世界代次）。 */
		if (vidow_overview_active(server) &&
				ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM) {
			continue;
		}
		/* 这一帧画不出来（两个目标都还在飞 / 底衬还没建好）就留着上一帧的
		 * 节点：别一帧有、一帧没有地闪。 */
		(void)layer_blur_frame(server, output, ls);
	}

	/* 弹出物背后的模糊（xdg_popup / X11 菜单 / 输入法候选框）：跟 layer 那条
	 * 路同一套，每块屏只管「中心落在自己身上」的那些。 */
	struct vidow_popup_blur *pb;
	wl_list_for_each(pb, &server->popup_blurs, link) {
		popup_blur_frame(server, output, pb);
	}

	/* overview：整屏一张糊掉的壁纸（niri 那样），垫在压暗遮罩下面。 */
	vidow_overview_blur_frame(server, output);

	/* **跨屏拖拽：这块屏把自己那一半按自己的倍率画一遍。**
	 *
	 * 照 Hyprland（Renderer.cpp 的 renderMonitor 逐屏循环）：没有「跨屏的
	 * 窗口」，每块屏各渲染一遍、各裁到自己那块屏、各用自己那块屏的模糊
	 * 纹理（m_blurFB 每屏一张）。两块屏各画一半，拼起来看着像跨屏。
	 *
	 * Vidow 的窗口装饰是离屏烘的一张图，图上烘死了采样坐标和倍率，所以
	 * 「每屏一遍」落地成「每块屏各烘一份自己的装饰缓冲」（见
	 * struct vidow_blur_slot 的 dec*）。只在窗口真的骑在两块屏中间时才
	 * 多烘一份。 */
	view_bake_for_output(server, output);
}

/* **这块屏把自己那一半画一遍**（照 Hyprland 的 renderMonitor 逐屏循环）。
 *
 * 只在窗口真的骑在两块屏中间时才多烘一份：窗口完全落在某块屏里的时候，
 * 那一趟已经在 vidow_view_anim_bake 里烘过了（bake_output 就是它）。
 *
 * 这里做的只是「换一块屏、同一个窗口、再烘一遍」——几何、倍率、底衬全都取
 * 那一块屏自己的（ensure_targets 按 bake_output 选缓冲，decor_draw 按
 * bake_output 取底衬槽），所以两半各自按所在屏的缩放渲染。
 *
 * 显示的裁剪交给场景：装饰节点的 clip（fx->clip，见 view_clip_box）已经
 * 收紧到工作区 / 屏，超出去的那块根本不画。两块屏各显示自己那一半。 */
static void view_bake_for_output(struct vidow_server *server,
		struct vidow_output *output) {
	struct vidow_view *view;
	/* 这块屏的布局框只跟 output 有关，提在循环外算一次：放在循环体内就是每个
	 * 窗口每帧一次 wlr_output_layout_get_box（输出布局链表遍历）。 */
	struct wlr_box ob = {0};
	wlr_output_layout_get_box(server->output_layout,
		output->wlr_output, &ob);
	wl_list_for_each(view, &server->views, link) {
		if (!view->mapped || view->popup || view->scene_tree == NULL ||
				!view->fx.active || view->fx.node == NULL) {
			continue;
		}
		/* **overview 里没有「跨屏的另一半」。**
		 *
		 * 全景把每一行画成一块缩小版的屏，行本身就在窗口自己那块屏里；
		 * 一份装饰缓冲就够了。更不能拿下面那个 vb（**布局坐标**的 geom）
		 * 去判「跟这块屏相不相交」：overview 里节点画的是**行坐标**
		 * （vidow_overview_map：行的 content_x/y + 局部 × scale），和布局
		 * 里的 geom 差着视口 / 缩放。窗口在屏幕上明明还露着半块，vb 却落在
		 * 屏外 —— 这一趟就把槽位节点关掉，窗口整个消失（用户报的
		 * 「overview 的窗口超出一点显示器就消失」）。
		 *
		 * 归属那块屏的那一份由 vidow_view_apply_anim 每帧烘好、摆好
		 * （overview_apply 对每个窗口都调一次），这里只把**别的屏上残留**
		 * 的槽位收掉：进 overview 之前跨屏拖过的话，对面那格还停在老位置，
		 * 不收就是一块残影。归属不明的（owner == NULL）别动 —— 收错了就是
		 * 整块不画。 */
		if (vidow_overview_active(server)) {
			struct wlr_output *own = view_blur_owner(view);
			struct vidow_blur_slot *sl = blur_slot_for(&view->fx,
				output->wlr_output, false);
			if (own != NULL && own != output->wlr_output &&
					sl != NULL && sl->node != NULL) {
				wlr_scene_node_set_enabled(&sl->node->node, false);
			}
			continue;
		}
		/* Output instances also need updating after release and during layout
		 * animations; otherwise the non-owner half retains an old buffer. */
		struct wlr_box vb = vidow_view_screen_box(view);
		/* **先问「它现在到底看得见吗」，再问「跟这块屏相交吗」。**
		 *
		 * 平铺窗口的 geom 里含着滚动视口的偏移，而那是**布局绝对坐标**：
		 * DP-2 上两列宽 1909、视口滚到第二列时，第一列的
		 * geom.x = 1920 + 0 - 1901 = 19 —— 它在 DP-2 左侧屏外（被
		 * view_clip_box 裁光了），可那个坐标**落在 DP-3 的范围里**
		 * （0..1920）。直接拿 vb 跟这块屏的框做交集，就会判成「这个窗口
		 * 骑在 DP-3 上」，每帧给它白烘一份 DP-3 的装饰 —— 实测静止 6 秒
		 * 3459 次 bakeout，view=7 明明在 DP-2 上却有 869 次 out=DP-3，
		 * 而且 rev 永远不命中缓存（DP-2 那趟刚烘完，DP-3 这趟又把
		 * dec_screen_box 改掉，下一帧又反过来）。
		 *
		 * fx->clip 就是 view_clip_box 的结果（见 view.c 的
		 * vidow_view_apply_clip）：滚出视口的窗口裁完是空的，跟它一比
		 * 就现原形。裁剪关着（浮窗 / 正在拖）时才退回按 vb 判。 */
		if (view->fx.clip_on) {
			struct wlr_box visible = {0};
			if (!wlr_box_intersection(&visible, &vb, &view->fx.clip)) {
				struct vidow_blur_slot *sl = blur_slot_for(&view->fx,
					output->wlr_output, false);
				if (sl != NULL && sl->node != NULL)
					wlr_scene_node_set_enabled(&sl->node->node, false);
				continue;
			}
			vb = visible;
		}
		struct wlr_box hit;
		if (!wlr_box_intersection(&hit, &vb, &ob)) {
			struct vidow_blur_slot *sl = blur_slot_for(&view->fx,
				output->wlr_output, false);
			if (sl != NULL && sl->node != NULL)
				wlr_scene_node_set_enabled(&sl->node->node, false);
			continue;
		}
		/* An output frame is not a content update. Reuse its decoration until
		 * a commit/placement/effect update, backdrop change or output change.
		 * This also avoids rebaking the owner immediately after apply_anim. */
		struct vidow_blur_slot *sl = blur_slot_for(&view->fx,
			output->wlr_output, false);
		if (sl != NULL && sl->dec_valid && sl->node != NULL &&
				sl->node->node.enabled &&
				sl->dec_revision == view->fx.bake_revision &&
				sl->dec_blur_ver == view->fx.blur_ver &&
				sl->dec_output_scale == output->wlr_output->scale &&
				sl->dec_output_box.x == ob.x && sl->dec_output_box.y == ob.y &&
				sl->dec_output_box.width == ob.width &&
				sl->dec_output_box.height == ob.height) {
			if (vidow_xtrace_on()) {
				wlr_log(WLR_INFO, "bake: %s 缓存命中跳过重烘 "
					"rev=%u/%u blur=%u/%u",
					vidow_view_title(view) != NULL ?
						vidow_view_title(view) : "(无题)",
					sl->dec_revision, view->fx.bake_revision,
					sl->dec_blur_ver, view->fx.blur_ver);
			}
			continue;
		}
		if (vidow_xtrace_on()) {
			wlr_log(WLR_INFO, "bake: %s 要重烘 dec_valid=%d en=%d "
				"rev=%u/%u blur=%u/%u",
				vidow_view_title(view) != NULL ?
					vidow_view_title(view) : "(无题)",
				sl != NULL ? sl->dec_valid : -1,
				sl != NULL && sl->node != NULL ?
					sl->node->node.enabled : -1,
				sl != NULL ? sl->dec_revision : 0,
				view->fx.bake_revision,
				sl != NULL ? sl->dec_blur_ver : 0, view->fx.blur_ver);
		}
		/* **烘完把 bake_output 还回去。**
		 *
		 * 这一趟是「这块屏借这个窗口再烘一份自己的那一半」，是临时的。
		 * 可 bake_output 是单值，vidow_view_anim_bake 下一帧会拿它当
		 * 「这个窗口该按哪块屏的倍率烘」—— 留着不回，窗口就永久按最后
		 * 那趟借它的屏来烘了。
		 *
		 * 实测症状（用户 2026-10-27）：窗口在 2x 的 DP-2 上，第一次往
		 * 1x 的 DP-3 拖，跨屏那一帧 DP-3 把 bake_output 抢过去，之后
		 * 每帧都按 DP-3 的 1x 烘 —— 窗口卡在 DP-2 左边缘，拖得越远背景
		 * 横条越多，超过一个窗口宽才突然跳过去；之后再拖就正常了（那时
		 * bake_output 已经稳定在 DP-3）。 */
		struct wlr_output *saved_bake_output = view->fx.bake_output;
		view->fx.bake_output = output->wlr_output;
		/* 跟手探针：跨屏那一半到底有没有被对面那块屏烘一次（一行 = 一次重烘）。 */
		if (vidow_xtrace_on() &&
				(view->dragging || server->grab_view == view)) {
			vidow_gtrace("bake v=%d out=%s vb=%d,%d %dx%d vm=%d,%d -> bake_scale=%.2f",
				view->id, output->wlr_output->name, vb.x, vb.y,
				vb.width, vb.height,
				(int)lround(view->anim_off_x), (int)lround(view->anim_off_y),
				output->wlr_output->scale);
		}
		if (vidow_xtrace_on()) {
			wlr_log(WLR_INFO, "bakeout: view=%d out=%s vb=%d,%d %dx%d "
				"cached[rev=%u blur=%u scale=%.2f box=%d,%d %dx%d] "
				"now[rev=%u blur=%u scale=%.2f box=%d,%d %dx%d]",
				view->id, output->wlr_output->name, vb.x, vb.y,
				vb.width, vb.height,
				sl != NULL ? sl->dec_revision : 0,
				sl != NULL ? sl->dec_blur_ver : 0,
				sl != NULL ? sl->dec_output_scale : 0.0,
				sl != NULL ? sl->dec_output_box.x : 0,
				sl != NULL ? sl->dec_output_box.y : 0,
				sl != NULL ? sl->dec_output_box.width : 0,
				sl != NULL ? sl->dec_output_box.height : 0,
				view->fx.bake_revision, view->fx.blur_ver,
				output->wlr_output->scale,
				ob.x, ob.y, ob.width, ob.height);
		}
		vidow_view_anim_bake(view);
		view->fx.bake_output = saved_bake_output;
	}
}

/* ------------------------------------------------------------------- draw */

/* 装饰那一趟的几何（全部是**渲染目标像素**口径）。屏幕那条路（decor_draw）和
 * 窗口抓帧那条路（vidow_fx_capture_decor）只有这一份不一样，uniform 那几十行
 * 共用同一段代码 —— 两处各写一遍必然走散，某处忘了乘 scale 就是「录出来跟屏上
 * 长得不一样」。
 *
 * off_x/off_y：内容（窗口可见区）左上角在渲染目标里的位置。屏幕那条路是内边距
 * px_pad（内容居中），窗口抓帧那条路是窗口在取景框里的位置（抓帧框常常就是窗口
 * 本身，于是是 0）。
 * origin_x / origin_y 与 out_size_x / out_size_y：内容左上角在**底衬贴图**里的
 * 像素坐标，以及那张贴图的尺寸。
 * 装饰着色器按它算模糊采样的 UV（见 decor_fragment_src 的 u_origin /
 * u_output_size）。 */
struct decor_geo {
	int dst_w, dst_h;
	int content_w, content_h;
	float off_x, off_y;
	float origin_x, origin_y;
	float out_size_x, out_size_y;
	/* 内容那一项的乘子：屏幕那条路是 1，窗口抓帧那条路是 0（内容由场景节点
	 * 画，这张图里只要底衬 / 画框，但剔除仍按内容 alpha 走）。 */
	float content_alpha;
};


/* 传输函数 → 着色器里的 0..4 小整数（见 fx-gl.c 的 decor_fragment_src）。
 * wlroots 的枚举是位标志（1/2/4/8/16），直接传进 GLSL 不好读，也容易和
 * 协议那边的枚举混。0 = 线性；2 = gamma2.2（wlroots 的默认）。 */
static float color_tf_code(enum wlr_color_transfer_function tf) {
	switch (tf) {
	case WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ:
		return 1.0f;
	case WLR_COLOR_TRANSFER_FUNCTION_BT1886:
		return 3.0f;
	case WLR_COLOR_TRANSFER_FUNCTION_SRGB:
		return 4.0f;
	case WLR_COLOR_TRANSFER_FUNCTION_EXT_LINEAR:
		return 0.0f;
	case WLR_COLOR_TRANSFER_FUNCTION_GAMMA22:
	default:
		return 2.0f;
	}
}

/* 源色域 → 输出色域（sRGB）的 3x3 线性 RGB 矩阵（行主序；上传时 GL_TRUE
 * 转置给 GLSL）。只处理 wlroots 已有的两个 named primaries —— 协议里还有
 * DCI-P3 / Display-P3 / Adobe RGB，但我们没把它们报出去（第 5 步的表）。 */
static void color_src_matrix(enum wlr_color_named_primaries prim, float m[9]) {
	for (int i = 0; i < 9; i++) {
		m[i] = i % 4 == 0 ? 1.0f : 0.0f;
	}
	if (prim == WLR_COLOR_NAMED_PRIMARIES_BT2020) {
		struct wlr_color_primaries src, dst;
		wlr_color_primaries_from_named(&src, WLR_COLOR_NAMED_PRIMARIES_BT2020);
		wlr_color_primaries_from_named(&dst, WLR_COLOR_NAMED_PRIMARIES_SRGB);
		wlr_color_primaries_transform_absolute_colorimetric(&src, &dst, m);
	}
}

static bool decor_pass(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		const struct decor_geo *g,
		struct wlr_texture *src, struct wlr_texture *mask,
		const float border[4], double alpha, double scale,
		const struct vidow_look *look, bool shadow_only) {
	struct vidow_decor_prog *d = &server->decor;
	struct vidow_config *cfg = &server->config;

	struct wlr_gles2_texture_attribs sa;
	wlr_gles2_texture_get_attribs(src, &sa);

	struct wlr_egl *egl = NULL;
	if (begin_pass(server, dst, g->dst_w, g->dst_h, &egl) == 0) {
		return false;
	}

	glUseProgram(d->program);
	glUniform1f(d->u_shadow_only, shadow_only ? 1.0f : 0.0f);
	bind_tex(0, sa.target, sa.tex);
	glUniform1i(d->u_texture, 0);

	struct wlr_texture *blurt = fx_blur_tex_now(fx);
	struct wlr_texture *blur = blurt != NULL ? blurt : server->empty_tex;
	if (blur != NULL) {
		struct wlr_gles2_texture_attribs ba;
		wlr_gles2_texture_get_attribs(blur, &ba);
		bind_tex(1, ba.target, ba.tex);
	}
	glUniform1i(d->u_blur, 1);

	/* 元素自己的内容（透明度剔除用）。mask == NULL 时用 u_texture 那张内容贴图
	 * （窗口 / 弹窗：内容贴图就是元素自己）；layer surface 的模糊底衬不画内容，
	 * 调用方把 layer 自己的缓冲当 mask 递进来。采样器不能空着，配上 empty_tex
	 * 也无所谓：u_mask_on = 0 时着色器根本不采它。 */
	struct wlr_texture *mtex = mask != NULL ? mask : server->empty_tex;
	struct wlr_gles2_texture_attribs ma;
	wlr_gles2_texture_get_attribs(mtex, &ma);
	bind_tex(2, ma.target, ma.tex);
	glUniform1i(d->u_mask, 2);
	glUniform1f(d->u_mask_on, mask != NULL ? 1.0f : 0.0f);
	/* 阈值：客户端用 ext-background-effect 自己要了区域就不按元素 alpha 剔 ——
	 * 那块区域就是它画出来的边界（niri 的 need_ignore_alpha 同一个判断）。layer
	 * 底衬拿不到元素内容时也不能剔，不然整块模糊都没了。0 = step(0, a) 恒为 1，
	 * 也就是不剔。 */
	float mask_min = fx->mask_min > 0.0f ?
		fx->mask_min : (float)cfg->blur_alpha_min;
	/* region_on 时「区域边界就是画出来的边界」，不再按元素 alpha 剔 —— **只对
	 * 没递 mask 的那条路成立**（窗口 / 弹窗：区域是客户端 ext-background-effect
	 * 声明的，精确）。
	 *
	 * layer 这条路递了 mask（面板自己的缓冲），而区域是**抓底衬的框**（比内容
	 * 大一圈：往外放了模糊半径、对齐到 128 网格，bar 是 949x438 里的 0,0 949x64
	 * 再放一圈 → 好几百像素高）。区域一放行就按整框糊，bar 底下会糊出一条
	 * 别的窗口的色带（用户 2026-10-31：「UI 带出别的窗口内容」）。所以这里必须
	 * 继续按 mask 的 alpha 剔。 */
	if (mask == NULL && (fx->blur_region_on || fx->blur_only)) {
		mask_min = 0.0f;
	}
	glUniform1f(d->u_mask_min, mask_min);
	/* mask 的采样映射：默认恒等；xdg popup 只糊窗口体时采表面缓冲里的
	 * geometry 子矩形（见 popup_blur_frame）。 */
	float mask_ox = fx->mask_scale_x > 0.0f ? fx->mask_off_x : 0.0f;
	float mask_oy = fx->mask_scale_y > 0.0f ? fx->mask_off_y : 0.0f;
	float mask_sx = fx->mask_scale_x > 0.0f ? fx->mask_scale_x : 1.0f;
	float mask_sy = fx->mask_scale_y > 0.0f ? fx->mask_scale_y : 1.0f;
	glUniform4f(d->u_mask_map, mask_ox, mask_oy, mask_sx, mask_sy);

	/* 着色器全程在「渲染目标的物理像素」里算：配置里的圆角、边框、阴影都是
	 * 逻辑单位，乘上倍率才是像素。 */
	glUniform2f(d->u_size, (float)g->content_w, (float)g->content_h);
	glUniform2f(d->u_target_size, (float)g->dst_w, (float)g->dst_h);
	glUniform2f(d->u_content_off, g->off_x, g->off_y);
	glUniform1f(d->u_radius, (float)(look->corner_radius * scale));
	/* 圆角的形状指数：2 = 圆弧、4 = 超椭圆。夹一道（配置 / 环境变量 / IPC /
	 * 窗口规则四条路都夹过，这里只是「进着色器的值一定是合法的」）。 */
	glUniform1f(d->u_corner_n, (float)vidow_corner_n_clamp(look->corner_n));
	glUniform1f(d->u_border_width, (float)(look->border_width * scale));
	/* The decoration shader works in premultiplied alpha; the config keeps
	 * straight values so a script can read them back. */
	float border_pm[4] = { border[0] * border[3], border[1] * border[3],
		border[2] * border[3], border[3] };
	float shadow_pm[4] = { look->shadow_color[0] * look->shadow_color[3],
		look->shadow_color[1] * look->shadow_color[3],
		look->shadow_color[2] * look->shadow_color[3],
		look->shadow_color[3] };
	glUniform4fv(d->u_border_color, 1, border_pm);
	glUniform4fv(d->u_shadow_color, 1, shadow_pm);
	glUniform2f(d->u_shadow_offset, (float)(look->shadow_offset_x * scale),
		(float)(look->shadow_offset_y * scale));
	glUniform1f(d->u_shadow_radius, (float)(look->shadow_radius * scale));
	glUniform1f(d->u_opacity, (float)look->opacity);
	glUniform1f(d->u_content_alpha, g->content_alpha);
	glUniform1f(d->u_noise, (float)look->noise);
	glUniform1f(d->u_alpha, (float)alpha);
	glUniform1f(glGetUniformLocation(d->program, "u_time"),
		(float)(vidow_now_ns() % 1000000000000LL) / 1e9f);
	/* 窗口在输出上的位置（物理像素，输出原点已减掉）：散景贴图是按输出分辨
	 * 率抓的，采样坐标必须落在这块屏自己的坐标系里。被模糊的 layer surface
	 * 那张贴图只盖住它自己那一小块区域（fx->blur_tex_*），坐标就再减掉区域
	 * 原点、除数也换成区域尺寸 —— 不减就是整块错位。
	 *
	 * 那块屏是哪一块，认**跟着这张贴图一起记的** blur_out_*（见 server.h）：
	 * 重烘会落在别的屏正在渲染的那一帧上（output.c 帧循环那趟不看归属），
	 * 现场读 server->output_* 拿到的是那块屏的尺寸/原点 —— 整屏底衬只取中间
	 * 一块铺满整窗，看着就是「静止时底衬啪一下换挡」（中心放大 + 变糊）。 */
	/* 底衬采样的映射：全部由调用方算好（屏幕那条路按窗口在输出上的位置算，
	 * 窗口抓帧那条路按取景框算）—— 两边共用这一份 uniform，别在这儿现场读
	 * 全局量（跨屏底衬采样的坑，见 server.h 的 blur_out_*）。 */
	/* 跨屏没有「分界 / 第二套坐标」：一次渲染只采这一块屏的底衬，另一半由
	 * 对面那块屏自己那份装饰缓冲画（见 vidow_fx_frame 的按屏裁剪）。 */
	glUniform2f(d->u_origin, g->origin_x, g->origin_y);
	glUniform2f(d->u_output_size, g->out_size_x, g->out_size_y);
	float bsx = fx->blur_scale_x > 0.0f ? fx->blur_scale_x : 1.0f;
	float bsy = fx->blur_scale_y > 0.0f ? fx->blur_scale_y : 1.0f;
	glUniform2f(d->u_blur_scale, bsx, bsy);
	glUniform2f(d->u_blur_bias, fx->blur_bias_x, fx->blur_bias_y);
	glUniform1f(d->u_flip_y, cfg->shader_flip_y ? 1.0f : 0.0f);
	/* 线性合成（color.linear_blend）：目标固定 gamma2.2（当前输出编码），
	 * 源按 fx 记的 TF / primaries。关着的时候着色器完全走老路径，这几个
	 * uniform 不影响任何像素。 */
	glUniform1f(d->u_linear, cfg->color_linear_blend ? 1.0f : 0.0f);
	glUniform1f(d->u_src_tf, color_tf_code(fx->src_tf));
	glUniform1f(d->u_dst_tf,
		color_tf_code(WLR_COLOR_TRANSFER_FUNCTION_GAMMA22));
	float src_m[9];
	color_src_matrix(fx->src_primaries, src_m);
	glUniformMatrix3fv(d->u_src_to_dst, 1, GL_TRUE, src_m);
	/* 模糊那层要不要叠：除了「有贴图」（fx->blur_tex）和全局开关（fx->blur），
	 * 还得看**这一次渲染自己的 look** —— 预览（缩略图）那一趟在
	 * vidow_fx_frame 里把 look.blur 关掉，于是预览里不叠模糊（见那里的注释）。 */
	float strength = fx->blur && blurt != NULL && look->blur ?
		(float)cfg->blur_strength : 0.0f;

	glUniform1f(d->u_blur_strength, strength);
	/* 客户端用 ext-background-effect 指定了区域就只模糊那块 */
	glUniform1f(d->u_blur_region_on, fx->blur_region_on ? 1.0f : 0.0f);
	/* 区域是 surface 局部坐标（逻辑），着色器在物理像素里比对。 */
	glUniform4f(d->u_blur_region, (float)(fx->blur_region[0] * scale),
		(float)(fx->blur_region[1] * scale),
		(float)(fx->blur_region[2] * scale),
		(float)(fx->blur_region[3] * scale));
	/* 内容贴图里哪一块是可见窗口（归一化）。没留白的元素是 0 / 1 —— layer 底衬、
	 * X11、以及不自己画装饰的客户端都走这一支，和以前逐像素一样。 */
	glUniform4f(d->u_tex_map, fx->tex_off_x, fx->tex_off_y,
		fx->tex_scale_x, fx->tex_scale_y);

	if (fx->blur_only) {
		/* 只要那一层模糊：内容、阴影、边框、圆角、噪点全关（layer surface 的
		 * 底衬用；内容由 wlroots 自己的场景节点画在上面）。 */
		glUniform1f(d->u_radius, 0.0f);
		glUniform1f(d->u_border_width, 0.0f);
		glUniform4f(d->u_border_color, 0.0f, 0.0f, 0.0f, 0.0f);
		glUniform4f(d->u_shadow_color, 0.0f, 0.0f, 0.0f, 0.0f);
		glUniform1f(d->u_noise, 0.0f);
	}

	draw_quad(d->program);
	glUseProgram(0);
	end_pass(egl);
	return true;
}
bool decor_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, struct wlr_texture *mask,
		const float border[4], int ox, int oy,
		double alpha, double scale, const struct vidow_look *look) {
	/* 窗口在输出上的位置（物理像素，输出原点已减掉）：散景贴图是按输出分辨
	 * 率抓的，采样坐标必须落在这块屏自己的坐标系里。被模糊的 layer surface
	 * 那张贴图只盖住它自己那一小块区域（fx->blur_tex_*），坐标就再减掉区域
	 * 原点、除数也换成区域尺寸 —— 不减就是整块错位。
	 *
	 * 那块屏是哪一块，认**跟着这张贴图一起记的** blur_out_*（见 server.h）：
	 * 重烘会落在别的屏正在渲染的那一帧上（output.c 帧循环那趟不看归属），
	 * 现场读 server->output_* 拿到的是那块屏的尺寸/原点 —— 整屏底衬只取中间
	 * 一块铺满整窗，看着就是「静止时底衬啪一下换挡」（中心放大 + 变糊）。 */
	/* **这块窗口在这一块屏上**用的那一份底衬（见 server.h 的 struct
	 * vidow_blur_slot）：窗口骑在两块屏中间时，两块屏各有一格，各自记着自己
	 * 那份贴图的原点 / 区域 / 视野。取错格的后果就是「一半正常、一半横条」。 */
	int out_x = fx->blur_out_w > 0 ? fx->blur_out_x : server->output_x;
	int out_y = fx->blur_out_h > 0 ? fx->blur_out_y : server->output_y;
	int out_w = fx->blur_out_w > 0 ? fx->blur_out_w : server->output_w;
	int out_h = fx->blur_out_h > 0 ? fx->blur_out_h : server->output_h;
	int rg_x = fx->blur_tex_x, rg_y = fx->blur_tex_y;
	int rg_w = fx->blur_tex_w, rg_h = fx->blur_tex_h;
	struct vidow_blur_slot *bs = fx->cur_slot;
	if (bs != NULL) {
		out_x = bs->ox; out_y = bs->oy;
		out_w = bs->ow; out_h = bs->oh;
		rg_x = bs->rx; rg_y = bs->ry;
		rg_w = bs->rw; rg_h = bs->rh;
	}

	/* 尺寸取**这一帧那一格**（跨屏时两块屏各一对，尺寸差一倍）。 */
	int dw2 = fx->cur_slot != NULL ? fx->cur_slot->dec_w : fx->w;
	int dh2 = fx->cur_slot != NULL ? fx->cur_slot->dec_h : fx->h;
	int dcw2 = fx->cur_slot != NULL ? fx->cur_slot->dec_content_w : fx->content_w;
	int dch2 = fx->cur_slot != NULL ? fx->cur_slot->dec_content_h : fx->content_h;
	int dpad2 = fx->cur_slot != NULL ? fx->cur_slot->dec_px_pad : fx->px_pad;
	if (span_trace_on() && bs != NULL) {
		wlr_log(WLR_INFO, "span: out=%s scale=%.2f target=%dx%d content=%dx%d origin=%d,%d viewport=%d,%d %dx%d region=%d,%d %dx%d tex=%p",
			bs->out->name, scale, dw2, dh2, dcw2, dch2, ox, oy,
			out_x, out_y, out_w, out_h, rg_x, rg_y, rg_w, rg_h, (void *)bs->tex);
	}
	struct decor_geo g = {
		.dst_w = dw2, .dst_h = dh2,
		.content_w = dcw2, .content_h = dch2,
		.off_x = (float)dpad2, .off_y = (float)dpad2,
		.origin_x = (float)((int)lround((double)ox * scale) - out_x - rg_x),
		.origin_y = (float)((int)lround((double)oy * scale) - out_y - rg_y),
		.out_size_x = (float)(rg_w > 0 ? rg_w : out_w),
		/* 判据要看**自己那个轴**的 w：写成 blur_tex_w 查 blur_tex_h 的话，
		 * 只要两者不同时为 0 就看不出来，一旦某帧只设了其中一个，
		 * V 方向的除数就成了 0 —— UV 恒定，「全是横条」。 */
		.out_size_y = (float)(rg_h > 0 ? rg_h : out_h),
		.content_alpha = 1.0f,
	};

	/* **跨屏两半各自采自己那块屏的底衬 —— 但不是在这一次渲染里。**
	 *
	 * 这里曾经按「另一块屏的布局左边界」算一个分界，越过它就把 origin /
	 * output_size 换成另一块屏那套。那套只有在**一张装饰图能同时拿到两块屏
	 * 的底衬贴图**时才成立，而装饰着色器一次只绑一张 u_blur —— 换了坐标换
	 * 不了贴图，右半边只是拿这一张贴图按另一块屏的原点 / 尺寸去采，UV 直接
	 * 飞出区域，clamp 到边缘像素（「跨屏那一半是横条 / 整块糊」的来源）。
	 *
	 * 现在跨屏是**每块屏各烘一份装饰缓冲**（view_bake_for_output + 每屏一个
	 * vidow_blur_slot，各自带自己那块屏的私有模糊贴图），每份只画自己那块屏
	 * 那一半（见 vidow_fx_frame 里按 instance->layout_box 的裁剪）。一次渲染
	 * 只认一张底衬，采样坐标也就只需要这一组。
	 *
	 * 坐标口径：
	 *   p          = 目标像素，原点在装饰缓冲左上角
	 *   p.x        = off_x + (窗口内容左边 - ox) * scale   （off_x = px_pad）
	 *   采样       = (origin - off + p * blur_scale + bias) / out_size
	 */

	return decor_pass(server, fx, dst, &g, src, mask, border, alpha, scale, look,
		false);
}

/* 窗口抓帧那条路（wincapture.c）：把「窗口内容」按屏上的样子合成出来 —— 模糊底衬
 * + 圆角 + 边框 + 投影 + 颗粒，和屏幕上**同一个着色器、同一遍**（不是另写一套）。
 *
 * 和屏幕那条路的差别只有几何：
 *   - 渲染目标是**取景框**（这一帧的流），不是窗口的离屏缓冲，内容摆在哪由调用方
 *     给（content_x/y）；
 *   - 底衬采样按「内容左上角的布局坐标 × 取景框缩放」算：取景框的缩放常常和那块
 *     屏的缩放不是一回事（窗口等比缩进流里），所以先按布局坐标换算到那块屏的设备
 *     像素，再乘目标像素/底衬像素那个比值（k）。取景框就是窗口本身时 k = 1，
 *     公式跟屏幕那条路逐项一致。
 *
 * 取景框比窗口大（窗口缩进流、四周留黑）时，装饰只画在窗口那块；外面的留黑由
 * 调用方保证（这一趟不动那儿的像素）。 */
/* 客户端缓冲 → 贴图（借来那张或自己建一张）。窗口抓帧那条路要拿它当「元素
 * 内容」贴图（采样映射 + 透明度剔除），和屏幕上那条路共用这一份。owned 非空时
 * 告诉调用方要不要自己销毁。 */
struct wlr_texture *vidow_fx_client_texture(struct vidow_server *server,
		struct wlr_buffer *buffer, struct wlr_scene_buffer *scene_buffer,
		bool *owned) {
	if (server == NULL || buffer == NULL) {
		return NULL;
	}
	if (scene_buffer != NULL && !client_buffer_wait(server, scene_buffer)) {
		return NULL;
	}
	bool own = true;
	struct wlr_texture *tex = client_texture(server->renderer, buffer, &own);
	if (owned != NULL) {
		*owned = own;
	}
	return tex;
}

bool vidow_fx_capture_decor(struct vidow_server *server,
		const struct vidow_capture_decor *c) {
	if (server == NULL || c == NULL || c->dst == NULL || c->look == NULL ||
			c->dst_w <= 0 || c->dst_h <= 0 || server->decor.program == 0) {
		return false;
	}
	double frame_scale = c->frame_scale > 0.0 ? c->frame_scale : 1.0;
	double out_scale = c->out_scale > 0.0 ? c->out_scale : frame_scale;
	/* 目标像素 / 底衬像素。两者一样时（窗口源那条路就是这样：出图尺寸 =
	 * 窗口逻辑尺寸 × 那块屏的缩放）下面这两行就是屏幕那条路的公式。 */
	double k = frame_scale / out_scale;
	int bw = c->blur_w > 0 ? c->blur_w : c->out_w;
	int bh = c->blur_h > 0 ? c->blur_h : c->out_h;
	if (bw <= 0 || bh <= 0) {
		bw = 1;
		bh = 1;
	}

	/* fx 只用来告诉 decor_pass「底衬是哪张、内容贴图怎么采」：几何全在 g 里。
	 * 模糊只有「底衬那一趟」画（edge 那趟画的是画框，叠在内容上面，再糊一次
	 * 就是错的）。 */
	struct vidow_fx_target fx = {0};
	fx.blur_tex = c->blur;
	fx.blur = !c->edge && c->blur != NULL;
	fx.tex_off_x = c->tex_u0;
	fx.tex_off_y = c->tex_v0;
	fx.tex_scale_x = c->tex_du;
	fx.tex_scale_y = c->tex_dv;

	struct decor_geo g = {
		.dst_w = c->dst_w, .dst_h = c->dst_h,
		.content_w = c->content_w, .content_h = c->content_h,
		.off_x = (float)c->content_x, .off_y = (float)c->content_y,
		.origin_x = (float)(((double)c->content_layout_x - (double)c->out_x) *
			frame_scale - (double)c->blur_x * k),
		.origin_y = (float)(((double)c->content_layout_y - (double)c->out_y) *
			frame_scale - (double)c->blur_y * k),
		.out_size_x = (float)((double)bw * k),
		.out_size_y = (float)((double)bh * k),
		/* 内容不画进这张图（由场景里那个镜像节点画），但 u_texture 照样得
		 * 采 —— 透明度剔除按它的 alpha 走（和屏上同一条判据）。 */
		.content_alpha = 0.0f,
	};

	return decor_pass(server, &fx, c->dst, &g,
		c->content != NULL ? c->content : server->empty_tex, NULL,
		c->border, 1.0, frame_scale, c->look, false);
}

static bool anim_draw(struct vidow_server *server,
		struct vidow_fx_target *fx, struct wlr_buffer *dst,
		struct wlr_texture *src, int ox, int oy, double alpha, double scale) {
	struct vidow_shader *sh = fx->shader;
	struct wlr_gles2_texture_attribs sa;
	wlr_gles2_texture_get_attribs(src, &sa);

	struct wlr_egl *egl = NULL;
	if (begin_pass(server, dst, fx->w, fx->h, &egl) == 0) {
		return false;
	}

	glUseProgram(sh->program);
	bind_tex(0, sa.target, sa.tex);
	glUniform1i(sh->u_texture, 0);
	/* 过冲的曲线（out_elastic / out_back）让 t 稍微越过 1：几何量过冲是我
	 * 们要的，着色器的 u_progress 不是 —— 它的契约是 0..1，> 1 的进度会让
	 * zoom 这类 shader 采到贴图外面去。这里夹住。 */
	double progress = fx->progress;
	if (!(progress > 0.0)) {
		progress = 0.0;
	} else if (progress > 1.0) {
		progress = 1.0;
	}
	glUniform1f(sh->u_progress, (float)progress);
	glUniform1i(sh->u_phase, (int)fx->phase);
	glUniform2f(sh->u_size, (float)fx->w, (float)fx->h);
	glUniform2f(glGetUniformLocation(sh->program, "u_target_size"),
		(float)fx->w, (float)fx->h);
	glUniform2f(sh->u_origin,
		(float)((int)lround((double)ox * scale) - server->output_x),
		(float)((int)lround((double)oy * scale) - server->output_y));
	glUniform2f(sh->u_output_size, (float)server->output_w,
		(float)server->output_h);
	glUniform1f(sh->u_alpha, (float)alpha);
	glUniform1f(sh->u_time, (float)(vidow_now_ns() - fx->start_ns) / 1e9f);
	glUniform1f(sh->u_flip_y, server->config.shader_flip_y ? 1.0f : 0.0f);

	draw_quad(sh->program);
	glUseProgram(0);
	end_pass(egl);
	return true;
}

/* -------------------------------------------------------------- debugging */

/* ------------------------------------------------------------- public API */

bool vidow_fx_set_shader(struct vidow_server *server,
		struct vidow_fx_target *fx, const char *name,
		enum vidow_shader_phase phase) {
	struct vidow_shader *sh = vidow_shader_get(server, name);
	if (sh == NULL) {
		return false;
	}
	fx->shader = sh;
	fx->phase = phase;
	fx->progress = 0.0;
	fx->start_ns = vidow_now_ns();
	return true;
}

/* 装饰画面是一个普通的 scene buffer，而普通 buffer 默认「接受输入」。它盖在
 * 客户端自己的 surface 节点上面，于是会把所有点击都吃掉 —— 窗口就点不着了。
 * 返回 false 让场景的命中测试继续往下走，落到客户端表面上。 */
bool fx_node_rejects_input(struct wlr_scene_buffer *buffer, double *sx,
		double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

bool vidow_fx_begin(struct vidow_server *server, struct vidow_fx_target *fx,
		struct wlr_scene_tree *parent, const char *shader_name) {
	bool decorated = server->config.decor_enabled && server->decor.program != 0;
	bool animated = shader_name != NULL && shader_name[0] != '\0';
	if (!decorated && !animated) {
		return false;
	}
	if (fx->node == NULL) {
		fx->node = wlr_scene_buffer_create(parent, NULL);
		if (fx->node == NULL) {
			return false;
		}
		fx->node->point_accepts_input = fx_node_rejects_input;
	}
	/* X11 withdraw/reassociate creates a new scene tree while its FX nodes
	 * can still belong to the disabled old tree. Move every output instance
	 * with the owner, not just the legacy node, before hiding the real surface. */
	if (fx->node->node.parent != parent) {
		wlr_scene_node_reparent(&fx->node->node, parent);
		for (struct vidow_blur_slot *sl = fx->blur_slot; sl; sl = sl->next) {
			if (sl->node != NULL) {
				wlr_scene_node_reparent(&sl->node->node, parent);
			}
			sl->dec_valid = false;
		}
	}
	wlr_scene_node_raise_to_top(&fx->node->node);
	wlr_scene_node_set_enabled(&fx->node->node, true);
	fx->active = true;
	fx->blur = server->config.blur_enabled;
	/* 默认恒等：内容贴图就是整块元素。窗口那一路每帧按窗口几何覆盖它。 */
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;
	/* mask 默认恒等：没有几何偏移。 */
	fx->mask_off_x = 0.0f;
	fx->mask_off_y = 0.0f;
	fx->mask_scale_x = 1.0f;
	fx->mask_scale_y = 1.0f;
	fx->mask_min = 0.0f;
	fx->anim_scale = 1.0;
	if (animated) {
		vidow_fx_set_shader(server, fx, shader_name, VIDOW_SHADER_OPEN);
	}
	return true;
}
