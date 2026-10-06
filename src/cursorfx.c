/* 摇晃找光标：快速来回晃几下指针，指针就地放大 —— **晃一下大一圈，接着晃接着大**
 * （晃到找着为止），停下来 700ms 自己**动画**缩回去。
 *
 * 一圈多大：**加**一个固定倍数，不是乘（`cursor_shake_scale = 2` 是「第一次
 * 到大一倍，之后每次再 +1」：2x → 3x → 4x → 5x …）。乘的话是 2、4、8、16 一路
 * 炸开，第一下就大得吓人、再晃一下就顶到屏幕。放大和缩小本身都按**线性**补间
 * 过去（150ms / 200ms），不跳。
 *
 * 放大是**换一张更大的光标图**：把当前光标图放大重画进一张新 buffer，再交给
 * wlr_cursor_set_buffer()。于是它和普通光标走同一条路：
 *
 *   - 硬件光标平面放得下（DRM 会报上限，比如 256x256）就还是硬件光标：不进合成器
 *     的渲染通道，因此**不会出现在截图 / 录屏里**。
 *   - 放不下 wlroots 会自己降级成软光标 —— 软光标是画进画面的，就会进截图。所以
 *     抓帧时**让一下位**：那一帧临时换回原尺寸那张（它放得下硬件平面），画面上照样
 *     有光标，而抓到的帧里一个光标像素都没有。截图应用**显式要光标**（screencopy 的
 *     overlay_cursor，`grim -c` 就是它）时才连放大的一起给。
 *   - 倍数不设上限：晃一次乘一次，到屏幕那么大为止（比屏幕还大的光标没意义）。
 *
 * 放大图一律从**原尺寸那张**算起（放大开始时自己 1:1 复制一份），不是叠着放大 ——
 * 不然晃几下就糊成一团。原尺寸那张同时是抓帧让位时的替身。
 *
 * 放大期间不动 wlr_cursor 里记的「当前光标图」（客户端 surface / 主题 xcursor），
 * 所以到点能原样还原；这期间客户端换光标（request_set_cursor）就立刻收摊让位。 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/util/log.h>

#include "json.h"
#include "server.h"

/* 摇晃判定：SHAKE_WINDOW_MS 里至少换向 SHAKE_FLIPS 次，每次换向前至少走
 * SHAKE_MIN_TRAVEL 个逻辑像素 —— 慢慢画圈不算，来回抖才算。 */
#define SHAKE_WINDOW_MS 700
#define SHAKE_MIN_TRAVEL 12.0
#define SHAKE_FLIPS 3
/* 放大后保持多久（不晃了就缩回去）。 */
#define SHAKE_HOLD_MS 700
/* 后端没报硬件光标尺寸上限时的天花板：只有嵌套 Wayland / headless / X11 会走到
 * 这里 —— 它们把光标交给上层合成器画，没有硬性的平面尺寸。真后端（DRM）会报
 * 上限，那就按它夹。 */
#define SHAKE_SOFT_MAX 256
/* 每一圈至少加这么多（配置值 = 加多少 + 1），免得配个 1.01 晃半天看不出来。 */
#define SHAKE_MIN_STEP 1.2
/* 尺寸补间：放大 150ms、缩小 200ms，线性；每 SHAKE_TICK_MS 一帧重画一次光标图
 * （光标图不像场景，没有每帧回调可用，动画得自己拿定时器推）。 */
#define SHAKE_GROW_MS   150
#define SHAKE_SHRINK_MS 200
#define SHAKE_TICK_MS   16
/* 倍数小于这个就算「没放大」，该还原真图了。 */
#define SHAKE_UNIT_EPS  1e-3

struct vidow_cursor_fx {
	/* 摇晃判定 */
	double last_x, last_y;
	bool have_last;
	int dir;        /* 上一次的主方向：-1 / 0 / 1 */
	double acc;     /* 当前方向累计走了多远 */
	int flips;      /* 窗口内换了几次向 */
	int64_t window_ns;

	/* 放大中 */
	bool active;
	int64_t until_ns;
	double factor;       /* 当前**画在屏幕上**的倍数（补间中间值） */
	double want;         /* 目标倍数（每晃一次加一个固定量，被屏幕尺寸夹住） */
	double anim_from;    /* 这一段补间的起点倍数 */
	int64_t anim_start_ns;
	int anim_ms;         /* 这一段补间多长 */
	bool animating;
	double scale;        /* 光标所在输出的缩放 */
	bool hardware;       /* 放大图在不在硬件光标平面上（只影响日志 / 诊断） */
	struct wlr_buffer *src; /* 原尺寸那张（1:1，自己画的） */
	struct wlr_buffer *big; /* 放大那张 */
	int32_t big_hx, big_hy; /* 放大图的热点（逻辑坐标） */
	int32_t src_hx, src_hy; /* 原尺寸那张的热点（逻辑坐标） */
	int limit_w, limit_h;   /* 倍数上限：放大图的物理像素不超过这块屏 */
	struct wl_event_source *timer;

	/* 当前光标图（还原用）：客户端 surface 或主题 xcursor（surface == NULL） */
	struct wlr_surface *surface;
	int32_t hx, hy;
	struct wl_listener surface_destroy;
};

/* 前置声明：定时器（动画的推手）在上面，重画光标图那两段在下面。 */
static bool shake_draw_at(struct vidow_server *server, double f,
		const char **why);

/* --------------------------------------------------------------- 取状态 */

/* 指针底下那块屏上的光标对象（wlr_cursor 每块输出一个）。 */
static struct wlr_output_cursor *cursor_under_pointer(
		struct vidow_server *server, struct wlr_output **out_output) {
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (out_output != NULL) {
		*out_output = output;
	}
	if (output == NULL) {
		return NULL;
	}
	struct wlr_output_cursor *oc;
	wl_list_for_each(oc, &output->cursors, link) {
		if (oc->enabled && oc->visible && oc->texture != NULL) {
			return oc;
		}
	}
	return NULL;
}

/* 硬件光标能画多大。没有硬件光标（嵌套 / headless / X11 后端）就返回一个够宽松的
 * 天花板，反正那种情况下光标是交给上层合成器画的。 */
static void cursor_size_limit(struct wlr_output *output, int *w, int *h) {
	*w = SHAKE_SOFT_MAX;
	*h = SHAKE_SOFT_MAX;
	if (output->impl == NULL || output->impl->set_cursor == NULL) {
		return; /* 没有光标平面：本来就是软光标，没什么可夹的 */
	}
	if (output->impl->get_cursor_sizes == NULL) {
		return; /* 后端没报尺寸：按 SHAKE_SOFT_MAX 宽松着来 */
	}
	size_t n = 0;
	const struct wlr_output_cursor_size *sizes =
		output->impl->get_cursor_sizes(output, &n);
	int bw = 0, bh = 0;
	for (size_t i = 0; i < n; i++) {
		if ((long)sizes[i].width * sizes[i].height > (long)bw * bh) {
			bw = sizes[i].width;
			bh = sizes[i].height;
		}
	}
	if (bw > 0 && bh > 0) {
		*w = bw;
		*h = bh;
	}
}

/* --------------------------------------------------------------- 画放大图 */

/* 把一张贴图重画成 w x h 的新 buffer（带 alpha，光标图不能留底色）。 */
static struct wlr_buffer *scale_texture(struct vidow_server *server,
		struct wlr_texture *tex, const struct wlr_fbox *src_box, int w, int h) {
	struct wlr_buffer *buf = vidow_alloc_target(server, w, h);
	if (buf == NULL) {
		return NULL;
	}
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server->renderer, buf, NULL);
	if (pass == NULL) {
		wlr_buffer_drop(buf);
		return NULL;
	}
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = w, .height = h },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = tex,
		.src_box = *src_box,
		.dst_box = { .width = w, .height = h },
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	});
	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_drop(buf);
		return NULL;
	}
	return buf;
}

/* --------------------------------------------------------------- 还原 */

static void drop_images(struct vidow_cursor_fx *fx) {
	if (fx->big != NULL) {
		wlr_buffer_drop(fx->big);
		fx->big = NULL;
	}
	if (fx->src != NULL) {
		wlr_buffer_drop(fx->src);
		fx->src = NULL;
	}
}

static void shake_restore(struct vidow_server *server) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	if (!fx->active) {
		return;
	}
	fx->active = false;
	fx->animating = false;
	wl_event_source_timer_update(fx->timer, 0);
	if (fx->surface != NULL) {
		wlr_cursor_set_surface(server->cursor, fx->surface, fx->hx, fx->hy);
	} else {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
	}
	drop_images(fx);
	wlr_log(WLR_DEBUG, "摇晃: 光标还原（%.2fx 收起）", fx->factor);
	fx->factor = 1.0;
	fx->want = 1.0;
}

/* 开始一段尺寸补间：从当前倍数线性走到 want，ms 毫秒走完。 */
static void shake_animate(struct vidow_server *server, double want, int ms) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	fx->anim_from = fx->factor;
	fx->want = want;
	fx->anim_ms = ms > 0 ? ms : 1;
	fx->anim_start_ns = vidow_now_ns();
	fx->animating = true;
	wl_event_source_timer_update(fx->timer, SHAKE_TICK_MS);
}

static int shake_timer_cb(void *data) {
	struct vidow_server *server = data;
	struct vidow_cursor_fx *fx = server->cursor_fx;
	int64_t now = vidow_now_ns();

	if (fx->animating) {
		double t = (double)(now - fx->anim_start_ns) /
			((double)fx->anim_ms * 1e6);
		if (t >= 1.0) {
			t = 1.0;
			fx->animating = false;
		}
		double f = fx->anim_from + (fx->want - fx->anim_from) * t;
		if (f < 1.0) {
			f = 1.0;
		}
		/* 缩回 1.0 的那一段走完就直接还原真图（f = 1.0 那张就是真图，
		 * 换回去看不出接缝）。 */
		if (!fx->animating && fx->want <= 1.0 + SHAKE_UNIT_EPS) {
			shake_restore(server);
			return 0;
		}
		const char *why = NULL;
		if (shake_draw_at(server, f, &why)) {
			if (!fx->animating) {
				/* 补间走完了：这一刻的尺寸是最终尺寸，日志（含「硬件光标
				 * / 软光标」那条判据）在这儿记一次就够，别每帧刷屏。 */
				wlr_log(WLR_INFO,
					"摇晃: 光标放大 %.2fx（%dx%d → %dx%d，%s）", f,
					fx->src->width, fx->src->height,
					fx->big->width, fx->big->height,
					fx->hardware ? "硬件光标，不进截图"
						: "软光标：会画进屏幕（抓帧是 vidow 自己渲染的，截图里照样没有它）");
			}
		} else {
			wlr_log(WLR_DEBUG, "摇晃: 这一帧的光标图没画成（%s）", why);
		}
		if (fx->animating) {
			wl_event_source_timer_update(fx->timer, SHAKE_TICK_MS);
			return 0;
		}
	}

	/* 补间走完：还在保持期就等，保持期过了就补间缩回去。 */
	if (fx->factor > 1.0 + SHAKE_UNIT_EPS) {
		int64_t left = fx->until_ns - now;
		if (left > 0) {
			wl_event_source_timer_update(fx->timer,
				(int)(left / 1000000) + 1);
			return 0;
		}
		wlr_log(WLR_DEBUG, "摇晃: 光标开始缩回（%.2fx → 1.00x，%dms）",
			fx->factor, SHAKE_SHRINK_MS);
		shake_animate(server, 1.0, SHAKE_SHRINK_MS);
		return 0;
	}
	shake_restore(server);
	return 0;
}

static void hold(struct vidow_server *server) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	fx->until_ns = vidow_now_ns() + (int64_t)SHAKE_HOLD_MS * 1000000;
	if (!fx->animating) {
		wl_event_source_timer_update(fx->timer, SHAKE_HOLD_MS);
	}
}

/* --------------------------------------------------------------- 放大 */

/* 按倍数 f 重画一张光标图交给 wlr_cursor（补间的每一帧都走这里）。
 * 成功时 *why 不动，失败时 *why 里是给人看的原因。 */
static bool shake_draw_at(struct vidow_server *server, double f, const char **why) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	if (fx->src == NULL) {
		*why = "原尺寸那张光标图没了";
		return false;
	}
	struct wlr_texture *src_tex =
		wlr_texture_from_buffer(server->renderer, fx->src);
	if (src_tex == NULL) {
		*why = "原尺寸那张光标图用不了";
		return false;
	}
	int src_w = fx->src->width, src_h = fx->src->height;
	int w = (int)lround(src_w * f);
	int h = (int)lround(src_h * f);
	if (w < 1) {
		w = 1;
	}
	if (h < 1) {
		h = 1;
	}
	struct wlr_fbox box = { .width = src_w, .height = src_h };
	struct wlr_buffer *big = scale_texture(server, src_tex, &box, w, h);
	wlr_texture_destroy(src_tex); /* 贴图用完就还（buffer 还留着） */
	if (big == NULL) {
		*why = "渲染放大图失败";
		return false;
	}

	int32_t hx = (int32_t)lround(fx->src_hx * f);
	int32_t hy = (int32_t)lround(fx->src_hy * f);
	wlr_cursor_set_buffer(server->cursor, big, hx, hy, (float)fx->scale);

	struct wlr_output *output = NULL;
	struct wlr_output_cursor *oc = cursor_under_pointer(server, &output);
	fx->hardware = oc != NULL && output->hardware_cursor == oc;

	/* 上一帧那张可能还在屏幕上（场景锁着它）：交给 wlroots 自己等它落地再
	 * 释放，这里只松手我们这一份引用。 */
	if (fx->big != NULL) {
		wlr_buffer_drop(fx->big);
	}
	fx->big = big;
	fx->factor = f;
	fx->big_hx = hx;
	fx->big_hy = hy;
	return true;
}

/* 把放大目标定在 fit 倍，并开始线性补间过去（放大 150ms）。天花板：放大图的
 * 物理像素不超过这块屏（比屏幕还大的光标没有意义，而且 8K 的 ARGB 缓冲就是
 * 256MB）。成功时 *why 不动，失败时 *why 里是给人看的原因。 */
static bool shake_target(struct vidow_server *server, double fit, const char **why) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	if (fx->src == NULL) {
		*why = "原尺寸那张光标图没了";
		return false;
	}
	int src_w = fx->src->width, src_h = fx->src->height;
	double cap = fit;
	if (fx->limit_w > 0) {
		cap = fmin(cap, (double)fx->limit_w / src_w);
	}
	if (fx->limit_h > 0) {
		cap = fmin(cap, (double)fx->limit_h / src_h);
	}
	if (cap < 1.05) {
		*why = "已经放到屏幕那么大了";
		return false;
	}
	if (cap < fit) {
		wlr_log(WLR_DEBUG, "摇晃: 屏幕只有 %dx%d，%.2fx 放不下，到 %.2fx 为止",
			fx->limit_w, fx->limit_h, fit, cap);
	}
	fx->want = cap;
	if (fabs(cap - fx->factor) < SHAKE_UNIT_EPS) {
		return true; /* 已经在这个倍数上了（到顶之后接着晃） */
	}
	shake_animate(server, cap, SHAKE_GROW_MS);
	return true;
}

bool vidow_cursor_fx_shake(struct vidow_server *server, const char **why) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	if (fx == NULL) {
		*why = "摇晃放大没初始化";
		return false;
	}
	if (!server->config.cursor_shake) {
		*why = "摇晃放大被配置关了（cursor_shake = false）";
		return false;
	}
	double step = server->config.cursor_shake_scale;
	if (step < SHAKE_MIN_STEP) {
		step = SHAKE_MIN_STEP;
	}
	/* 每晃一次**加**这么多倍（线性）：cursor_shake_scale = 2 就是「第一次到大
	 * 一倍，之后每次再 +1」。 */
	double inc = step - 1.0;

	if (fx->active) {
		/* 已经在放大了：接着晃就再大一圈（晃到找着为止）。加在**目标**上而不是
		 * 当前值上 —— 补间还在路上时连晃几下，每一圈都算数。到顶了也算成功：
		 * 光标这会儿本来就是大的。 */
		if (!shake_target(server, fx->want + inc, why)) {
			wlr_log(WLR_DEBUG, "摇晃: 不再放大（%s）", *why);
		}
		hold(server);
		return true;
	}

	struct wlr_output *output = NULL;
	struct wlr_output_cursor *oc = cursor_under_pointer(server, &output);
	if (output == NULL) {
		*why = "指针不在任何输出上";
		return false;
	}
	if (oc == NULL) {
		*why = "这块屏上没有光标图";
		return false;
	}
	if (oc->width <= 0 || oc->height <= 0) {
		*why = "光标图尺寸不对";
		return false;
	}
	/* 带旋转的光标图（客户端 surface 的 buffer transform）：重画一遍容易搞错
	 * 方向，宁可不放大。 */
	if (oc->transform != WL_OUTPUT_TRANSFORM_NORMAL) {
		*why = "光标图带旋转，先不放大";
		return false;
	}

	/* 原尺寸那张：先 1:1 复制一份。放大一律从它算起（不叠着放大，不然晃几下就
	 * 糊了），抓帧让位时也是拿它当替身。 */
	struct wlr_buffer *src = scale_texture(server, oc->texture, &oc->src_box,
		oc->width, oc->height);
	if (src == NULL) {
		*why = "复制光标图失败";
		return false;
	}
	double scale = output->scale > 0 ? output->scale : 1.0;
	int limit_w = 0, limit_h = 0;
	wlr_output_effective_resolution(output, &limit_w, &limit_h);
	limit_w = (int)lround(limit_w * scale);
	limit_h = (int)lround(limit_h * scale);

	drop_images(fx);
	fx->src = src;
	fx->scale = scale;
	fx->limit_w = limit_w;
	fx->limit_h = limit_h;
	fx->src_hx = (int32_t)lround(oc->hotspot_x / scale);
	fx->src_hy = (int32_t)lround(oc->hotspot_y / scale);
	fx->factor = 1.0;
	fx->want = 1.0;
	fx->animating = false;
	fx->active = true;

	if (!shake_target(server, 1.0 + inc, why)) {
		fx->active = false;
		drop_images(fx);
		wlr_log(WLR_DEBUG, "摇晃: 没放大（%s）", *why);
		return false;
	}
	hold(server);
	return true;
}

/* --------------------------------------------------------------- 抓帧 */

/* 这里原来有「抓帧让位」：放大图是软光标时，抓帧那一帧临时换回原尺寸那张，好让
 * 它落回硬件光标平面、不进截图。现在不需要了 —— vidow 的抓帧（capture.c /
 * screencopy.c）是自己渲染的一帧场景，场景里本来就没有光标：**不管硬件光标还是
 * 软光标，普通截图 / 录屏里都没有它**，而屏幕上照旧是放大后的光标（连一帧都不
 * 用缩回去）。要光标得显式要（grim -c），走 ext-image-copy-capture 的光标会话，
 * 那里给的就是放大后那张图（见 capture.c 的 cursor source）。 */


/* --------------------------------------------------------------- 摇晃判定 */

void vidow_cursor_fx_motion(struct vidow_server *server) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	if (fx == NULL || !server->config.cursor_shake) {
		return;
	}
	double x = server->cursor->x, y = server->cursor->y;
	if (!fx->have_last) {
		fx->last_x = x;
		fx->last_y = y;
		fx->have_last = true;
		return;
	}
	double dx = x - fx->last_x, dy = y - fx->last_y;
	fx->last_x = x;
	fx->last_y = y;

	double adx = fabs(dx), ady = fabs(dy);
	if (adx < 1.0 && ady < 1.0) {
		return; /* 抖不动的不算 */
	}
	int dir = adx >= ady ? (dx > 0 ? 1 : -1) : (dy > 0 ? 1 : -1);
	double dist = adx >= ady ? adx : ady;

	int64_t now = vidow_now_ns();
	if (now - fx->window_ns > (int64_t)SHAKE_WINDOW_MS * 1000000) {
		/* 窗口过期：上一串摇晃作废，重新数。 */
		fx->window_ns = now;
		fx->flips = 0;
		fx->acc = 0;
		fx->dir = 0;
	}

	if (dir == fx->dir) {
		fx->acc += dist;
	} else {
		if (fx->acc >= SHAKE_MIN_TRAVEL) {
			fx->flips++;
		}
		fx->acc = dist;
		fx->dir = dir;
	}

	if (fx->flips >= SHAKE_FLIPS) {
		fx->flips = 0;
		fx->acc = 0;
		fx->dir = 0;
		fx->window_ns = now;
		const char *why = NULL;
		vidow_cursor_fx_shake(server, &why);
	}
}

/* --------------------------------------------------------------- 光标图跟踪 */

static void surface_gone(struct wl_listener *listener, void *data) {
	struct vidow_cursor_fx *fx =
		wl_container_of(listener, fx, surface_destroy);
	(void)data;
	wl_list_remove(&fx->surface_destroy.link);
	wl_list_init(&fx->surface_destroy.link);
	fx->surface = NULL;
}

static void forget_surface(struct vidow_cursor_fx *fx) {
	wl_list_remove(&fx->surface_destroy.link);
	wl_list_init(&fx->surface_destroy.link);
	fx->surface = NULL;
}

/* 客户端换了光标图（request_set_cursor）。放大中就直接收摊 —— 新图由调用方设上
 * 去，别让我们的放大图顶掉它。 */
void vidow_cursor_fx_set_surface(struct vidow_server *server,
		struct wlr_surface *surface, int32_t hx, int32_t hy) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	if (fx == NULL) {
		return;
	}
	if (fx->active) {
		fx->active = false;
		wl_event_source_timer_update(fx->timer, 0);
		drop_images(fx);
		fx->factor = 1.0;
	}
	forget_surface(fx);
	fx->surface = surface;
	fx->hx = hx;
	fx->hy = hy;
	if (surface != NULL) {
		fx->surface_destroy.notify = surface_gone;
		wl_signal_add(&surface->events.destroy, &fx->surface_destroy);
	}
}

/* 指针底下没窗口：该显示主题默认箭头。放大期间**不碰真图**（碰了就把放大图顶
 * 掉、放大白做了），只记住「还原时该给什么」。 */
void vidow_cursor_fx_default_cursor(struct vidow_server *server) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	if (fx != NULL) {
		forget_surface(fx);
		if (fx->active) {
			return;
		}
	}
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
}

/* --------------------------------------------------------------- 状态 / 初始化 */

void vidow_cursor_fx_json(struct jbuf *b, struct vidow_server *server) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	struct wlr_output *output = NULL;
	struct wlr_output_cursor *oc = cursor_under_pointer(server, &output);
	char buf[64];

	jb_kv_bool(b, "magnified", fx != NULL && fx->active);
	jb_kv_double(b, "scale", fx != NULL && fx->active ? fx->factor : 1.0);
	/* 补间的终点：scale == target 就是这一圈已经走完（scale 在动画中间是
	 * 中间值，缩小那一段从 2 一路走回 1）。 */
	jb_kv_double(b, "target", fx != NULL && fx->active ? fx->want : 1.0);
	if (oc != NULL) {
		snprintf(buf, sizeof(buf), "%dx%d", oc->width, oc->height);
		jb_kv_str(b, "image", buf);
		jb_kv_bool(b, "hardware", output->hardware_cursor == oc);
	} else {
		jb_kv_str(b, "image", "0x0");
		jb_kv_bool(b, "hardware", false);
	}
	if (output != NULL) {
		int maxw = 0, maxh = 0;
		cursor_size_limit(output, &maxw, &maxh);
		snprintf(buf, sizeof(buf), "%dx%d", maxw, maxh);
		jb_kv_str(b, "hw_max", buf);
	}
}

void vidow_cursor_fx_init(struct vidow_server *server) {
	struct vidow_cursor_fx *fx = calloc(1, sizeof(*fx));
	if (fx == NULL) {
		wlr_log(WLR_ERROR, "摇晃: 初始化失败（内存不够），这个功能不可用");
		return;
	}
	wl_list_init(&fx->surface_destroy.link);
	fx->last_x = server->cursor->x;
	fx->last_y = server->cursor->y;
	fx->have_last = true;
	fx->factor = 1.0;
	fx->timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), shake_timer_cb, server);
	if (fx->timer == NULL) {
		free(fx);
		wlr_log(WLR_ERROR, "摇晃: 定时器建不出来，这个功能不可用");
		return;
	}
	server->cursor_fx = fx;
}
