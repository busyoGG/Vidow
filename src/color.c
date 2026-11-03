#include "server.h"

#include <math.h>
#include <stdlib.h>

#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------- 每表面编码追踪 */

/* 客户端声明的编码（wp_color_management_surface_v1）落在 surface 的 current
 * state 里，wlroots 的 wlr_scene_surface 每次 commit 会顺手把它写到自己的
 * scene buffer 上（types/scene/surface.c:290）。这里只是**看得见**：谁声明了
 * 什么、什么时候变、有没有声明。
 *
 * 为什么值得留着：第 6-9 步所有「颜色不对」的排查都要先回答「这个 surface
 * 到底声明了什么」。默认值（没声明 = gamma22/srgb）不打印，只打非默认的，
 * 平时一条日志都没有；全量要开 VIDOW_COLORTRACE=1。
 *
 * 挂 surface 的 commit 而不是 view 的 commit：layer surface、弹窗、X11、
 * 输入法候选框没有 vidow_view，漏掉它们就等于漏掉一半画面。 */
struct color_trace {
	struct wlr_surface *surface;
	struct wl_listener commit;
	struct wl_listener destroy;
	uint32_t tf, primaries, encoding, range;
	bool seen;
};

static int color_trace_on(void) {
	static int on = -1;
	if (on < 0) {
		on = getenv("VIDOW_COLORTRACE") != NULL ? 1 : 0;
	}
	return on;
}

static const char *color_tf_name(uint32_t tf) {
	switch (tf) {
	case 0:
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22:
		return "gamma22";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4:
		return "srgb";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
		return "st2084_pq";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
		return "ext_linear";
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_BT1886:
		return "bt1886";
	default:
		return "?";
	}
}

static const char *color_prim_name(uint32_t primaries) {
	switch (primaries) {
	case 0:
	case WP_COLOR_MANAGER_V1_PRIMARIES_SRGB:
		return "srgb";
	case WP_COLOR_MANAGER_V1_PRIMARIES_BT2020:
		return "bt2020";
	default:
		return "?";
	}
}

/* 没声明和「声明成 sRGB」在当前管线上完全等价，所以日志里当成同一档。 */
static bool color_is_default(struct color_trace *t) {
	bool tf_default = t->tf == 0 ||
		t->tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22 ||
		t->tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_COMPOUND_POWER_2_4;
	bool prim_default = t->primaries == 0 ||
		t->primaries == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
	return tf_default && prim_default && t->encoding == 0 && t->range == 0;
}

static void color_trace_read(struct color_trace *t) {
	const struct wlr_image_description_v1_data *desc =
		wlr_surface_get_image_description_v1_data(t->surface);
	t->tf = desc != NULL ? desc->tf_named : 0;
	t->primaries = desc != NULL ? desc->primaries_named : 0;
	/* wp_color_representation_v1：视频那种 YUV 客户端用，和 TF/primaries
	 * 是两套（一个是内容空间，一个是 buffer 的矩阵/范围）。一起打出来，
	 * 省得以后怀疑「是不是没收到」。 */
	const struct wlr_color_representation_v1_surface_state *repr =
		wlr_color_representation_v1_get_surface_state(t->surface);
	t->encoding = repr != NULL ? repr->coefficients : 0;
	t->range = repr != NULL ? repr->range : 0;
}

static void color_trace_commit(struct wl_listener *listener, void *data) {
	(void)data;
	struct color_trace *t = wl_container_of(listener, t, commit);
	uint32_t tf = t->tf, primaries = t->primaries;
	uint32_t encoding = t->encoding, range = t->range;
	color_trace_read(t);
	if (t->seen && t->tf == tf && t->primaries == primaries &&
			t->encoding == encoding && t->range == range) {
		return;
	}
	t->seen = true;
	if (!color_trace_on() && color_is_default(t)) {
		return;
	}
	wlr_log(color_trace_on() ? WLR_DEBUG : WLR_INFO,
		"color: surface %p 声明 tf=%s primaries=%s encoding=%u range=%u",
		(void *)t->surface, color_tf_name(t->tf),
		color_prim_name(t->primaries), t->encoding, t->range);
}

static void color_trace_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct color_trace *t = wl_container_of(listener, t, destroy);
	wl_list_remove(&t->commit.link);
	wl_list_remove(&t->destroy.link);
	free(t);
}

static void color_handle_new_surface(struct wl_listener *listener, void *data) {
	struct vidow_server *server = wl_container_of(listener, server, color_new_surface);
	struct wlr_surface *surface = data;
	(void)server;
	struct color_trace *t = calloc(1, sizeof(*t));
	if (t == NULL) {
		return;
	}
	t->surface = surface;
	t->commit.notify = color_trace_commit;
	wl_signal_add(&surface->events.commit, &t->commit);
	t->destroy.notify = color_trace_destroy;
	wl_signal_add(&surface->events.destroy, &t->destroy);
}

/* wlr_compositor 销毁时 wlroots 会断言它的 new_surface 监听链表已经空了
 * （types/wlr_compositor.c 的 compositor_handle_display_destroy）——
 * 摘掉我们那个，不然每次正常退出都 abort（核心转储）。 */
static void color_handle_compositor_destroy(struct wl_listener *listener,
		void *data) {
	(void)data;
	struct vidow_server *server =
		wl_container_of(listener, server, color_compositor_destroy);
	wl_list_remove(&server->color_new_surface.link);
	wl_list_init(&server->color_new_surface.link);
	wl_list_remove(&server->color_compositor_destroy.link);
	wl_list_init(&server->color_compositor_destroy.link);
}

/* 内容拷贝的标签继承：Vidow 自己烘出来的缓冲是源缓冲的逐像素拷贝
 * （gles2 不做任何色彩转换），编码必然和源一致。不继承的话场景把它当
 * 「没标色彩」—— 直通判据会误放行（PQ 内容当 sRGB 直接上屏就是发灰 /
 * 过曝），以后接上转换（第 6-9 步）还会再转一次。
 *
 * 只继承 TF/primaries，不继承 color_encoding/range：那是 YUV 矩阵，进了
 * RGBA 纹理之后已经不存在了。 */
void vidow_color_inherit_tags(struct wlr_scene_buffer *dst,
		const struct wlr_scene_buffer *src) {
	if (dst == NULL || src == NULL) {
		return;
	}
	wlr_scene_buffer_set_transfer_function(dst, src->transfer_function);
	wlr_scene_buffer_set_primaries(dst, src->primaries);
}

/* Vidow 自己画出来的「屏幕空间」内容（底衬模糊、窗口快照、关闭动画）：
 * 像素已经是输出那一套编码，现在输出是 sRGB，就明确标成 gamma22/srgb ——
 * 不标是「没有色彩信息」，以后输出切成 HDR 时这两种在直通判据里不等价。 */
void vidow_color_mark_srgb(struct wlr_scene_buffer *buffer) {
	if (buffer == NULL) {
		return;
	}
	wlr_scene_buffer_set_transfer_function(buffer,
		WLR_COLOR_TRANSFER_FUNCTION_GAMMA22);
	wlr_scene_buffer_set_primaries(buffer,
		WLR_COLOR_NAMED_PRIMARIES_SRGB);
}

/* ------------------------------------------------------ 调色的 KMS 曲线 */
/* 色温 / 亮度 / 对比度 / 伽马这四项都只是**每通道各自的一条曲线**（色温是对
 * R/G/B 各乘一个常数，算术上就是曲线的高度），所以能塞进 CRTC 的 3×1D
 * GAMMA_LUT —— 也就是 KMS 自己的硬件 LUT，renderer 不参与，直通照旧。
 *
 * 饱和度 / 色相要把 R/G/B 混在一起，1D 曲线做不到，那两项仍然走 output.c
 * 的 FP16 后处理（见 vidow_grade_can_kms）。
 *
 * 这里算的是「屏幕编码空间（gamma22）→ 屏幕编码空间」的曲线：和
 * fx-gl.c 的 vidow_grade() 在线性光里算的数学等价 —— 内部值先解码回线性光，
 * 做同样的白平衡 / 亮度 / 对比度 / 伽马，再编码回去。
 *
 * 色温增益（vidow_grade_temp_gain）两条路共用同一份、逐位一致（同一套
 * Tanner Helland 近似、同一个 6500K 归一、同样的 Rec.709 亮度归一回 1）：
 * 否则「色温开着、再把饱和度一开」时画面会在 LUT 和 shader 之间对不上，
 * 闪一下颜色。 */

/* 屏幕编码空间 <-> 线性光。**必须和 fx-gl.c 的 tf 2（gamma22，pow 2.2）
 * 一致**，不是 sRGB 的分段曲线：上屏缓冲是 renderer 按 gamma22 编码的，
 * shader 那边解码也是 pow(c, 2.2)。两者混用会在暗部差出一大截。 */
static float grade_decode_gamma22(float c) {
	return c <= 0.0f ? 0.0f : powf(c, 2.2f);
}

static float grade_linear_to_gamma22(float c) {
	return c <= 0.0f ? 0.0f : powf(c, 1.0f / 2.2f);
}

/* 色温近似的输入是 0..255 的 sRGB 值 —— 这一份用真正的 sRGB 分段曲线，
 * 和 fx-gl.c 里 post_srgb_to_linear 的算法保持一致。 */
static float grade_srgb_to_linear(float c) {
	c = fminf(fmaxf(c, 0.0f), 1.0f);
	return c <= 0.04045f ? c / 12.92f :
		powf((c + 0.055f) / 1.055f, 2.4f);
}

static void grade_kelvin_linear(float kelvin, float out[3]) {
	float t = kelvin / 100.0f;
	if (!(t > 0.0f)) {
		t = 65.0f;
	}
	float r, g, b;
	if (t <= 66.0f) {
		r = 255.0f;
		g = 99.4708025861f * logf(t) - 161.1195681661f;
		b = t <= 19.0f ? 0.0f :
			138.5177312231f * logf(t - 10.0f) - 305.0447927307f;
	} else {
		r = 329.698727446f * powf(t - 60.0f, -0.1332047592f);
		g = 288.1221695283f * powf(t - 60.0f, -0.0755148492f);
		b = 255.0f;
	}
	out[0] = grade_srgb_to_linear(r / 255.0f);
	out[1] = grade_srgb_to_linear(g / 255.0f);
	out[2] = grade_srgb_to_linear(b / 255.0f);
}

void vidow_grade_temp_gain(float kelvin, float out[3]) {
	float k[3], ref[3];
	grade_kelvin_linear(kelvin, k);
	grade_kelvin_linear(VIDOW_GRADE_NEUTRAL_TEMP, ref);
	float r = k[0] / fmaxf(ref[0], 1e-4f);
	float g = k[1] / fmaxf(ref[1], 1e-4f);
	float b = k[2] / fmaxf(ref[2], 1e-4f);
	float y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
	if (y > 1e-4f) {
		r /= y;
		g /= y;
		b /= y;
	}
	out[0] = r;
	out[1] = g;
	out[2] = b;
}

static bool grade_same(const struct vidow_grade *a, const struct vidow_grade *b) {
	return fabsf(a->saturation - b->saturation) < 1e-6f &&
		fabsf(a->contrast - b->contrast) < 1e-6f &&
		fabsf(a->temperature - b->temperature) < 1e-3f &&
		fabsf(a->hue - b->hue) < 1e-6f &&
		fabsf(a->brightness - b->brightness) < 1e-6f &&
		fabsf(a->gamma - b->gamma) < 1e-6f;
}

struct wlr_color_transform *vidow_grade_lut_get(struct vidow_server *server,
		const struct vidow_grade *grade, size_t size) {
	if (server == NULL || grade == NULL || size == 0) {
		return NULL;
	}
	if (server->grade_lut != NULL && server->grade_lut_size == size &&
			server->grade_lut_have_args &&
			grade_same(&server->grade_lut_args, grade)) {
		return server->grade_lut;
	}
	uint16_t *r = calloc(size, sizeof(uint16_t));
	uint16_t *g = calloc(size, sizeof(uint16_t));
	uint16_t *b = calloc(size, sizeof(uint16_t));
	struct wlr_color_transform *lut = NULL;
	if (r != NULL && g != NULL && b != NULL) {
		vidow_grade_curve(grade, size, r, g, b);
		lut = wlr_color_transform_init_lut_3x1d(size, r, g, b);
	}
	free(r);
	free(g);
	free(b);
	if (lut == NULL) {
		/* 建不出来就留着旧的（或者没有）：调用方会退到 shader 那条路。 */
		wlr_log(WLR_ERROR, "color: 调色曲线建不出来（%zu 项）", size);
		return NULL;
	}
	wlr_color_transform_unref(server->grade_lut);
	server->grade_lut = lut;
	server->grade_lut_size = size;
	server->grade_lut_args = *grade;
	server->grade_lut_have_args = true;
	return lut;
}

void vidow_grade_curve(const struct vidow_grade *grade, size_t size,
		uint16_t *r, uint16_t *g, uint16_t *b) {
	if (size == 0) {
		return;
	}
	float gain[3] = { 1.0f, 1.0f, 1.0f };
	vidow_grade_temp_gain(grade->temperature, gain);
	float bright = fmaxf(grade->brightness, 0.0f);
	float contrast = fmaxf(grade->contrast, 0.0f);
	float gamma = fmaxf(grade->gamma, 0.05f);
	for (size_t i = 0; i < size; i++) {
		float x = (float)i / (float)(size - 1);
		float lin = grade_decode_gamma22(x);
		for (int c = 0; c < 3; c++) {
			float v = lin * gain[c] * bright;
			v = (v - 0.18f) * contrast + 0.18f;
			if (!(v > 0.0f)) {
				v = 0.0f;
			}
			float e = grade_linear_to_gamma22(powf(v, 1.0f / gamma));
			if (!(e > 0.0f)) {
				e = 0.0f;
			} else if (e > 1.0f) {
				e = 1.0f;
			}
			/* 只夹到 0..1（和 shader 输出到 8 位缓冲时一样），不钉端头：
			 * 亮度和对比度本来就会把满值抬过 1 或压到 1 以下，硬钉 1 会
			 * 让近白处的三个通道对不上 shader（色温会被吃掉一截）。 */
			uint16_t *dst = c == 0 ? r : c == 1 ? g : b;
			dst[i] = (uint16_t)lrintf(e * 65535.0f);
		}
	}
}
/* wp_color_manager_v1：客户端（Chromium / Proton / gamescope / mpv）靠它
 * 问「这块屏现在是什么色彩空间」「我应该按什么编码画」，也是 HDR 开关的
 * 唯一判据 —— 没有这个 global，客户端一律按 sRGB，游戏里 HDR 选项根本不
 * 会出现。
 *
 * **不传给 scene**（`wlr_scene_set_color_manager_v1`）：scene 上 gamma
 * control 和 color manager 是互斥的两个位置，而 gamma 已经自己接了（见
 * output.c 的 vidow_gamma_init）。自己接只是少了两件 scene 的便利：
 *   - surface feedback 的默认值：wlroots 在 get_surface_feedback 时就填好了
 *     gamma22 + srgb，不调 set_surface_preferred_image_description 也是对的；
 *   - 输出有 image_description 时按它推 preferred —— 我们暂时不给输出设
 *     image_description，等于 sRGB，和第 9 步接 HDR 输出时再补。
 */
void vidow_color_init(struct vidow_server *server) {
	/* **只开 parametric**：wlroots 0.20.2 的 wlr_color_manager_v1_create()
	 * 把其余 feature 全 assert 成 false（icc_v2_v4 / set_primaries /
	 * set_tf_power / set_luminances / extended_target_volume /
	 * windows_scrgb 都还没实现），开了直接 abort。计划里写的
	 * set_primaries / set_tf_power / set_luminances 这一版拿不到。
	 *
	 * TF / primaries 也只报**现在真的能正确出图**的：gamma22 + srgb。
	 * gles2 没有 input_color_transform（render/gles2/renderer.c 只设了
	 * output_color_transform = false），scene 和 gles2 都不做任何色彩转换。
	 * 这一版要是把 ST2084_PQ / BT2020 报出去，客户端会按 HDR 编码画，我们
	 * 原样当 sRGB 上屏 —— 画面发灰、高光全糊。等第 6 步把 per-surface 转换
	 * 做进 fx 的自建 pass，再把它们加进这两张表（届时 --pq 才会 ready）。
	 *
	 * 注意 SRGB 不能出现在 TF 列表里（协议规定它是隐式默认，wlroots 会
	 * assert）；sRGB 的传输函数在协议里叫 compound_power_2_4。这里报
	 * gamma22，和输出 / surface feedback 的默认值一致。 */
	static const enum wp_color_manager_v1_transfer_function tfs[] = {
		WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	};
	static const enum wp_color_manager_v1_primaries prims[] = {
		WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	};
	static const enum wp_color_manager_v1_render_intent intents[] = {
		WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL,
	};
	const struct wlr_color_manager_v1_options opt = {
		.features = {
			.parametric = true,
		},
		.render_intents = intents,
		.render_intents_len = sizeof(intents) / sizeof(intents[0]),
		.transfer_functions = tfs,
		.transfer_functions_len = sizeof(tfs) / sizeof(tfs[0]),
		.primaries = prims,
		.primaries_len = sizeof(prims) / sizeof(prims[0]),
	};
	/* v2 才有 ready2（identity 64 位）+ set_max_cll / set_max_fall；
	 * wlroots 支持到 2，直接报满，客户端按自己支持的版本降级。 */
	server->color_manager = wlr_color_manager_v1_create(server->display, 2, &opt);
	if (server->color_manager == NULL) {
		wlr_log(WLR_ERROR, "颜色: 导出 wp_color_manager_v1 失败");
	}

	/* wp_color_representation_v1：告诉客户端「送到合成器的 buffer 用什么
	 * 矩阵 / 范围解释」（视频播放器、YUV dmabuf 客户端用）。渲染器不会的
	 * 编码它自己就不报，所以直接按 renderer 的能力建 —— gles2 这一版只剩
	 * identity + full range，正好等于普通 RGB buffer 的语义。 */
	server->color_representation =
		wlr_color_representation_manager_v1_create_with_renderer(
			server->display, 1, server->renderer);
	if (server->color_representation == NULL) {
		wlr_log(WLR_ERROR, "颜色: 导出 wp_color_representation_v1 失败");
	}

	/* 每个 surface 的声明都记一笔（见上面的 color_trace）。新 surface 从
	 * wlr_compositor 拿 —— xdg / layer / 弹窗 / X11 / 输入法全走这一条。 */
	wl_list_init(&server->color_new_surface.link);
	server->color_new_surface.notify = color_handle_new_surface;
	wl_signal_add(&server->compositor->events.new_surface,
		&server->color_new_surface);
	wl_list_init(&server->color_compositor_destroy.link);
	server->color_compositor_destroy.notify = color_handle_compositor_destroy;
	wl_signal_add(&server->compositor->events.destroy,
		&server->color_compositor_destroy);
}
