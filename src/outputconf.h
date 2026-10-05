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

#include <wayland-util.h>

struct vidow_server;

struct vidow_output_conf {
	struct wl_list link; /* vidow_server.output_confs */
	char name[64];       /* 输出名（DRM 上是 DP-1 / HDMI-A-1 这种） */

	bool has_scale;      /* 写了 scale 才管这块屏的缩放 */
	char scale[16];      /* "auto" / "2" / "off"（同全局 scale 的写法） */

	bool has_pos;        /* 写了 x 或 y 才管它的位置 */
	int x, y;            /* 布局位置，逻辑坐标 */
};

/* 配置里的一条（Lua 那边解析完表之后传过来；name 必须有，别的可以为空）。 */
struct vidow_output_spec {
	const char *name;
	bool has_scale;
	const char *scale;
	bool has_pos;
	int x, y;
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
