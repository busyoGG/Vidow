#include "server.h"

#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_color_representation_v1.h>
#include <wlr/util/log.h>

/* ---------------------------------------------------------- 色彩管理导出 */
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
}
