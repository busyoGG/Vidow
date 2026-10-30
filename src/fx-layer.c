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
