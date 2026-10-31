/* layer surface 背后的模糊：区域怎么算、那一小块底衬怎么抓。
 *
 * 消费者是 fx.c 的 vidow_blur_update（每帧遍历这块屏上的 layer，调
 * layer_blur_frame），以及弹出物那一层（它用同一套区域判据）。
 *
 * 这一块**只管「区域」和「底衬」**：要不要模糊、抓多大、抓哪一块。模糊链本身
 * 在 fx.c 的基础设施里（fx-internal.h），往上画成场景节点也在调用方。 */
#include "server.h"
#include "fx-internal.h"
#include "fx-layer.h"
#include "fx-gl.h"

#include <drm_fourcc.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/* ------------------------------------------------------- layer 的背景模糊 */

/* 状态栏 / 面板这些 layer surface 背后的模糊。
 *
 * 做法和窗口不一样：窗口是把内容和装饰一起画进一张离屏图，再让场景节点画那张图；
 * layer surface 的内容渲染归 wlroots 的 scene 管（subsurface 树、buffer scale、
 * damage 都在它手里），接管过来等于重写一遍。所以在它**下面垫一层**就够 —— 一个
 * 和它一样大的场景节点，里面只有模糊。客户端半透明的地方透出来就是模糊的底衬，
 * 不透明的地方把它盖住，等于没画。
 *
 * 底衬抓的是「本来会画在它底下的东西」（跟随 niri：它就地抓 framebuffer 里已经
 * 画好的部分），也就是这个 layer 盖住的内容：
 *   - bottom 层的 surface 先于窗口画，它底下只有壁纸和 background 层 —— 正好是共享
 *     的那张底衬，直接拿来用，一次抓取都不用（dock 那种）。
 *   - top / overlay 层反过来：窗口就在它底下，模糊里**得有窗口**。这时按它自己那
 *     一小块区域重抓一份：基座是从共享底衬上 1:1 拷过来的那块（壁纸 + background /
 *     bottom 层），窗口再画上去。代价跟这块区域的面积成正比，不是整屏。 */
/* 在子树里找第一个挂着 wlr_scene_surface 的 buffer 节点。 */
static struct wlr_scene_surface *find_scene_surface(struct wlr_scene_node *node) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		return wlr_scene_surface_try_from_buffer(
			wlr_scene_buffer_from_node(node));
	}
	if (node->type != WLR_SCENE_NODE_TREE) {
		return NULL;
	}
	struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		struct wlr_scene_surface *ss = find_scene_surface(child);
		if (ss != NULL) {
			return ss;
		}
	}
	return NULL;
}

/* 找这块 layer 的 wlr_scene_surface。wlr_scene_layer_surface_v1 只把 tree 暴露出来，
 * 表面在那棵树**里面**（wlroots 0.20 给它套了一层 subsurface 树，不是直接子节点），
 * 旁边还躺着我们自己垫的那张模糊节点的 buffer —— 所以要递归找、且要认得出哪个才是
 * 客户端的表面（find_scene_surface 只认挂得起 scene_surface 的）。 */
static struct wlr_scene_surface *layer_scene_surface(
		struct vidow_layer_surface *ls) {
	if (ls->scene == NULL || ls->scene->tree == NULL) {
		return NULL;
	}
	struct wlr_scene_node *node;
	wl_list_for_each(node, &ls->scene->tree->children, link) {
		struct wlr_scene_surface *ss = find_scene_surface(node);
		if (ss != NULL) {
			return ss;
		}
	}
	return NULL;
}

/* 这块 layer 的模糊看得见吗 —— 和窗口那条路同一个道理（见 view_blur_visible）：
 * 内容整块不透明的状态栏 / 面板，底下垫一层模糊一个像素都透不出来（装饰着色器里
 * 模糊被 content.a 乘成 0），那趟私有底衬抓取 + kawase 链条全是白算的。
 *
 * 判据保守：拿不准就当看得见。 */
bool layer_blur_visible(struct vidow_layer_surface *ls) {
	/* 入场 / 退场动画期间内容在淡入淡出（layer.c 把 anim_alpha 乘在内容上）。
	 * 淡到 0 就是完全看不见：底下垫的模糊一个像素都透不出来。 */
	if (ls->anim_alpha <= 0.0) {
		return false;
	}
	if (ls->anim != NULL || ls->anim_alpha < 1.0) {
		return true;
	}
	/* 图层规则写了 opacity：内容被画淡，模糊从里面透出来。0 同上是白烧。 */
	double rule_opacity = vidow_rule_opacity(&ls->rule_eff);
	if (rule_opacity <= 0.0) {
		return false;
	}
	if (rule_opacity < 1.0) {
		return true;
	}
	/* 抓图遮挡正盖着这一块（画出来的不是客户端的内容）。 */
	if (ls->mask_opacity) {
		return true;
	}
	struct wlr_scene_surface *ss = layer_scene_surface(ls);
	if (ss == NULL || ss->buffer == NULL || ss->buffer->buffer == NULL) {
		return true;
	}
	if (wlr_buffer_is_opaque(ss->buffer->buffer)) {
		return false; /* 格式里没 alpha（XRGB / RGBX）：整块 alpha == 1 */
	}
	/* 客户端声明了盖住整块 layer 的 opaque region（坐标是表面局部，wlroots 按
	 * 表面尺寸设的）。只声明了一部分的（圆角、留白）照旧算模糊。 */
	pixman_box32_t want = {
		.x1 = 0, .y1 = 0,
		.x2 = ls->geom.width, .y2 = ls->geom.height,
	};
	return pixman_region32_contains_rectangle(&ss->buffer->opaque_region,
		&want) != PIXMAN_REGION_IN;
}

/* **内容包围盒**：合成器自己把这块面板的缓冲读回来，量出「真正画出东西」
 * 的那一块。返回 -1 = 没测成（照旧画模糊），1 = 一个像素都没有，0 = 有内容。
 *
 * 为什么要有：vshell 的 bar / dock 是「给菜单预留几百像素」的透明画布 ——
 * bar 1920x438 里只有顶上 38px 有内容、dock 1920x372 里只有 56px。它的申报
 * （input region / ext-background-effect）就是整块画布，照它抓底衬 + 跑
 * Kawase 金字塔，DP-2 上是 3840x876 一块 49MB、每帧白算 5~11 倍的量。
 *
 * 判据是合成器自己渲染出来的像素，不信客户端申报（用户原则）。看的是
 * **alpha 通道**：ARGB 缓冲里 alpha=0 的像素照样可以有非零 RGB（清屏色、
 * 上一帧残色），把它们当内容的话包围盒永远等于整块。
 *
 * 阈值 8/255（≈3%）：着色器按 blur_alpha_min（默认 0.15）剔，这里松一档 ——
 * 宁可盒子大一点，不能把真的要画的地方裁掉。采样每 32 像素一跳，命中点前后
 * 最多差 31 像素，盒子再往外补一个步长。
 *
 * 二分开关：/tmp/vidow-no-alpha-scan 存在就整个跳过（不读回、永远「没测成」），
 * 用来现场确认「画面上的怪东西是不是这次读回引起的」。 */
static int layer_content_scan(struct vidow_server *server,
		struct vidow_layer_surface *ls) {
	if (access("/tmp/vidow-no-alpha-scan", F_OK) == 0) {
		return -1;
	}
	if (ls->layer_surface == NULL || ls->layer_surface->surface == NULL ||
			ls->layer_surface->surface->buffer == NULL) {
		return -1;
	}
	/* 借 wlr_client_buffer 自己那张贴图（dmabuf 客户端每帧重画同一个缓冲，
	 * 另建一张源图导出的一直是第一次那版 —— 见 client_texture 的说明）。
	 * 借来的不许 destroy，owned 标记决定。 */
	bool owned = false;
	struct wlr_texture *tex = client_texture(server->renderer,
		&ls->layer_surface->surface->buffer->base, &owned);
	if (tex == NULL) {
		return -1;
	}
	uint32_t format = wlr_texture_preferred_read_format(tex);
	int w = tex->width, h = tex->height;
	/* 没有 alpha 通道（XRGB）：内容必然可见，不读回。 */
	if (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_ABGR8888 &&
			format != DRM_FORMAT_RGBA8888 && format != DRM_FORMAT_BGRA8888) {
		if (getenv("VIDOW_DMG_WHY") != NULL) {
			wlr_log(WLR_INFO, "ALPHASCAN ns=%s fmt=%#x %dx%d -> 不可读",
				ls->layer_surface->namespace ?: "?", format, w, h);
		}
		if (owned) {
			wlr_texture_destroy(tex);
		}
		return format == 0 ? -1 : 0;
	}
	size_t stride = (size_t)w * 4;
	void *data = malloc(stride * (size_t)h);
	if (data == NULL) {
		if (owned) {
			wlr_texture_destroy(tex);
		}
		return -1;
	}
	struct wlr_texture_read_pixels_options opts = {
		.data = data,
		.format = format,
		.stride = (uint32_t)stride,
		.src_box = { .x = 0, .y = 0, .width = w, .height = h },
	};
	bool ok = wlr_texture_read_pixels(tex, &opts);
	if (owned) {
		wlr_texture_destroy(tex);
	}
	if (!ok) {
		free(data);
		return -1;
	}
	/* alpha 在哪个字节：ARGB / ABGR 在最高位，RGBA / BGRA 在最低位。 */
	uint32_t alpha_shift = 24;
	uint32_t alpha_mask = 0xff000000u;
	if (format == DRM_FORMAT_RGBA8888 || format == DRM_FORMAT_BGRA8888) {
		alpha_shift = 0;
		alpha_mask = 0x000000ffu;
	}
	const uint32_t visible_alpha = 8;
	int invisible = 1;
	int bx0 = 0x3fffffff, by0 = 0x3fffffff, bx1 = -1, by1 = -1;
	for (int y = 0; y < h; y += 32) {
		const uint32_t *row = (const uint32_t *)((const uint8_t *)data +
			(size_t)y * stride);
		for (int x = 0; x < w; x += 32) {
			uint32_t a = (row[x] & alpha_mask) >> alpha_shift;
			if (a == 0) {
				continue;
			}
			invisible = 0;
			if (a < visible_alpha) {
				continue;
			}
			if (x < bx0) {
				bx0 = x;
			}
			if (y < by0) {
				by0 = y;
			}
			if (x > bx1) {
				bx1 = x;
			}
			if (y > by1) {
				by1 = y;
			}
		}
	}
	free(data);
	if (invisible || bx1 < 0) {
		/* 全透明：盒子没意义，清掉（下次有内容要从整块重新算）。
		 *
		 * bx1 < 0 但 !invisible：有 alpha 但都低于门槛 —— 照样要画模糊，
		 * 只是盒子不可信，退回整块。别返回 1（那是「整块跳过」的意思，
		 * 会让这块面板的模糊反复删掉又建回来）。 */
		ls->content_w = 0;
		ls->content_h = 0;
		return invisible ? 1 : 0;
	}
	bx0 -= 32;
	by0 -= 32;
	bx1 += 32;
	by1 += 32;
	if (bx0 < 0) {
		bx0 = 0;
	}
	if (by0 < 0) {
		by0 = 0;
	}
	if (bx1 > w) {
		bx1 = w;
	}
	if (by1 > h) {
		by1 = h;
	}
	/* 缓冲像素 -> 逻辑（surface 局部）：geom 就是这块 surface 的逻辑尺寸。 */
	double sx = (double)ls->geom.width / (double)w;
	double sy = (double)ls->geom.height / (double)h;
	int cx = (int)floor((double)bx0 * sx);
	int cy = (int)floor((double)by0 * sy);
	int cx2 = (int)ceil((double)bx1 * sx);
	int cy2 = (int)ceil((double)by1 * sy);
	if (cx2 > ls->geom.width) {
		cx2 = ls->geom.width;
	}
	if (cy2 > ls->geom.height) {
		cy2 = ls->geom.height;
	}
	if (cx2 > cx && cy2 > cy) {
		ls->content_x = cx;
		ls->content_y = cy;
		ls->content_w = cx2 - cx;
		ls->content_h = cy2 - cy;
	}
	if (getenv("VIDOW_DMG_WHY") != NULL) {
		wlr_log(WLR_INFO, "ALPHASCAN ns=%s buf=%dx%d box=%d,%d %dx%d geom=%dx%d",
			ls->layer_surface->namespace ?: "?", w, h, ls->content_x,
			ls->content_y, ls->content_w, ls->content_h,
			ls->geom.width, ls->geom.height);
	}
	return 0;
}

/* 读回排到事件循环空闲时跑（见下面 layer_content_invisible 的说明）。 */
static void layer_content_idle(void *data) {
	struct vidow_layer_surface *ls = data;
	ls->content_idle = NULL;
	if (ls->layer_surface == NULL || ls->server == NULL) {
		return;
	}
	int old_verdict = ls->content_invisible;
	int old_x = ls->content_x, old_y = ls->content_y;
	int old_w = ls->content_w, old_h = ls->content_h;
	ls->content_dirty = false;
	ls->content_invisible = layer_content_scan(ls->server, ls);
	ls->content_check_ns = vidow_now_ns();
	/* **只有结果真的变了才推帧。** 读回本身每 500ms 跑一次是便宜的，但
	 * gen_scene++ 会让所有底衬（窗口的、每块面板的）整体作废、下一帧全价
	 * 重抓 —— 读回结果没变还推，就是「空闲时也每 500ms 整屏重画」。 */
	if (ls->content_invisible != old_verdict || ls->content_x != old_x ||
			ls->content_y != old_y || ls->content_w != old_w ||
			ls->content_h != old_h) {
		ls->server->gen_scene++;
		if (ls->output != NULL) {
			vidow_output_schedule_frame(ls->server, ls->output->wlr_output,
				"layer 内容判定");
		}
	}
}

/* 内容是不是一个像素都没有（模糊整块跳过）。带 200ms 节流，读回走 idle。
 *
 * **读回绝不能跑在渲染路径里。** wlr_texture_read_pixels 自己会 begin 一趟
 * render pass（GLES2 上还要建临时 FBO），在渲染途中干这个会把后面要画的东西
 * 搅乱 —— 2026-10-31 现场：「趟」从 26~38 涨到 608~966 / 500ms，模糊
 * 0.14ms/帧 → 4.7ms/帧，画面跟着花。所以这里只排 idle，渲染路径只用上一次的
 * 结果（-1 = 还没测过，照旧画，不闪）。
 *
 * 节流不看 content_invisible 是不是 -1：commit 会把它置 -1 当「未测」，而
 * vshell 每秒交几十次帧 —— 那等于每次 commit 都读一遍。 */
static bool layer_content_invisible(struct vidow_server *server,
		struct vidow_layer_surface *ls) {
	if (ls->content_dirty && ls->content_idle == NULL &&
			vidow_now_ns() - ls->content_check_ns >= 200000000LL &&
			server->display != NULL) {
		ls->content_idle = wl_event_loop_add_idle(
			wl_display_get_event_loop(server->display),
			layer_content_idle, ls);
	}
	if (ls->content_invisible < 0) {
		return false; /* 还没测过：别闪，照旧画 */
	}
	if (ls->content_invisible == 1 && ls->blur_fail != 16) {
		ls->blur_fail = 16;
		wlr_log(WLR_INFO, "layer %s: 内容全透明，模糊跳过（200ms 复查）",
			ls->layer_surface != NULL &&
				ls->layer_surface->namespace != NULL ?
				ls->layer_surface->namespace : "?");
	} else if (ls->content_invisible == 0 && ls->blur_fail == 16) {
		ls->blur_fail = 0;
	}
	return ls->content_invisible == 1;
}

bool layer_blur_wanted(struct vidow_server *server,
		struct vidow_layer_surface *ls) {
	if (!server->shader_ok || !server->config.blur_enabled) {
		return layer_blur_bail(ls, 10, "全局模糊关了 / 着色器没就绪");
	}
	/* 图层规则 blur = false：这块 layer 底下不垫模糊（全局开关仍然是总闸）。 */
	if (ls->rule_eff.set[VRA_BLUR] && !ls->rule_eff.val[VRA_BLUR].b) {
		return layer_blur_bail(ls, 11, "图层规则 blur = false");
	}
	/* 壁纸自己（background 层）不模糊：模糊的底衬里就有它，垫一层等于画重。 */
	if (ls->layer == ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND) {
		return layer_blur_bail(ls, 12, "这是 background 层（壁纸自己）");
	}
	/* **场景树被关掉的（全屏窗口自动隐藏）不模糊**：节点关了，内容一个像素
	 * 都不出现在屏上，模糊垫了也看不见 —— 可客户端还在交帧，模糊循环照样
	 * 跑全价的抓取 + 金字塔。树开着才算「看得见」。 */
	if (ls->scene != NULL && !ls->scene->tree->node.enabled) {
		return layer_blur_bail(ls, 15, "场景树被关（全屏自动隐藏中）");
	}
	/* **内容全透明的不糊**（合成器自己判定，不信客户端申报）：树开着、几何
	 * 正常，但当前这次提交里一个不透明像素都没有（滑出式 dock / opacity=0
	 * 的 OSD），垫模糊纯白烧。 */
	if (layer_content_invisible(server, ls)) {
		return layer_blur_bail(ls, 16, "内容全透明（客户端自己藏了内容）");
	}
	if (!(ls->geom.width > 0 && ls->geom.height > 0)) {
		return layer_blur_bail(ls, 13, "几何还是 0（没提交过 / 还没摆）");
	}
	if (ls->scene == NULL) {
		return layer_blur_bail(ls, 1, "没有场景节点");
	}
	if (!layer_blur_visible(ls)) {
		return layer_blur_bail(ls, 14, "整块不透明（缓冲没有 alpha，或 opaque region 盖满整块）");
	}
	return true;
}

/* 私有底衬那块区域按这个格对齐（设备像素）：窗口 / layer 在格子里挪动时区域一个字
 * 都不变，缓存才真的能命中 —— 否则拖窗口时区域每帧跟着平移，每帧都得重抓。 */

/* 这两个定义在本文件下面（模糊那一段）；layer 的区域计算要先用到。 */
int blur_levels_for(struct vidow_server *server, int w, int h);
int blur_reach_px(struct vidow_server *server, int levels);

/* 这块 layer 真正需要模糊的那一块（设备像素，相对这块屏）。
 *
 * **别再按整块 geom 抓。** quickshell 的 bar / dock 是「给菜单预留几百像素」的
 * 透明画布：实测 bar 1920x438 里只有顶上 38px 有内容、dock 1920x372 里只有
 * 56px —— 按整块算就是把抓取、kawase 金字塔和显存一起放大 5~11 倍（DP-2 是
 * 2 倍屏：3840x876 → 49MB 一块，两块加起来近百兆）。判据按可信度从高到低：
 *
 *   1. 客户端用 ext-background-effect 自己声明的模糊区域 —— 协议就是为这个
 *      存在的，以前只有窗口那条路读它（见 view.c），layer 这条路根本没看；
 *   2. 否则取 surface 的 input region 外接框。quickshell 的 mask 就落在这里
 *      （自己写的「多出来的部分不绘制也不吃鼠标」），它比 opaque region 可靠：
 *      透明画布通常压根不声明 opaque region；
 *   3. 都没有（空区域 / 还没提交过）：退回整块 geom，跟以前一样。
 *
 * 再往外放一圈模糊半径（链要采到区域外的像素），原点对齐到 128 的网格 ——
 * 和窗口那条路（view_region_rect）同一个理由：区域在格子里挪动时缓冲尺寸不变，
 * 金字塔那几块 GBM 缓冲才能一直复用。 */
bool layer_region_rect(struct vidow_server *server,
		struct vidow_output *output, struct vidow_layer_surface *ls,
		int *rx, int *ry, int *rw, int *rh, int *levels) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	struct wlr_box box = ls->geom; /* 布局坐标 */
	if (box.width <= 0 || box.height <= 0) {
		return false;
	}

	/* 可见部分（surface 局部坐标），默认整块。 */
	struct wlr_box vis = { .x = 0, .y = 0,
		.width = box.width, .height = box.height };
	struct wlr_box want;
	bool have = false;
	if (ls->layer_surface != NULL &&
			vidow_bgeffect_blur_box(server,
				ls->layer_surface->surface, &want)) {
		have = true; /* 客户端自己说的，最准 */
	} else if (ls->layer_surface != NULL) {
		pixman_box32_t *e = pixman_region32_extents(
			&ls->layer_surface->surface->input_region);
		if (e != NULL && e->x2 > e->x1 && e->y2 > e->y1) {
			want = (struct wlr_box){ .x = e->x1, .y = e->y1,
				.width = e->x2 - e->x1, .height = e->y2 - e->y1 };
			have = true;
		}
	}
	if (have) {
		struct wlr_box inter;
		if (wlr_box_intersection(&inter, &want, &vis)) {
			vis = inter;
		}
	}
	/* **再用合成器自己读回的内容包围盒收一圈**（见 layer_content_scan）：
	 * 客户端声明的区域只是上界 —— vshell 的 bar / dock 给的是整块预留画布
	 * （1920x438 / 1920x372），真正画出来的只有顶上那几十像素。照它抓底衬
	 * 就是白付 5~11 倍的抓取 + 金字塔。
	 *
	 * 盒子 0（还没测过 / 读回失败）就照旧走上面那条；盒子跟声明区域求交，
	 * 不让任何一方单独把要画的地方裁掉。 */
	if (ls->content_w > 0 && ls->content_h > 0) {
		struct wlr_box c = { .x = ls->content_x, .y = ls->content_y,
			.width = ls->content_w, .height = ls->content_h };
		struct wlr_box inter;
		if (wlr_box_intersection(&inter, &c, &vis)) {
			vis = inter;
		}
	}
	int out_x = (int)lround((double)box.x * scale);
	int out_y = (int)lround((double)box.y * scale);
	/* 这块屏自己在布局里的原点（设备像素），不是 server->output_x/y。 */
	int my_out_x = (int)lround((double)output->box.x * scale);
	int my_out_y = (int)lround((double)output->box.y * scale);
	int my_out_w = output->wlr_output->width;
	int my_out_h = output->wlr_output->height;

	int lv = blur_levels_for(server,
		(int)lround((double)vis.width * scale),
		(int)lround((double)vis.height * scale));
	int margin = blur_reach_px(server, lv);
	int left = out_x + (int)lround((double)vis.x * scale) - my_out_x - margin;
	int top = out_y + (int)lround((double)vis.y * scale) - my_out_y - margin;
	int span_w = (int)lround((double)vis.width * scale) + 2 * margin;
	int span_h = (int)lround((double)vis.height * scale) + 2 * margin;
	const int grid = VIDOW_BLUR_REGION_GRID;
	int gx1 = (int)floor((double)left / grid) * grid;
	int gy1 = (int)floor((double)top / grid) * grid;
	int size_w = (int)ceil((double)(span_w + grid) / grid) * grid;
	int size_h = (int)ceil((double)(span_h + grid) / grid) * grid;
	if (size_w < 1 || size_h < 1 || size_w > MAX_TARGET_PX ||
			size_h > MAX_TARGET_PX) {
		return false;
	}
	int max_x = my_out_w - size_w;
	int max_y = my_out_h - size_h;
	if (max_x < 0) {
		max_x = 0;
	}
	if (max_y < 0) {
		max_y = 0;
	}
	if (gx1 < 0) {
		gx1 = 0;
	} else if (gx1 > max_x) {
		gx1 = max_x;
	}
	if (gy1 < 0) {
		gy1 = 0;
	} else if (gy1 > max_y) {
		gy1 = max_y;
	}
	*rx = gx1;
	*ry = gy1;
	*rw = size_w;
	*rh = size_h;
	*levels = lv;
	return true;
}

/* 同一帧里那些要抓底衬的 layer，区域有没有压在一起 —— 压在一起才值得共享
 * （见 server.h 的 under_share）。
 *
 * 判定用「并集包围盒的面积 <= 各自面积之和」：两块区域重叠时并集不会比加起来
 * 大多少，共享省下的是重复的窗口绘制；bar 在上、dock 在下这种分离布局，并集
 * 包围盒比各自加起来的几倍还大，等于把两次小抓取换成一次大抓取，纯亏。
 *
 * 并集里每一块都**先夹进屏内**：后面抓取要拿它当 src_box，超出屏的框会让
 * wlroots 的 render pass 断言越界（也采不到东西）。 */
void layer_under_plan(struct vidow_server *server,
		struct vidow_output *output) {
	output->under_share = false;
	if (output->backdrop_tex == NULL) {
		return;
	}
	/* A/B 探针：文件一出现就走老路（各层各自抓），拿来比对同一画面。 */
	if (access("/tmp/vidow-no-layer-share", F_OK) == 0) {
		return;
	}
	int out_w = output->wlr_output->width;
	int out_h = output->wlr_output->height;
	struct wlr_box uni = {0};
	int64_t sum = 0;
	int n = 0;
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->output != output ||
				ls->layer == ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM ||
				!layer_blur_wanted(server, ls)) {
			continue;
		}
		int rx, ry, rw, rh, lv;
		if (!layer_region_rect(server, output, ls, &rx, &ry, &rw, &rh, &lv)) {
			continue;
		}
		int x1 = rx < 0 ? 0 : rx, y1 = ry < 0 ? 0 : ry;
		int x2 = rx + rw, y2 = ry + rh;
		if (x2 > out_w) {
			x2 = out_w;
		}
		if (y2 > out_h) {
			y2 = out_h;
		}
		if (x2 <= x1 || y2 <= y1) {
			continue; /* 整块在屏外 */
		}
		blur_box_add(&uni, &(struct wlr_box){ .x = x1, .y = y1,
			.width = x2 - x1, .height = y2 - y1 });
		sum += (int64_t)(x2 - x1) * (y2 - y1);
		n++;
	}
	if (n < 2) {
		return;
	}
	if ((int64_t)uni.width * uni.height > sum) {
		return;
	}
	output->under_share = true;
	output->under_box = uni;
}

/* 这一帧的共享抓取：壁纸底衬 + 窗口，一次画完。同一帧里后面的层直接裁它。
 *
 * 和 layer_region_build 默认那条路画的东西**逐像素一样**（同样的 backdrop_tex
 * 作基座、同样的窗口树、同样的 clip），区别只是画一次还是画 N 次。 */
static bool layer_under_ensure(struct vidow_server *server,
		struct vidow_output *output) {
	if (!output->under_share) {
		return false;
	}
	if (output->under_seq == blur_frame_seq && output->under_tex != NULL) {
		return true;
	}
	struct wlr_box b = output->under_box;
	if (b.width < 1 || b.height < 1) {
		return false;
	}
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	if (output->under_bd == NULL || output->under_bd->width != b.width ||
			output->under_bd->height != b.height) {
		if (output->under_tex != NULL) {
			stat_tex_del(output->under_tex);
			output->under_tex = NULL;
		}
		if (output->under_bd != NULL) {
			wlr_buffer_drop(output->under_bd);
			output->under_bd = NULL;
		}
		output->under_bd = vidow_alloc_target(server, b.width, b.height);
		if (output->under_bd == NULL) {
			return false;
		}
		output->under_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, output->under_bd));
		if (output->under_tex == NULL) {
			wlr_buffer_drop(output->under_bd);
			output->under_bd = NULL;
			return false;
		}
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, output->under_bd, NULL);
	if (pass == NULL) {
		return false;
	}
	struct wlr_render_rect_options clear = {
		.box = { .x = 0, .y = 0, .width = b.width, .height = b.height },
		.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	};
	wlr_render_pass_add_rect(pass, &clear);

	float one = 1.0f;
	struct wlr_render_texture_options base = {
		.texture = output->backdrop_tex,
		.src_box = {
			.x = (float)b.x, .y = (float)b.y,
			.width = (float)b.width, .height = (float)b.height,
		},
		.dst_box = { .x = 0, .y = 0,
			.width = b.width, .height = b.height },
		.alpha = &one,
		.transform = WL_OUTPUT_TRANSFORM_NORMAL,
		.filter_mode = WLR_SCALE_FILTER_NEAREST,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	};
	wlr_render_pass_add_texture(pass, &base);

	struct backdrop_ctx ctx = {
		.pass = pass,
		.server = server,
		.scale = scale,
		.ox = (int)lround((double)output->box.x * scale) + b.x,
		.oy = (int)lround((double)output->box.y * scale) + b.y,
		.clip_x = 0,
		.clip_y = 0,
		.clip_w = b.width,
		.clip_h = b.height,
		/* **面板自己不能进自己的底衬**：这张图是「我底下是什么」，层号
		 * 0 起全都不要 —— 包括 bar / dock / osd 自己和它们的模糊节点。
		 * 以前整趟遍历把面板自己画进去，半透明处透出「自己的糊版」，
		 * 窗口一动重抓一次、每版都不一样（用户：「纹理全脏」）。
		 *
		 * 用这一个字段挑，而不是 layer_blur_capture_visible() 去开关场景
		 * 节点：开关节点每次都记一笔损伤，面板区域于是永远有损伤、永远
		 * 重画 —— 那就变成「每帧整屏脏」。 */
		.below_layer = 0,
	};
	struct wlr_scene_tree *trees[] = {
		server->layer_toplevels,
		server->layer_popups,
	};
	for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
		wlr_scene_node_for_each_buffer(&trees[i]->node, backdrop_iter, &ctx);
	}
	if (!wlr_render_pass_submit(pass)) {
		return false;
	}
	output->under_seq = blur_frame_seq;
	vidow_stats.under++;
	return true;
}

/* 抓一块「这个 layer 盖住的东西」，含窗口。
 *
 * 只抓它自己那块区域（rx, ry 是它在输出里的位置，设备像素）：基座从共享底衬上
 * 1:1 拷一块过来，再把窗口画上去 —— 壁纸那么大的贴图不用每帧重新上传，代价跟区域
 * 面积成正比而不是整屏。区域外没有像素可抓，所以只抓屏内那一块（缓冲仍按整块
 * layer 分配，屏内那块画在自己的偏移上），采样坐标由 fx->blur_tex_* 负责对齐。
 *
 * partial != NULL：只有这块（**区域局部设备像素**）的内容变了，其余像素保留上一版
 * —— 抓也只抓这一块、链条也只重跑这一块（和窗口那条路 view_region_build 一样）。
 * 以前 layer 这条路没有局部更新：面板底下一条 20px 的进度条在动，整块 3840x876
 * 的抓取 + 5 趟 kawase 每帧重跑一遍。 */

bool layer_region_build(struct vidow_server *server,
		struct vidow_output *output, struct vidow_layer_surface *ls,
		int rx, int ry, int rw, int rh, int levels,
		const struct wlr_box *partial) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int out_w = output->wlr_output->width;
	int out_h = output->wlr_output->height;
	int px = 0, py = 0, pw = rw, ph = rh;
	if (partial != NULL) {
		px = partial->x < 0 ? 0 : partial->x;
		py = partial->y < 0 ? 0 : partial->y;
		int px2 = partial->x + partial->width;
		int py2 = partial->y + partial->height;
		if (px2 > rw) {
			px2 = rw;
		}
		if (py2 > rh) {
			py2 = rh;
		}
		if (px2 <= px || py2 <= py) {
			return false;
		}
		pw = px2 - px;
		ph = py2 - py;
	}
	int x1 = rx < 0 ? 0 : rx, y1 = ry < 0 ? 0 : ry;
	int x2 = rx + rw, y2 = ry + rh;
	if (x2 > out_w) {
		x2 = out_w;
	}
	if (y2 > out_h) {
		y2 = out_h;
	}
	if (x2 <= x1 || y2 <= y1) {
		return false; /* 整块在屏外，没东西可抓 */
	}
	if (partial != NULL) {
		/* 要抓的那一块也得夹进屏内（区域原点可能被夹过）。 */
		int cx1 = rx + px, cy1 = ry + py;
		int cx2 = cx1 + pw, cy2 = cy1 + ph;
		if (cx1 < x1) {
			cx1 = x1;
		}
		if (cy1 < y1) {
			cy1 = y1;
		}
		if (cx2 > x2) {
			cx2 = x2;
		}
		if (cy2 > y2) {
			cy2 = y2;
		}
		if (cx2 <= cx1 || cy2 <= cy1) {
			return false;
		}
		px = cx1 - rx;
		py = cy1 - ry;
		pw = cx2 - cx1;
		ph = cy2 - cy1;
	}

	bool use_under = layer_under_ensure(server, output);
	if (use_under) {
		/* **共享这一路完全不碰 ls->bd。**
		 *
		 * 底衬直接从「layer 之下」那张屏大的图里拷一块过来，窗口不用再画
		 * 一遍 —— 这就是省下来的那部分。 */
	}
	/* 基座在屏幕上的那一块（夹进屏内）。两条路都用这几个数：老路当底衬缓冲
	 * 里的源框 / 窗口裁剪框，共享路当共享图里的采样框。 */
	int bx1 = rx + px, by1 = ry + py;
	if (bx1 < x1) {
		bx1 = x1;
	}
	if (by1 < y1) {
		by1 = y1;
	}
	int bx2 = bx1 + pw, by2 = by1 + ph;
	if (bx2 > x2) {
		bx2 = x2;
	}
	if (by2 > y2) {
		by2 = y2;
	}

	if (ls->bd == NULL || ls->bd_w != rw || ls->bd_h != rh) {
		layer_bd_drop(ls);
		ls->bd = vidow_alloc_target(server, rw, rh);
		if (ls->bd == NULL) {
			return false;
		}
		ls->bd_tex = stat_tex_new(
			wlr_texture_from_buffer(server->renderer, ls->bd));
		if (ls->bd_tex == NULL) {
			layer_bd_drop(ls);
			return false;
		}
		ls->bd_w = rw;
		ls->bd_h = rh;
	}
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
		server->renderer, ls->bd, NULL);
	if (pass == NULL) {
		return false;
	}
	{
		struct wlr_render_rect_options clear = {
			.box = { .x = px, .y = py, .width = pw, .height = ph },
			.color = { .r = 0.0f, .g = 0.0f, .b = 0.0f, .a = 0.0f },
			.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
		};
		wlr_render_pass_add_rect(pass, &clear);

		/* 基座：共享图（layer 之下，屏大）或这块屏的壁纸底衬里 1:1 捞一块。
		 * 源框必须夹在纹理里面（wlroots 的 render pass 会断言越界）。 */
		int usx = use_under ? output->under_box.x : 0;
		int usy = use_under ? output->under_box.y : 0;
		int dw = bx2 - bx1, dh = by2 - by1;
		float one = 1.0f;
		struct wlr_render_texture_options base = {
			.texture = use_under ? output->under_tex : output->backdrop_tex,
			.src_box = {
				.x = (float)(bx1 - usx), .y = (float)(by1 - usy),
				.width = (float)(bx2 - bx1), .height = (float)(by2 - by1),
			},
			.dst_box = {
				.x = bx1 - rx, .y = by1 - ry,
				.width = dw, .height = dh,
			},
			.alpha = &one,
			.transform = WL_OUTPUT_TRANSFORM_NORMAL,
			.filter_mode = WLR_SCALE_FILTER_NEAREST,
			.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
		};
		wlr_render_pass_add_texture(pass, &base);
	}
	if (!use_under) {

		/* 窗口画在基座上：绘制顺序上它们就在 top / overlay 层底下，连窗口的
		 * 弹出物一起（窗口树 → 窗口弹出物树）。只画要抓的那一块（partial 时
		 * 就是损伤那一小块），底衬别的像素一个都不碰。 */
		struct backdrop_ctx ctx = {
			.pass = pass,
			.server = server,
			.scale = scale,
			.ox = (int)lround((double)output->box.x * scale) + x1,
			.oy = (int)lround((double)output->box.y * scale) + y1,
			.clip_x = bx1 - rx,
			.clip_y = by1 - ry,
			.clip_w = bx2 - bx1,
			.clip_h = by2 - by1,
			/* 同上：面板自己不进自己的底衬。 */
			.below_layer = 0,
		};
		struct wlr_scene_tree *trees[] = {
			server->layer_toplevels,
			server->layer_popups,
		};
		for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); i++) {
			wlr_scene_node_for_each_buffer(&trees[i]->node, backdrop_iter,
				&ctx);
		}
		if (!wlr_render_pass_submit(pass)) {
			return false;
		}
	}

	dmg_clear(&ls->fx.pending_damage); /* 这一版的账已经算清了 */
	/* 旧贴图裹着的缓冲马上要被金字塔复用。 */
	blur_own_drop(&ls->fx);
	if (!blur_set_alloc(server, &ls->fx.blur_set, rw, rh, levels)) {
		return false;
	}
	if (partial != NULL) {
		blur_chain_rect(server, &ls->fx.blur_set, ls->bd_tex, rw, rh,
			&(struct wlr_box){ .x = px, .y = py,
				.width = pw, .height = ph },
			blur_reach_px(server, levels));
	} else {
		blur_chain(server, &ls->fx.blur_set, ls->bd_tex, rw, rh);
	}
	ls->fx.blur_own_tex = ls->fx.blur_set.up_tex[0];
	if (ls->fx.blur_own_tex == NULL) {
		return false;
	}
	ls->fx.blur_own_id = server->blur_serial++;
	ls->bd_x = rx;
	ls->bd_y = ry;
	ls->bd_levels = levels;
	ls->bd_gen_scene = server->gen_scene;
	ls->bd_gen_layer = server->gen_layer;

	static int dumped = 0;
	if (dumped < blur_dump_limit()) {
		dump_buffer(server, ls->bd, rw, rh, "vidow-lbd", dumped);
		if (ls->fx.blur_set.up[0] != NULL && rw > 1 && rh > 1) {
			dump_buffer(server, ls->fx.blur_set.up[0], rw >> 1, rh >> 1,
				"vidow-lblur", dumped);
		}
		dumped++;
	}
	wlr_log(WLR_DEBUG, "blur: layer region %dx%d at %d,%d levels=%d (含窗口)",
		rw, rh, rx, ry, levels);
	return true;
}

/* 没画成的出口：记下原因，只在**原因变了**时打一行（每帧都跑，不限流会刷屏）。
 * 排查「某块屏上的 layer 没有模糊」时，这一行直接说是哪一步挡住的。 */

bool layer_blur_bail(struct vidow_layer_surface *ls, int reason,
		const char *what) {
	if (ls->blur_fail != reason) {
		ls->blur_fail = reason;
		wlr_log(WLR_DEBUG, "layer 模糊没画: %s（namespace=%s 输出=%s）",
			what,
			ls->layer_surface != NULL && ls->layer_surface->namespace != NULL ?
				ls->layer_surface->namespace : "?",
			ls->output != NULL && ls->output->wlr_output != NULL &&
				ls->output->wlr_output->name != NULL ?
				ls->output->wlr_output->name : "?");
	}
	return false;
}

/* ------------------------------------------------------------- overview 壁纸 */

/* overview 里那张糊掉的壁纸。
 *
 * 跟 bottom 层 layer surface 的底衬是同一套（共享底衬 output->blur_tex + blur_only
 * 那次装饰渲染），区别只有两点：它摊满整块屏，而且画在 layer_overview_dim 那棵树
 * 里（壁纸之上、窗口之下）—— 全景里缩小的窗口底下就是一整片糊壁纸，niri 那个味道。
 *
 * 拿不到共享底衬（没开模糊 / 这屏还没有底衬）就什么都不画，退回「只压暗」：遮罩
 * 还在，不至于变成一块没压过的壁纸。 */
void vidow_overview_blur_frame(struct vidow_server *server,
		struct vidow_output *output) {
	struct vidow_overview *g = &server->overview;
	/* **每块屏各有一份底衬**（以前是单实例、只画进 overview 时聚焦的那块屏，
	 * 别的屏上一片空白）。 */
	struct vidow_ov_screen *ov = &output->ov;
	struct vidow_fx_target *fx = &ov->fx;

	/* 每块屏画自己那份，而且得真在展开（t > 0）。 */
	bool want = g->t > 0.002 &&
		server->config.overview_blur && server->config.blur_enabled;
	if (!want || output->blur_tex == NULL) {
		if (fx->node != NULL) {
			wlr_scene_node_set_enabled(&fx->node->node, false);
		}
		ov->fx_on = false;
		return;
	}

	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	if (box.width < 1 || box.height < 1) {
		ov->fx_on = false;
		return;
	}
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int logical_w = box.width, logical_h = box.height;
	int px_w = (int)lround((double)logical_w * scale);
	int px_h = (int)lround((double)logical_h * scale);
	if (px_w < 1 || px_h < 1 || px_w > MAX_TARGET_PX || px_h > MAX_TARGET_PX) {
		ov->fx_on = false;
		return;
	}

	if (fx->node == NULL) {
		fx->node = wlr_scene_buffer_create(server->layer_overview_dim, NULL);
		if (fx->node == NULL) {
			ov->fx_on = false;
			return;
		}
		fx->node->point_accepts_input = fx_node_rejects_input;
		/* 垫在压暗遮罩下面（遮罩比它先建）。 */
		wlr_scene_node_lower_to_bottom(&fx->node->node);
	}
	fx->blur_only = true;
	fx->active = true;
	fx->blur = true;
	/* 恒等映射：这块元素就是整块屏，没有「窗口几何」那层偏移（跟 layer 底衬
	 * 一样，这一路不经过 vidow_fx_begin，字段得自己填）。 */
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;

	if (fx->buf[0] == NULL || fx->w != px_w || fx->h != px_h ||
			fabs(fx->scale - scale) > 1e-3) {
		drop_targets(fx);
		for (int i = 0; i < 2; i++) {
			fx->buf[i] = vidow_alloc_target(server, px_w, px_h);
			if (fx->buf[i] == NULL) {
				drop_targets(fx);
				ov->fx_on = false;
				return;
			}
		}
		fx->w = px_w;
		fx->h = px_h;
		fx->content_w = px_w;
		fx->content_h = px_h;
		fx->px_pad = 0;
		fx->dst_w = logical_w;
		fx->dst_h = logical_h;
		fx->scale = scale;
		fx->blur_drawn = 0; /* 尺寸变了，必须重画 */
	}
	/* region = NULL：这张贴图盖的就是整块屏，着色器按「元素在输出上的位置」
	 * 采样（u_origin = ox*scale - 输出原点），跟 bottom 层那条路一样。 */
	blur_assign(fx, output->wlr_output, output->blur_tex, false, output->blur_id, NULL);
	if (fx->blur_drawn == fx->blur_ver) {
		ov->fx_on = true; /* 已经画的就是这一版 */
		wlr_scene_node_set_enabled(&fx->node->node, true);
		return;
	}

	struct wlr_buffer *target = pick_free(fx->buf);
	if (target == NULL) {
		return; /* 两个目标都还在飞：留着上一帧那张，别闪 */
	}
	static const float no_border[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	struct vidow_look look = {0};
	look.opacity = 1.0;
	look.blur = true;
	look.decor = false;
	/* mask = NULL：整块屏都要糊，不按元素 alpha 剔（blur_only 且 mask == NULL
	 * 时着色器自己把阈值归零，见 decor_draw）。 */
	decor_draw(server, fx, target, server->empty_tex, NULL, no_border,
		box.x, box.y, 1.0, scale, &look);
	wlr_scene_node_set_position(&fx->node->node, box.x, box.y);
	wlr_scene_buffer_set_dest_size(fx->node, logical_w, logical_h);
	wlr_scene_buffer_set_opacity(fx->node, fx->masked ? 0.0f : 1.0f);
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, logical_w, logical_h);
	wlr_scene_buffer_set_buffer_with_damage(fx->node, target, &damage);
	pixman_region32_fini(&damage);
	wlr_scene_node_set_enabled(&fx->node->node, true);
	fx->blur_drawn = fx->blur_ver;
	ov->fx_on = true;
}

void layer_blur_capture_visible(struct vidow_server *server, bool on) {
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->fx.node != NULL) {
			wlr_scene_node_set_enabled(&ls->fx.node->node, on);
		}		/* 光藏模糊节点不够：**会被模糊的那个 surface 本身**也不能进底衬。
		 * bottom 层的面板（dock 这类）自己也在抓的树里，它要的那份底衬如果含它
		 * 自己，模糊里就会浮出一个自己的影子（半透明的地方透出「自己的糊版」）。
		 * 藏之前记一笔，免得回填的时候把本来就该藏着的东西点亮。 */
		if (!on) {
			if (ls->scene != NULL && layer_blur_wanted(server, ls) &&
					ls->scene->tree->node.enabled) {
				wlr_scene_node_set_enabled(&ls->scene->tree->node, false);
				ls->capture_hidden = true;
			}
		} else if (ls->capture_hidden) {
			ls->capture_hidden = false;
			wlr_scene_node_set_enabled(&ls->scene->tree->node, true);
		}
	}
}

/* 位置 / 尺寸变了（或者底衬换了）：作废已画的那一版，下一帧重画。
 * 距离和采样坐标都在着色器里，节点自己动了也得重算。 */
void vidow_layer_blur_invalidate(struct vidow_layer_surface *ls) {
	ls->fx.blur_ver++;
	blur_ver_hits[6347]++;
	if (ls->output != NULL) {
		vidow_output_schedule_frame(ls->server, ls->output->wlr_output,
			"layer 模糊失效");
	}
}

void vidow_layer_blur_release(struct vidow_layer_surface *ls) {
	if (ls->fx.node == NULL && ls->fx.buf[0] == NULL &&
			ls->bd == NULL && ls->bd_tex == NULL) {
		return;
	}
	if (ls->fx.node != NULL) {
		/* 这行是给 test-layerdestroy.sh 数数用的：layer 没了，垫在它下面的
		 * 模糊节点得当场收掉（不收就是每次面板重建漏一张整块大小的缓冲）。 */
		wlr_log(WLR_DEBUG, "layer blur release %dx%d",
			ls->geom.width, ls->geom.height);
		wlr_scene_node_destroy(&ls->fx.node->node);
		ls->fx.node = NULL;
	}
	vidow_fx_release(&ls->fx);
	ls->fx.blur_drawn = 0;
	ls->fx.blur_tex_w = 0;
	ls->fx.blur_tex_h = 0;
	/* 抓来的那块区域底衬也得跟着走，否则 layer 没了还占着一张整块大小的缓冲。 */
	layer_bd_drop(ls);
}

bool layer_blur_frame(struct vidow_server *server,
		struct vidow_output *output, struct vidow_layer_surface *ls) {
	struct vidow_fx_target *fx = &ls->fx;

	if (ls->scene == NULL) {
		return layer_blur_bail(ls, 1, "没有场景节点");
	}

	/* 恒等映射：layer 的元素就是整块缓冲，没有「窗口几何」那层偏移（窗口那一
	 * 路走 vidow_fx_frame，每帧按可见窗口覆盖这几个字段）。这一路不经过
	 * vidow_fx_begin，字段默认是 0 —— 不设就是拿 texel (0,0) 当整块用。 */
	fx->tex_off_x = 0.0f;
	fx->tex_off_y = 0.0f;
	fx->tex_scale_x = 1.0f;
	fx->tex_scale_y = 1.0f;
	fx->mask_off_x = 0.0f;
	fx->mask_off_y = 0.0f;
	fx->mask_scale_x = 1.0f;
	fx->mask_scale_y = 1.0f;
	fx->mask_min = 0.0f;
	blur_mask_min_changed(fx, server->config.blur_alpha_min);
	/* bottom 层先于窗口画，它底下没有窗口：共享的那张底衬就是它要的全部。top /
	 * overlay 层底下有窗口，得自己抓一小块（见 layer_region_build）。 */
	bool with_windows = ls->layer != ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
	if (with_windows ? output->backdrop_tex == NULL : output->blur_tex == NULL) {
		return layer_blur_bail(ls, 2, with_windows ?
			"这块屏的共享底衬（backdrop_tex）还没建" :
			"这块屏的壁纸底衬（blur_tex）还没建");
	}
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	int logical_w = ls->geom.width, logical_h = ls->geom.height;
	int px_w = (int)lround((double)logical_w * scale);
	int px_h = (int)lround((double)logical_h * scale);
	if (px_w < 1 || px_h < 1 || px_w > MAX_TARGET_PX || px_h > MAX_TARGET_PX) {
		return layer_blur_bail(ls, 3, "设备像素尺寸越界");
	}

	if (fx->node == NULL) {
		fx->node = wlr_scene_buffer_create(ls->scene->tree, NULL);
		if (fx->node == NULL) {
			return false;
		}
		fx->node->point_accepts_input = fx_node_rejects_input;
		/* 垫在 layer surface 自己那个节点下面。 */
		wlr_scene_node_lower_to_bottom(&fx->node->node);
	}
	fx->blur_only = true;
	fx->active = true;
	/* 着色器里 u_blur_strength 看的是 fx->blur（窗口那条路是 vidow_fx_begin
	 * 设的），这儿得自己开：不开就是垫了一张全透明的东西，什么都看不见。 */
	fx->blur = true;

	if (fx->buf[0] == NULL || fx->w != px_w || fx->h != px_h ||
			fabs(fx->scale - scale) > 1e-3) {
		drop_targets(fx);
		for (int i = 0; i < 2; i++) {
			fx->buf[i] = vidow_alloc_target(server, px_w, px_h);
			if (fx->buf[i] == NULL) {
				drop_targets(fx);
				return layer_blur_bail(ls, 5, "分配离屏目标失败");
			}
		}
		fx->w = px_w;
		fx->h = px_h;
		fx->content_w = px_w;
		fx->content_h = px_h;
		fx->px_pad = 0;
		fx->dst_w = logical_w;
		fx->dst_h = logical_h;
		fx->scale = scale;
		fx->blur_drawn = 0; /* 尺寸变了，必须重画 */
	}

	if (with_windows) {
		/* **区域只取真正要模糊的那一块**（见 layer_region_rect）：以前是整块
		 * geom —— quickshell 给菜单预留的透明画布也一起抓、一起跑金字塔。
		 * rx/ry 是**这块屏**的设备像素（减的是这块屏自己的布局原点，不是
		 * server->output_x/y：后者是最后渲染那块屏留下的全局量）。 */
		int rx = 0, ry = 0, rw_px = px_w, rh_px = px_h, levels = 0;
		if (!layer_region_rect(server, output, ls, &rx, &ry, &rw_px,
				&rh_px, &levels)) {
			return layer_blur_bail(ls, 6, "模糊区域算不出来（整块在屏外 / 太大）");
		}
		/* **抓的框和画的框必须是同一个。**
		 *
		 * 上面只抓了 rx/ry/rw×rh 那一块（bar 的顶栏、dock 的图标行），而着色器
		 * **不知道**这件事：u_blur_region_on 一直是 0，于是 cover 恒为 1 ——
		 * 模糊按整块 geom 铺满（bar 的 438 高预留画布、dock 的 372），区域外
		 * 采的是贴图边缘 clamp 出来的那几个像素，屏幕上就是一条条别的窗口的
		 * 颜色糊在面板上（用户 2026-10-31 说的「UI 带出别的窗口内容」）。
		 *
		 * 这里把「真抓了哪一块」告诉着色器（surface 局部逻辑坐标）：模糊只在
		 * 这块里面画，外面一个像素都不碰。 */
		int obx = (int)lround((double)output->box.x * scale);
		int oby = (int)lround((double)output->box.y * scale);
		int gx = (int)lround((double)ls->geom.x * scale) - obx;
		int gy = (int)lround((double)ls->geom.y * scale) - oby;
		fx->blur_region_on = true;
		fx->blur_region[0] = (float)(rx - gx) / (float)scale;
		fx->blur_region[1] = (float)(ry - gy) / (float)scale;
		fx->blur_region[2] = (float)rw_px / (float)scale;
		fx->blur_region[3] = (float)rh_px / (float)scale;
		/* 缓存键：区域（含输出原点，多屏下会动）、世界代次（窗口重画 / layer
		 * 变动）、级数、还有那两套直接影响结果的参数。 */
		/* 同窗口那条路：世界代次只是粗筛，损伤盒子没碰到这一块就不重抓。
		 *
		 * 碰的是**区**域那块（rx/ry/rw/rh），不是整块 geom：quickshell 的
		 * bar 画布 438 高、真正有内容的只有顶上 38px —— 拿 geom 判的话，
		 * 菜单预留区里有窗口动一下照样整块重抓，区域收窄就白做了。 */
		/* 右 / 下边界 ceil((rx+rw)/scale) 精确盖住，不多加 1 —— 理由同
		 * fx.c view_region_build：贴屏边的区域多 1px 会探进隔壁屏的布局
		 * 范围，被隔壁屏的损伤流拖着每帧重抓。 */
		int lg_x = output->box.x + (int)floor((double)rx / scale);
		int lg_y = output->box.y + (int)floor((double)ry / scale);
		int lg_r = output->box.x +
			(int)ceil((double)(rx + rw_px) / scale);
		int lg_b = output->box.y +
			(int)ceil((double)(ry + rh_px) / scale);
		struct wlr_box region_logical = {
			.x = lg_x,
			.y = lg_y,
			.width = lg_r - lg_x,
			.height = lg_b - lg_y,
		};
		bool key_changed = ls->bd_gen_scene != server->gen_scene;
		bool touched = key_changed &&
			blur_damage_hits(fx, &region_logical);
		if (getenv("VIDOW_DMG_WHY") != NULL) {
			static int64_t lz_last;
			int64_t lz_now = vidow_now_ns();
			if (lz_now - lz_last > 200000000LL) {
				lz_last = lz_now;
				wlr_log(WLR_INFO, "LAYERBLUR geom=%dx%d%+d%+d px=%dx%d "
					"touched=%d key=%d out=%s", logical_w, logical_h,
					ls->geom.x, ls->geom.y, px_w, px_h, (int)touched,
					(int)key_changed, output->wlr_output->name);
			}
		}
		/* **判据里不再看 gen_layer**（只有损伤盒子算数）。
		 *
		 * 原来这里还要求「抓的时候的 gen_layer == 现在的 gen_layer」，而
		 * gen_layer 是**全局**的：任何一次 layer 变化（上屏 / 下屏 / 壁纸换 /
		 * %background% 重画）都会让它 +1，于是屏幕上**每一块** layer 的私有
		 * 底衬一起作废、一起重抓。
		 *
		 * 代价实测：noctalia 那 8 块 32x32 的角层（noctalia-screen-corner）
		 * 每帧都在重抓 —— BLURVER 探针统计 ~2000 次/秒的 blur_ver 推进，
		 * 渲染统计里「贴图 +12/-0」的纹理反复建删就是它，静止桌面白烧 CPU。
		 *
		 * 而 gen_layer 真正想表达的那些变化**都已经记了损伤**：
		 * vidow_backdrop_damage / vidow_backdrop_damage_output 都调
		 * blur_note_damage，把整屏（或那块屏）的盒子记到每个 layer 头上 ——
		 * 所以 touched 那一条已经覆盖了它们。留下 gen_layer 只是纯churn。
		 *
		 * 换句话说：**判据必须是「我这一块底下的像素变了吗」，不是「世界
		 * 有没有动过」**。全局代次回答的是后一个问题，用它就会把「屏幕另一头
		 * 某个 32x32 角上什么都没变」也判成过期。 */
		/* 区域和缓冲尺寸都是**块感**（按 128 对齐），尺寸跟内容大小有关、跟位置
		 * 无关：移动面板不会让缓冲重建（见 layer_region_rect）。 */
		bool fresh = ls->bd != NULL && fx->blur_own_tex != NULL && !touched &&
			ls->bd_w == rw_px && ls->bd_h == rh_px &&
			ls->bd_x == rx && ls->bd_y == ry &&
			ls->bd_levels == levels &&
			fx->blur_set.w == rw_px && fx->blur_set.h == rh_px &&
			fx->blur_set.levels == levels &&
			fabs(fx->blur_set.offset - server->config.blur_offset) < 1e-6;
		if (!fresh) {
			/* 形状（区域位置尺寸 / 级数）没变：只重算受损的那一小块（见
			 * region_update_rect）。以前 layer 这条路没有局部更新，面板底下
			 * 一条 20px 的进度条在动，整块区域每帧重抓 + 5 趟 kawase。 */
			bool shape_ok = ls->bd != NULL && !touched &&
				ls->bd_w == rw_px && ls->bd_h == rh_px &&
				ls->bd_x == rx && ls->bd_y == ry &&
				ls->bd_levels == levels &&
				fx->blur_set.w == rw_px && fx->blur_set.h == rh_px &&
				fx->blur_set.levels == levels &&
				fabs(fx->blur_set.offset - server->config.blur_offset) < 1e-6;
			struct wlr_box upd;
			bool partial = shape_ok && !blur_no_partial() &&
				region_update_rect(server, fx, output, rx, ry, rw_px, rh_px,
					&upd);
			if (!layer_region_build(server, output, ls, rx, ry, rw_px,
					rh_px, levels, partial ? &upd : NULL)) {
				/* 抓不出来就留着上一帧那张，别闪 */
				return layer_blur_bail(ls, 4, "私有底衬抓不出来");
			}
			/* **底衬重抓了 = 内容变了，必须重画这一版。**
			 *
			 * 别指望下面的 blur_assign() 顺手把 blur_ver 推上去：它只在
			 * 「贴图 / id / 区域」变了时才 ++，而重抓底衬恰恰不换这两样
			 * （同一张 blur_own_tex、同一块区域，只是里面的像素是新的）。
			 * 不在这儿显式 ++ 的话，后面 blur_drawn == blur_ver 立刻成立、
			 * 直接 return true —— 底下窗口 / 壁纸变了，layer 背后的模糊还是
			 * 旧的那一张（用户报的「layer 的 blur 好像很容易不更新」）。
			 *
			 * 窗口那条路没这个问题：view_region_build 走完，blur_assign
			 * 拿到的是新的 blur_own_id（每次重抓都换号），自然会 ++。 */
			fx->blur_ver++;
			blur_ver_hits[5360]++;
		}
		blur_assign(fx, NULL, fx->blur_own_tex, false, fx->blur_own_id,
			&(struct wlr_box){ .x = rx, .y = ry,
				.width = rw_px, .height = rh_px });
	} else {
		/* bottom 层：用共享的那张。`set_layer` 把层改过来的话，之前抓的区域底衬
		 * 就没用了 —— 连着金字塔一起放掉，别攥着几 MB 不放。
		 *
		 * 加 if 守卫：这个分支**每帧**都走，无守卫就是每帧白跑一遍 6 次赋值
		 * （把已经归零的字段再写一遍）。它真正要干的只有「刚从 top/overlay
		 * 切过来」那一次。 */
		/* 共享底衬是整屏的，没有「区域外」这回事：把区域限制关掉。
		 * （从 top / overlay 切到 bottom 时不清，屏幕上就只剩顶上那一条糊。） */
		fx->blur_region_on = false;
		if (fx->blur_own_tex != NULL) {
			blur_own_drop(fx);
		}
		/* 缓冲和包着它的贴图要一起收：只 drop buffer 会把 ls->bd_tex
		 * 留成一张悬空贴图（layer 从 top/overlay 切到 bottom 时漏的就是它）。 */
		if (ls->bd != NULL || ls->bd_tex != NULL) {
			layer_bd_drop(ls);
		}
		blur_assign(fx, output->wlr_output, output->blur_tex, false, output->blur_id, NULL);
	}
	/* 采样原点必须记**这块屏自己**的（设备像素）：装饰着色器拿 out_* 和
	 * blur_tex_* 算 UV，而 server->output_* 是「最后渲染的那块屏」留下的全局
	 * 量 —— 两块屏上各有一块 bar 的时候，后渲染的那块会把原点写给先渲染的，
	 * 先渲染那块屏上的模糊就整体错一屏宽。区域收窄之后这里错一下更明显
	 * （会采到区域外面去），所以跟着一起补上。 */
	fx->blur_out_w = output->wlr_output->width;
	fx->blur_out_h = output->wlr_output->height;
	fx->blur_out_x = (int)lround((double)output->box.x * scale);
	fx->blur_out_y = (int)lround((double)output->box.y * scale);
	if (ls->blur_fail != 0) {
		ls->blur_fail = 0; /* 又能画了：下次出问题要重新报 */
	}
	if (fx->blur_drawn == fx->blur_ver) {
		return true; /* 已经画的就是这一版 */
	}

	struct wlr_buffer *target = pick_free(fx->buf);
	if (target == NULL) {
		return false;
	}
	static const float no_border[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
	/* 元素自己的内容，给透明度剔除当 mask：模糊只画在 layer surface 真的有内容的
	 * 地方，它自己透明的内边距 / 圆角外不再透出一圈模糊。取的是 surface 当前的
	 * 缓冲；层的 geom 就是 surface 的尺寸（layer.c 的 arrange 就那么算的），所以
	 * uv 跟内容那趟完全一致，不用另算映射。拿不到（还没 commit 过）就让 mask_min
	 * 归零 —— 也就是整块都糊，退回老行为而不是把模糊整块剔没。
	 *
	 * 代价：这是 surface 的**主**缓冲，不含子表面 —— 一个用子表面画内容的层，那
	 * 部分会被剔掉（少见；要精确就得像 niri 那样把整棵子树离屏渲一张 alpha 纹理）。 */
	struct wlr_texture *mask = NULL;
	struct wlr_client_buffer *cbuf = ls->layer_surface->surface->buffer;
	if (cbuf != NULL) {
		mask = wlr_texture_from_buffer(server->renderer,
			render_source(&cbuf->base));
	}
	/* VIDOW_MASK_DUMP=N：把前 N 份 mask（就是下面递给着色器的这张 layer 自己的
	 * 内容）dump 成裸数据，用来量「元素自己透明的地方到底剔没剔」。 */
	static int mask_dumped = 0;
	static int mask_limit = -1;
	if (mask_limit < 0) {
		const char *env = getenv("VIDOW_MASK_DUMP");
		mask_limit = env != NULL ? atoi(env) : 0;
	}
	if (mask != NULL && mask_dumped < mask_limit) {
		dump_buffer(server, &cbuf->base, px_w, px_h, "vidow-mask",
			mask_dumped);
		mask_dumped++;
	}
	struct vidow_look ls_look;
	vidow_rule_look(&server->config, &ls->rule_eff, &ls_look);
	decor_draw(server, fx, target, server->empty_tex, mask, no_border,
		ls->geom.x, ls->geom.y, 1.0, scale, &ls_look);
	if (mask != NULL) {
		wlr_texture_destroy(mask);
	}

	wlr_scene_node_set_position(&fx->node->node, 0, 0);
	wlr_scene_buffer_set_dest_size(fx->node, logical_w, logical_h);
	/* 同上：layer surface 也会被抓帧遮挡压着。 */
	wlr_scene_buffer_set_opacity(fx->node, fx->masked ? 0.0f : 1.0f);
	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, logical_w, logical_h);
	wlr_scene_buffer_set_buffer_with_damage(fx->node, target, &damage);
	pixman_region32_fini(&damage);
	wlr_scene_node_set_enabled(&fx->node->node, true);

	fx->blur_drawn = fx->blur_ver;
	return true;
}
