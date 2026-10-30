/* layer 背后的模糊：区域判据 + 那一小块底衬的构建。
 *
 * 调用方：fx.c 的 vidow_blur_update（每帧每块屏）、弹出物那一层（复用区域判据）。
 * 只依赖 fx-internal.h 里的基础设施，不反过来依赖 fx.c 的窗口那一路。 */
#ifndef VIDOW_FX_LAYER_H
#define VIDOW_FX_LAYER_H

#include "server.h"

#include <stdbool.h>

/* 这块 layer 现在要不要模糊（配置 + 规则 + 可见性）。 */
bool layer_blur_wanted(struct vidow_server *server,
	struct vidow_layer_surface *ls);
/* 它的模糊在屏幕上真的看得见吗（内容不透明的整块层不糊）。 */
bool layer_blur_visible(struct vidow_layer_surface *ls);
/* 模糊区域在屏幕上的框（设备像素）+ 级数。 */
bool layer_region_rect(struct vidow_server *server,
	struct vidow_output *output, struct vidow_layer_surface *ls,
	int *rx, int *ry, int *rw, int *rh, int *levels);
/* 抓底衬时把每块 layer 的模糊节点 / 它本体一起藏起来（on=false），画完再回填。
 * 不藏就是「自己糊自己」：半透明的地方透出这块面板自己的糊版。 */
void layer_blur_capture_visible(struct vidow_server *server, bool on);

/* overview 里那张糊掉的壁纸（整屏，垫在压暗遮罩下面）。 */
void vidow_overview_blur_frame(struct vidow_server *server,
	struct vidow_output *output);

/* 这一帧要不要把「layer 之下」抓一次给所有层共用（区域互相压住才值得）。 */
void layer_under_plan(struct vidow_server *server,
	struct vidow_output *output);
/* 抓这块区域背后的底衬（含窗口），并跑完模糊链。partial 非空时只重跑那一块。 */
bool layer_region_build(struct vidow_server *server,
	struct vidow_output *output, struct vidow_layer_surface *ls,
	int rx, int ry, int rw, int rh, int levels,
	const struct wlr_box *partial);
/* 没画成的出口：只在原因变了时打一行。 */
bool layer_blur_bail(struct vidow_layer_surface *ls, int reason,
	const char *what);

#endif
