/* 缩放计算的离线自检：不启动合成器，直接对着数字验。
 *   meson compile -C build scaletest && ./build/scaletest
 * 退出码 0 = 全过。 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "scale.h"

static int failures;

static void check(bool ok, const char *what) {
	if (!ok) {
		failures++;
		printf("FAIL %s\n", what);
	} else {
		printf("ok   %s\n", what);
	}
}

static bool near(double a, double b, double eps) {
	return fabs(a - b) <= eps;
}

struct panel {
	const char *name;  /* 输出名，就是 vidow.output{ name = … } 里写的那个 */
	const char *label; /* 人看的 */
	int w, h, mm_w, mm_h;
};

/* 常见搭配：4K 27 寸、1080p 24 寸、1440p 27 寸。 */
static const struct panel panels[] = {
	{ "DP-1",       "4K 27\"",    3840, 2160, 597, 336 },
	{ "HDMI-A-1",   "1080p 24\"", 1920, 1080, 531, 299 },
	{ "eDP-1",      "1440p 27\"", 2560, 1440, 597, 336 },
};

static void report_physical_size(const struct panel *a, const struct panel *b,
		const char *global, const struct vidow_scale_spec *named_a,
		const struct vidow_scale_spec *named_b) {
	double sa = vidow_scale_resolve(named_a, global, a->w, a->h, a->mm_w, a->mm_h);
	double sb = vidow_scale_resolve(named_b, global, b->w, b->h, b->mm_w, b->mm_h);

	double diag_a = sqrt((double)a->mm_w * a->mm_w + (double)a->mm_h * a->mm_h);
	double diag_b = sqrt((double)b->mm_w * b->mm_w + (double)b->mm_h * b->mm_h);
	double ppi_a = sqrt((double)a->w * a->w + (double)a->h * a->h) /
		(diag_a / 25.4);
	double ppi_b = sqrt((double)b->w * b->w + (double)b->h * b->h) /
		(diag_b / 25.4);

	/* 同一个逻辑尺寸 L 在两块屏上的物理尺寸（英寸） */
	const double L = 200.0;
	double inch_a = L * sa / ppi_a;
	double inch_b = L * sb / ppi_b;

	printf("     %s scale %.2f (%.1f ppi) x %.4f in | "
		"%s scale %.2f (%.1f ppi) x %.4f in | 差 %.2f%%\n",
		a->label, sa, ppi_a, inch_a, b->label, sb, ppi_b, inch_b,
		fabs(inch_a - inch_b) / inch_a * 100.0);
}

int main(void) {
	printf("== 解析 ==\n");
	struct vidow_scale_spec spec;
	check(vidow_scale_parse("auto", &spec) && spec.kind == VIDOW_SCALE_AUTO,
		"\"auto\" -> AUTO");
	check(vidow_scale_parse(" 1.75 ", &spec) && spec.kind == VIDOW_SCALE_FIXED
		&& near(spec.value, 1.75, 1e-9), "\" 1.75 \" -> 1.75");
	check(vidow_scale_parse("off", &spec) && spec.kind == VIDOW_SCALE_OFF,
		"\"off\" -> OFF");
	check(!vidow_scale_parse("", &spec), "\"\" 拒绝");
	check(!vidow_scale_parse("1.5x", &spec), "\"1.5x\" 拒绝");
	check(!vidow_scale_parse("abc", &spec), "\"abc\" 拒绝");
	check(!vidow_scale_parse("0", &spec), "\"0\" 拒绝");

	printf("== 每块屏点名（vidow.output{ name = …, scale = … }）==\n");
	/* 每块屏那一组里的 scale 就是这段文本，解析规则和全局 scale 一模一样。 */
	check(vidow_scale_parse("2", &spec) && near(spec.value, 2.0, 1e-9),
		"scale = 2 -> 2");
	check(vidow_scale_parse("1.25", &spec) && near(spec.value, 1.25, 1e-9),
		"scale = \"1.25\" -> 1.25");
	check(vidow_scale_parse("auto", &spec) && spec.kind == VIDOW_SCALE_AUTO,
		"scale = \"auto\" -> auto");
	check(!vidow_scale_parse("2x", &spec), "scale = \"2x\" 拒绝（vidow.output 报错）");

	printf("== DPI 推算 ==\n");
	double s4k = vidow_scale_from_dpi(3840, 2160, 597, 336);
	double s1080 = vidow_scale_from_dpi(1920, 1080, 531, 299);
	double s1440 = vidow_scale_from_dpi(2560, 1440, 597, 336);
	printf("     4K27      %.4f\n     1440p27   %.4f\n     1080p24   %.4f\n",
		s4k, s1440, s1080);
	check(near(s4k, 1.70, 0.01), "4K 27 寸 ≈ 1.70（163ppi / 96）");
	check(near(s1440, 1.13, 0.02), "1440p 27 寸 ≈ 1.13（109ppi / 96）");
	check(near(s1080, 0.96, 0.02), "1080p 24 寸 ≈ 0.96（92ppi / 96）");
	check(vidow_scale_from_dpi(3840, 2160, 0, 0) == 0.0,
		"没有 EDID 尺寸 -> 0（推不出来）");

	printf("== 量化 ==\n");
	check(near(vidow_scale_quantize(1.7037037, 1.0, 8.0), 1.70, 1e-9),
		"1.7037037 -> 1.70");
	check(near(vidow_scale_quantize(0.956, 1.0, 8.0), 1.0, 1e-9),
		"0.956 夹到 1.0（不降到 1 以下，降了就整屏放大糊）");
	check(near(vidow_scale_quantize(20.0, 1.0, 8.0), 8.0, 1e-9), "上限 8.0");

	printf("== 端到端：同一个逻辑尺寸在两块屏上的物理尺寸 ==\n");
	struct vidow_scale_spec named2 = { .kind = VIDOW_SCALE_FIXED, .value = 2.0 };
	struct vidow_scale_spec named1 = { .kind = VIDOW_SCALE_FIXED, .value = 1.0 };
	printf("  全局 scale=auto：\n");
	report_physical_size(&panels[0], &panels[1], "auto", NULL, NULL);
	report_physical_size(&panels[2], &panels[1], "auto", NULL, NULL);
	printf("  两块屏各自点名 4K:2 1080p:1：\n");
	report_physical_size(&panels[0], &panels[1], "auto", &named2, &named1);

	printf("== 优先级 ==\n");
	check(near(vidow_scale_resolve(&named2, "1.5", 3840, 2160, 597, 336),
		2.0, 1e-9), "这块屏自己点名的压过全局 scale");
	check(near(vidow_scale_resolve(NULL, "1.5", 3840, 2160, 597, 336),
		1.5, 1e-9), "没点名用全局 scale");
	check(near(vidow_scale_resolve(NULL, "", 3840, 2160, 597, 336),
		1.70, 1e-9), "两个都没写 -> auto");
	check(near(vidow_scale_resolve(NULL, "", 3840, 2160, 0, 0),
		1.0, 1e-9), "auto 但没 EDID -> 1.0");
	check(near(vidow_scale_resolve(NULL, "off", 3840, 2160, 597, 336),
		1.0, 1e-9), "\"off\" -> 1.0");

	printf("== X 屏幕对账（这台 Xwayland 认不认那个属性）==\n");
	/* 一块 2 倍屏 1280x720：布局逻辑 640x360，认属性的话 X 屏幕该是 1280x720 */
	check(vidow_scale_x11_fit(1280, 720, 640, 360, 2.0) ==
		VIDOW_X11_FIT_DEVICE_PX, "1280x720 = 640x360 × 2 -> 认");
	check(vidow_scale_x11_fit(640, 360, 640, 360, 2.0) ==
		VIDOW_X11_FIT_LOGICAL_PX, "640x360 只有逻辑尺寸 -> 不认（要退回 1 倍）");
	check(vidow_scale_x11_fit(639, 359, 640, 360, 2.0) ==
		VIDOW_X11_FIT_LOGICAL_PX, "差 1 像素也算逻辑尺寸（Xwayland 会取整）");
	check(vidow_scale_x11_fit(1281, 721, 640, 360, 2.0) ==
		VIDOW_X11_FIT_DEVICE_PX, "差 1 像素也算设备像素");
	check(vidow_scale_x11_fit(1920, 1080, 640, 360, 2.0) ==
		VIDOW_X11_FIT_UNKNOWN, "对不上账（两块屏还没摆好）-> 不下结论");
	check(vidow_scale_x11_fit(0, 0, 640, 360, 2.0) ==
		VIDOW_X11_FIT_UNKNOWN, "X 屏幕 0x0（一块屏都没有）-> 不下结论");
	check(vidow_scale_x11_fit(1280, 720, 0, 0, 2.0) ==
		VIDOW_X11_FIT_UNKNOWN, "布局还是空的 -> 不下结论");
	check(vidow_scale_x11_fit(640, 360, 640, 360, 1.0) ==
		VIDOW_X11_FIT_DEVICE_PX, "想要 1 倍时逻辑尺寸就是对的（没什么可量的）");

	printf("\n%s（%d 项失败）\n", failures == 0 ? "全部通过" : "有失败", failures);
	return failures == 0 ? 0 : 1;
}
