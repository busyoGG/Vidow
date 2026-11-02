/* 显示器（输出）的配置：一块屏一组，写在配置里。
 *
 *   vidow.output{ name = "DP-3", scale = 2, x = 0,    y = 0 }   -- 左边那块
 *   vidow.output{ name = "DP-2", scale = 2, x = 1920, y = 0 }   -- 右边那块
 *
 * 为什么是「一块屏一组」而不是两个平铺键（老配置的 outputs + output_positions）：
 * 同一块屏的缩放和位置本来是一件事 —— 「DP-3 按 2 倍、摆在 0,0」。拆成两个字符串
 * 键之后，屏的名字要在两处各写一遍，改一块屏要改两行，手滑只改一行就得到一块
 * 「缩放对了位置没对」的屏，而且看不出来是哪一半没生效。
 *
 * 没写的字段 = 不管：scale 不写就用全局 scale（auto 或写死的数），位置不写就按
 * 输出到达顺序自动排（那个顺序是后端说了算，所以「哪块在左边」要确定就得写）。
 */
#ifndef VIDOW_OUTPUTCONF_H
#define VIDOW_OUTPUTCONF_H

#include <stdbool.h>
#include <stddef.h>

#include <wayland-server-protocol.h> /* enum wl_output_transform */
#include <wayland-util.h>

struct vidow_server;

struct vidow_output_conf {
	struct wl_list link; /* vidow_server.output_confs */
	char name[64];       /* 输出名（DRM 上是 DP-1 / HDMI-A-1 这种） */

	bool has_scale;      /* 写了 scale 才管这块屏的缩放 */
	char scale[16];      /* "auto" / "2" / "off"（同全局 scale 的写法） */

	bool has_pos;        /* 写了 x 或 y 才管它的位置 */
	int x, y;            /* 布局位置，逻辑坐标 */

	/* 模式：分辨率 + 刷新率（mode = "3840x2160@144"）。hz = 0 = 只要分辨率
	 * 对上，刷新率不挑（拿那块屏的首选刷新率）。 */
	bool has_mode;
	int mode_w, mode_h;
	double mode_hz;

	/* 可变刷新率（adaptive sync / VRR）。后端或这块屏不支持时 vidow 只报一句，
	 * 不影响别的（见 output.c 的 output_apply_conf_extra）。 */
	bool has_vrr;
	bool vrr;

	/* 按需 VRR（niri 的 `variable-refresh-rate on-demand=true`）：这块屏默认
	 * 「有要 VRR 的窗口显示着才提刷新率」。不写就继承全局 vrr_on_demand
	 * （默认 false = 不碰 VRR）；写了 vrr（静态开 / 关）时它让位给 vrr。 */
	bool has_vrr_on_demand;
	bool vrr_on_demand;

	/* 渲染位深：8 或 10。10 位走 DRM_FORMAT_XRGB2101010 的输出缓冲，需要
	 * 显卡 + 显示器 + 渲染器都支持；不支持时保持 8 位并说清楚。 */
	bool has_depth;
	int depth;

	/* 输出端 HDR（第 9 步）：true = 场景先渲进 FP16 离屏，再一趟全屏 shader
	 * 做色域 / tone map / PQ 编码 / dither 上屏。默认 false（SDR，老路径）。
	 * 需要显示器支持 BT.2020 + PQ（见 EDID），否则只是把画面搞坏。 */
	bool has_hdr;
	bool hdr;
	bool has_sdr_white_nits;
	double sdr_white_nits;

	/* 方向 / 翻转（normal / 90 / 180 / 270 / flipped / flipped-90 …） */
	bool has_transform;
	enum wl_output_transform transform;

	/* 这块屏开不开。false = 关掉它（DRM 那边就是关 CRTC），上面的窗口留着。 */
	bool has_enabled;
	bool enabled;
};

/* 配置里的一条（Lua 那边解析完表之后传过来；name 必须有，别的可以为空）。 */
struct vidow_output_spec {
	const char *name;
	bool has_scale;
	const char *scale;
	bool has_pos;
	int x, y;
	bool has_mode;
	int mode_w, mode_h;
	double mode_hz;
	bool has_vrr;
	bool vrr;
	bool has_vrr_on_demand;
	bool vrr_on_demand;
	bool has_depth;
	int depth;
	bool has_hdr;
	bool hdr;
	bool has_sdr_white_nits;
	double sdr_white_nits;
	bool has_transform;
	enum wl_output_transform transform;
	bool has_enabled;
	bool enabled;
};

bool vidow_output_conf_init(struct vidow_server *server);

/* 清空（SIGHUP 重载 = 重新声明一遍）。 */
void vidow_output_conf_clear(struct vidow_server *server);

/* 加一组。同一个名字写两遍报错（返回 NULL 并填 err）—— 那多半是复制粘贴没改名，
 * 悄悄让后面那条覆盖前面那条，人只会看到「怎么没生效」。 */
struct vidow_output_conf *vidow_output_conf_add(struct vidow_server *server,
		const struct vidow_output_spec *spec, char *err, size_t errlen);

/* 这块屏的那一组配置，没写过返回 NULL。 */
const struct vidow_output_conf *vidow_output_conf_for(
		const struct vidow_server *server, const char *name);

#endif /* VIDOW_OUTPUTCONF_H */
