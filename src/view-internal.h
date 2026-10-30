/* view 模块的内部接口（只在 src/view*.c 之间用）。
 *
 * 和 fx 那边同一个规矩：对外 API 在 server.h（vidow_view_*），这里只放
 * 「拆分之后两个文件必须互相看见」的那几个 static。
 *
 * 方向：view-access.c（属性 / 几何 / 命中）→ view.c（布局 / 动画 / 事件），
 * 反过来只有三个更靠里的小工具。
 */
#ifndef VIDOW_VIEW_INTERNAL_H
#define VIDOW_VIEW_INTERNAL_H

#include "server.h"

#include <stdbool.h>

/* ---- view-access.c 出去 ---- */

/* 这个 wlr_surface 属于哪个窗口（弹出物 / subsurface 都往上找根）。 */
struct vidow_view *view_from_surface(struct vidow_server *server,
	struct wlr_surface *surface);
/* X11 的 DPI 补偿：按窗口自己的 x11_scale 把逻辑尺寸算出来。 */
void x11_apply_rule_scale(const struct vidow_view *view, double *w, double *h);
/* 进 / 退全屏（含 X11 同步与布局重排）。 */
void view_apply_fullscreen(struct vidow_view *view, bool want, const char *why);
void view_sync_x11_fullscreen(struct vidow_view *view);
/* 把当前节点尺寸记成基线（小窗 / overview 退出后回到这个尺寸）。 */
void view_dst_capture(struct vidow_view *view);
/* 窗口几何 → 装饰着色器的纹理映射（tex_u0/v0/du/dv）。 */
void fx_geo_tex_map(struct vidow_fx_geo *geo,
	const struct wlr_scene_buffer *sb, const struct wlr_surface *surface,
	const struct vidow_view *view);

/* ---- view.c 出去（给 view-access.c 用） ---- */

/* 窗口在布局坐标里被裁到的那块（哪块屏上都没画着时返回 false）。 */
bool view_clip_box(struct vidow_view *view, struct wlr_box *out);
/* 小窗（S3）：窗口的原生尺寸（场景坐标）。 */
void view_mini_native(const struct vidow_view *view, int *w, int *h);
/* X11：按窗口的绝对坐标把它归到某块输出上。 */
void x11_track_output(struct vidow_view *view);

#endif
