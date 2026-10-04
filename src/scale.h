/* 输出缩放的计算。这里只放纯函数：不碰 wlroots，不碰全局状态，所以
 * tools/scaletest.c 能直接拿一份编译出来对着数字验。 */
#ifndef VIDOW_SCALE_H
#define VIDOW_SCALE_H

#include <stdbool.h>

enum vidow_scale_kind {
	VIDOW_SCALE_AUTO = 0, /* 按物理尺寸推：让逻辑像素 ≈ 1/96 英寸 */
	VIDOW_SCALE_FIXED,    /* 配置里写死的数 */
	VIDOW_SCALE_OFF,      /* X11 专用：不做缩放桥，X 一律 1:1 */
};

struct vidow_scale_spec {
	enum vidow_scale_kind kind;
	double value; /* 仅 FIXED 有意义 */
};

/* "auto" / "1.75" / "off"（X11 专用）。返回 false = 认不出来。 */
bool vidow_scale_parse(const char *text, struct vidow_scale_spec *out);

/* 让逻辑像素等于 1/96 英寸所需的缩放：ppi / 96。mm 缺失（<=0）时返回 0，
 * 表示"没有 EDID，推不出来"。 */
double vidow_scale_from_dpi(int w, int h, int mm_w, int mm_h);

/* 量化到 1/20 步长再夹进 [min, max]：ppi/96 会算出 1.7037037 这种噪声值，
 * 直接拿去当缩放会让每一帧的取整都不一样。 */
double vidow_scale_quantize(double scale, double min, double max);

/* 在 "NAME:SCALE NAME:SCALE" 里查 NAME。返回 false = 这个输出没被点名。 */
bool vidow_scale_lookup(const char *outputs, const char *name,
		struct vidow_scale_spec *out);

/* 一个输出的最终缩放，优先级：每输出覆盖 > 全局 scale > auto。 */
double vidow_scale_resolve(const char *name, const char *global,
		const char *outputs, int w, int h, int mm_w, int mm_h);

#endif /* VIDOW_SCALE_H */
