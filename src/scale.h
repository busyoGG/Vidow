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

/* X 屏幕量出来的尺寸说明了什么（见 xwayland.c 的探测）：补丁版 Xwayland 收到
 * _XWAYLAND_GLOBAL_OUTPUT_SCALE 才把 X 屏幕按设备像素报。 */
enum vidow_x11_fit {
	VIDOW_X11_FIT_UNKNOWN = 0, /* 对不上账：还没量到，或者布局刚变它还没跟上 */
	VIDOW_X11_FIT_DEVICE_PX,   /* X 屏幕 = 逻辑 × want：属性生效了 */
	VIDOW_X11_FIT_LOGICAL_PX,  /* X 屏幕只有逻辑尺寸：它不认那个属性 */
};

/* screen_w/h = X 屏幕的尺寸（像素），layout_w/h = 布局的逻辑尺寸，want = 想让它
 * 是逻辑的几倍。 */
enum vidow_x11_fit vidow_scale_x11_fit(int screen_w, int screen_h,
		int layout_w, int layout_h, double want);

/* 一个输出的最终缩放，优先级：这块屏自己的（override，见 outputconf.c）>
 * 全局 scale > auto。override 为 NULL = 这块屏没被点名。 */
double vidow_scale_resolve(const struct vidow_scale_spec *override,
		const char *global, int w, int h, int mm_w, int mm_h);

#endif /* VIDOW_SCALE_H */
