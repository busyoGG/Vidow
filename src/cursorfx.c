/* 摇晃找光标：快速来回晃几下指针，指针就地放大一会儿，好找。
 *
 * 放大不是另画一层，而是**换一张更大的光标图**：把当前光标图放大重画进一张新
 * buffer，再交给 wlr_cursor_set_buffer()。于是它和普通光标走同一条路 ——
 *
 *   - 硬件光标平面放得下就还是硬件光标：不进合成器的渲染通道，因此**不会出现在
 *     截图 / 录屏里**。screencopy 只有在应用显式要 overlay_cursor 时才把光标锁成
 *     软光标画进画面，那正是「显式指定」那条路。
 *   - 放不下 wlroots 会自己降级成软光标（那就进截图了），所以放大倍数先按硬件
 *     光标尺寸上限夹一下：宁可小一点，也要待在硬件平面上。
 *   - 嵌套 / headless 后端根本没有硬件光标，只能软光标 —— 和普通光标一样，不是
 *     这个功能引入的问题。
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
#include <wlr/util/log.h>

#include "json.h"
#include "server.h"

/* 摇晃判定：SHAKE_WINDOW_MS 里至少换向 SHAKE_FLIPS 次，每次换向前至少走
 * SHAKE_MIN_TRAVEL 个逻辑像素 —— 慢慢画圈不算，来回抖才算。 */
#define SHAKE_WINDOW_MS 700
#define SHAKE_MIN_TRAVEL 12.0
#define SHAKE_FLIPS 3
/* 放大后保持多久。 */
#define SHAKE_HOLD_MS 700
/* 后端没报硬件光标尺寸上限时的天花板：只有嵌套 Wayland / headless / X11 会走到
 * 这里 —— 它们把光标交给上层合成器画，没有硬性的平面尺寸。真后端（DRM）会报
 * 上限，那就按它夹。 */
#define SHAKE_SOFT_MAX 256

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
	double applied;      /* 实际用上的倍数（可能被硬件上限夹过） */
	bool hardware;       /* 放大图在不在硬件光标平面上 */
	struct wl_event_source *timer;
	struct wlr_buffer *buffer;

	/* 当前光标图（还原用）：客户端 surface 或主题 xcursor（surface == NULL） */
	struct wlr_surface *surface;
	int32_t hx, hy;
	struct wl_listener surface_destroy;
};

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

/* 硬件光标能画多大。没有硬件光标（嵌套 / headless 后端）就返回一个够宽松的
 * 天花板，反正那种情况下本来就是软光标。 */
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

/* --------------------------------------------------------------- 还原 */

static void drop_buffer(struct vidow_cursor_fx *fx) {
	if (fx->buffer != NULL) {
		wlr_buffer_drop(fx->buffer);
		fx->buffer = NULL;
	}
}

static void shake_restore(struct vidow_server *server) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	if (!fx->active) {
		return;
	}
	fx->active = false;
	wl_event_source_timer_update(fx->timer, 0);
	if (fx->surface != NULL) {
		wlr_cursor_set_surface(server->cursor, fx->surface, fx->hx, fx->hy);
	} else {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "left_ptr");
	}
	drop_buffer(fx);
	wlr_log(WLR_DEBUG, "摇晃: 光标还原（%.2fx 收起）", fx->applied);
}

static int shake_timer_cb(void *data) {
	struct vidow_server *server = data;
	struct vidow_cursor_fx *fx = server->cursor_fx;
	int64_t left = fx->until_ns - vidow_now_ns();
	if (left > 0) {
		wl_event_source_timer_update(fx->timer, (int)(left / 1000000) + 1);
		return 0;
	}
	shake_restore(server);
	return 0;
}

/* --------------------------------------------------------------- 放大 */

/* 把 oc 的光标图放大 fit 倍，画进一张新 buffer 交给光标系统。
 * 成功时 *why 不动，失败时 *why 里是给人看的原因。 */
static bool shake_apply(struct vidow_server *server, double fit, const char **why) {
	struct vidow_cursor_fx *fx = server->cursor_fx;
	struct wlr_cursor *cur = server->cursor;

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

	double scale = output->scale > 0 ? output->scale : 1.0;
	if (fit < 1.05) {
		*why = "放大倍数太小，看不出来";
		return false;
	}

	int src_w = oc->width, src_h = oc->height;
	int w = (int)lround(src_w * fit);
	int h = (int)lround(src_h * fit);
	struct wlr_buffer *buf = vidow_alloc_target(server, w, h);
	if (buf == NULL) {
		*why = "分配放大用的缓冲失败";
		return false;
	}
	struct wlr_render_pass *pass =
		wlr_renderer_begin_buffer_pass(server->renderer, buf, NULL);
	if (pass == NULL) {
		wlr_buffer_drop(buf);
		*why = "拿不到渲染通道";
		return false;
	}
	/* 先清成全透明（光标图带 alpha，不能留底色），再把光标图放大画上去。 */
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { .width = w, .height = h },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
		.texture = oc->texture,
		.src_box = oc->src_box,
		.dst_box = { .width = w, .height = h },
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	});
	if (!wlr_render_pass_submit(pass)) {
		wlr_buffer_drop(buf);
		*why = "渲染放大图失败";
		return false;
	}

	/* hotspot 走逻辑坐标（wlr_cursor_set_buffer 自己乘输出缩放），所以先除回去。 */
	int32_t hx = (int32_t)lround(oc->hotspot_x * fit / scale);
	int32_t hy = (int32_t)lround(oc->hotspot_y * fit / scale);
	wlr_cursor_set_buffer(cur, buf, hx, hy, (float)scale);

	fx->buffer = buf; /* 这份引用我们自己留着，还原时放掉 */
	fx->applied = fit;
	fx->active = true;
	fx->hardware = output->hardware_cursor == oc;
	fx->until_ns = vidow_now_ns() + (int64_t)SHAKE_HOLD_MS * 1000000;
	wl_event_source_timer_update(fx->timer, SHAKE_HOLD_MS);
	wlr_log(WLR_INFO, "摇晃: 光标放大 %.2fx（%dx%d → %dx%d，%s）", fit,
		src_w, src_h, w, h,
		fx->hardware ? "硬件光标，不进截图" : "软光标，会进截图");
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
	if (fx->active) {
		/* 已经在放大了：续个期，不重画。 */
		fx->until_ns = vidow_now_ns() + (int64_t)SHAKE_HOLD_MS * 1000000;
		wl_event_source_timer_update(fx->timer, SHAKE_HOLD_MS);
		return true;
	}

	struct wlr_output *output = NULL;
	struct wlr_output_cursor *oc = cursor_under_pointer(server, &output);
	double fit = server->config.cursor_shake_scale;
	if (fit < 1.05) {
		fit = 1.05;
	}
	if (output != NULL && oc != NULL) {
		/* 按硬件光标上限夹一下：超了 wlroots 会降级成软光标，那就进截图了。 */
		int maxw = 0, maxh = 0;
		cursor_size_limit(output, &maxw, &maxh);
		double cap = fit;
		if (maxw > 0) {
			cap = fmin(cap, (double)maxw / oc->width);
		}
		if (maxh > 0) {
			cap = fmin(cap, (double)maxh / oc->height);
		}
		if (cap < fit) {
			wlr_log(WLR_DEBUG,
				"摇晃: 硬件光标最大 %dx%d，%.2fx 放不下，夹到 %.2fx",
				maxw, maxh, fit, cap);
		}
		fit = cap;
	}
	if (!shake_apply(server, fit, why)) {
		wlr_log(WLR_DEBUG, "摇晃: 没放大（%s）", *why);
		return false;
	}
	return true;
}

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
		drop_buffer(fx);
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
	jb_kv_double(b, "scale", fx != NULL && fx->active ? fx->applied : 1.0);
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
	fx->applied = 1.0;
	fx->timer = wl_event_loop_add_timer(
		wl_display_get_event_loop(server->display), shake_timer_cb, server);
	if (fx->timer == NULL) {
		free(fx);
		wlr_log(WLR_ERROR, "摇晃: 定时器建不出来，这个功能不可用");
		return;
	}
	server->cursor_fx = fx;
}
