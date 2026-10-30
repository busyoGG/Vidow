/* fx 模块的**内部**接口。
 *
 * 只有 src/fx*.c 之间互相用；对外 API 在 server.h（vidow_fx_* / vidow_blur_*）。
 *
 * 为什么要有这一层：fx 做的是「离屏烘一张、再让场景节点画那张」，拆开是四件事：
 *   1. 目标缓冲 + 模糊链（金字塔）：谁能画、画多大、跑几趟 —— 与窗口无关；
 *   2. 底衬抓取：把「这一块屏幕上现在是什么」画进一张缓冲 —— 与窗口无关；
 *   3. 损伤账本：这块底衬什么时候过期 —— 与窗口无关；
 *   4. 具体消费者：窗口装饰、layer 背后、弹出物背后 —— 只有这一层知道
 *      「窗口 / layer / 弹出物」。
 *
 * 前三件是基础设施，第 4 件有三份。以前四件事挤在一个 7200 行的 fx.c 里，
 * 第 3 层改一个坐标要读懂前两层，第 4 层之间还互相直接调（弹出物要调 layer
 * 的 layer_bd_drop）。现在前三件留在这里，第 4 件各自一个文件。
 *
 * 这些符号没有 vidow_ 前缀：它们是模块内的，不承诺 ABI，也别从别处 include。 */
#ifndef VIDOW_FX_INTERNAL_H
#define VIDOW_FX_INTERNAL_H

#include "server.h"

#include <stdbool.h>
#include <stdint.h>

/* 离屏目标的上限：再大就不建了（整屏 8K 的两倍，防止某块屏尺寸异常时爆炸）。 */
#define MAX_TARGET_PX 8192
/* layer 模糊区域的**块感**：区域按这个网格对齐，移动面板时缓冲不重建。 */
#define VIDOW_BLUR_REGION_GRID 128

/* ---------------------------------------------------------------- 底衬抓取 */

/* 抓底衬时给 backdrop_iter 的上下文。
 *
 * 场景图里的一切（节点坐标、dst 尺寸）都是**逻辑像素**，底衬缓冲却是设备像素 ——
 * 少了 scale 这个倍率，1.7 倍屏上底衬只在左上角 1/1.7 的一小块里画了内容（缩小
 * 的），其余全空：模糊采到的是空像素，越糊越透明，layer 背后的底衬也跟着错位。
 * 原点同理：多屏时得减掉这块屏自己的布局原点。 */
struct backdrop_ctx {
	struct wlr_render_pass *pass;
	struct vidow_server *server;
	double scale;
	int ox, oy; /* 这块屏在布局里的原点，换算成设备像素 */
	/* 只要这块目标区域（设备像素，目标缓冲的坐标系）。抓整屏时是 0（不管），抓
	 * layer 背后那一小块时，落在外面的 buffer 连贴图都不用建。 */
	int clip_x, clip_y, clip_w, clip_h;
	/* 抓私有底衬时明确跳过这个窗口自己的节点（即使节点开关有漏网）。 */
	struct vidow_view *skip;
};

void backdrop_iter(struct wlr_scene_buffer *sb, int sx, int sy, void *data);
void backdrop_clip_check(struct wlr_scene_buffer *sb, int sx, int sy, void *data);
bool backdrop_render(struct vidow_server *server, struct vidow_output *output,
	int w, int h);

/* 一行 buffer 的贴图包装（统计 tex_new / tex_del 用）。 */
struct wlr_texture *stat_tex_new(struct wlr_texture *t);
void stat_tex_del(struct wlr_texture *t);

/* 一行 buffer → 贴图。客户端缓冲自己带纹理时直接借（*owned = false），
 * 否则导入一张（*owned = true，调用方负责销毁）。 */
struct wlr_texture *client_texture(struct wlr_renderer *renderer,
	struct wlr_buffer *buffer, bool *owned);

/* 装饰着色器那一趟：把 src 按 look 画进 dst，配上模糊纹理 / 圆角 / 边框。
 *
 * 名字叫「装饰」，但三份消费者都用它来出图：窗口装饰、layer 背后的模糊、
 * 弹出物背后的模糊、overview 壁纸。mask 非空时按它的 alpha 剔。 */
bool decor_draw(struct vidow_server *server, struct vidow_fx_target *fx,
	struct wlr_buffer *dst, struct wlr_texture *src,
	struct wlr_texture *mask, const float border[4], int ox, int oy,
	double alpha, double scale, const struct vidow_look *look);

/* 模糊节点不吃输入（点了要穿到下面的窗口）。 */
bool fx_node_rejects_input(struct wlr_scene_buffer *buffer, double *sx,
	double *sy);

/* ---------------------------------------------------------------- 损伤账本 */

void blur_box_add(struct wlr_box *dst, const struct wlr_box *src);
struct wlr_box dmg_bounds(const struct vidow_dmg_set *set);
void dmg_clear(struct vidow_dmg_set *set);
void damage_bump(struct vidow_server *server, bool layers);
bool blur_damage_hits(const struct vidow_fx_target *fx,
	const struct wlr_box *region);

/* ------------------------------------------------------------ 目标与模糊链 */

struct wlr_buffer *vidow_alloc_target(struct vidow_server *server, int w,
	int h);
struct wlr_buffer *pick_free(struct wlr_buffer *bufs[2]);
bool ensure_targets(struct vidow_server *server, struct vidow_fx_target *fx,
	int content_w, int content_h, double scale, double os);
void drop_targets(struct vidow_fx_target *fx);

bool blur_set_alloc(struct vidow_server *server, struct vidow_blur_set *set,
	int w, int h, int levels);
void blur_set_drop(struct vidow_blur_set *set);
void blur_chain(struct vidow_server *server, struct vidow_blur_set *set,
	struct wlr_texture *backdrop_tex, int w, int h);
void blur_chain_rect(struct vidow_server *server, struct vidow_blur_set *set,
	struct wlr_texture *backdrop_tex, int w, int h,
	const struct wlr_box *rect, int reach);
int blur_levels_for(struct vidow_server *server, int w, int h);
int blur_reach_px(struct vidow_server *server, int levels);

/* --------------------------------------------------- 每屏槽位与私有底衬账本 */

struct vidow_blur_slot *blur_slot_for(struct vidow_fx_target *fx,
	struct wlr_output *out, bool need);
void instance_drop(struct vidow_blur_slot *sl);
void own_bd_drop(struct vidow_fx_target *fx);
void blur_own_drop(struct vidow_fx_target *fx);
void blur_target_release(struct vidow_fx_target *fx);
void blur_assign(struct vidow_fx_target *fx, struct wlr_output *out,
	struct wlr_texture *tex, bool owned, uint64_t id,
	const struct wlr_box *region);
void blur_mask_min_changed(struct vidow_fx_target *fx, double alpha_min);
void blur_drop(struct vidow_output *output);
void layer_bd_drop(struct vidow_layer_surface *ls);

/* ---------------------------------------------------------------- 调试探针 */

int blur_dump_limit(void);
void dump_buffer(struct vidow_server *server, struct wlr_buffer *buffer,
	int w, int h, const char *tag, int index);
bool ov_stats_on(void);
bool scanout_off(void);

extern uint64_t blur_frame_seq;
/* 「谁让 blur_ver 涨的」计数器（下标是调用点行号，见 vidow_fx_dump_blur_ver_hits）。 */
extern int blur_ver_hits[6400];
extern uint64_t ov_n, ov_ns;

#endif
