/* 底衬损伤账本：这一块私有底衬什么时候过期。
 *
 * 记的是「从上一次抓底衬到现在，世界里变过哪儿」（布局逻辑坐标）。消费者有三
 * 处：窗口的私有底衬、layer 背后的底衬、弹出物背后的底衬 —— 它们只需要问
 * blur_damage_hits：这些变化碰到我这一块了吗。
 *
 * 为什么要有代次（gen_scene / gen_layer）之外再加账本：代次只回答「世界动过
 * 没有」，不动哪儿；拿它判断就只能整块重抓。账本让「屏幕另一头的窗口在动」
 * 与「我底下这一块没变」区分开（见 server.h 的 struct vidow_dmg_set）。
 *
 * 也是「谁标脏的」探针（VIDOW_DMG_WHY）和 arrange 期间真实变化区域的来源。 */
#include "server.h"
#include "fx-internal.h"
#include "xtrace.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* -------------------------------------------------------------- backdrop */

/* Everything that can end up inside a blurred backdrop bumps one of these two
 * counters. Cheap, and it is what lets a still desktop skip the whole kawase
 * chain: an unchanged generation means the cached pyramid is still correct. */
/* ------------------------------------------------- 损伤区域（私有底衬的重抓判据） */

/* 两个盒子有没有重叠（宽高 <= 0 的空盒子永远不重叠）。用 64 位算，因为「整屏都
 * 算」那个盒子故意填得很大。 */
static bool blur_box_touch(const struct wlr_box *a, const struct wlr_box *b) {
	if (a->width <= 0 || a->height <= 0 || b->width <= 0 || b->height <= 0) {
		return false;
	}
	int64_t ax2 = (int64_t)a->x + a->width, ay2 = (int64_t)a->y + a->height;
	int64_t bx2 = (int64_t)b->x + b->width, by2 = (int64_t)b->y + b->height;
	return (int64_t)a->x < bx2 && (int64_t)b->x < ax2 &&
		(int64_t)a->y < by2 && (int64_t)b->y < ay2;
}

/* 把 src 并进 dst（dst 为空就直接是 src）。 */
void blur_box_add(struct wlr_box *dst, const struct wlr_box *src) {
	if (src->width <= 0 || src->height <= 0) {
		return;
	}
	if (dst->width <= 0 || dst->height <= 0) {
		*dst = *src;
		return;
	}
	int x1 = dst->x < src->x ? dst->x : src->x;
	int y1 = dst->y < src->y ? dst->y : src->y;
	int x2 = dst->x + dst->width > src->x + src->width ?
		dst->x + dst->width : src->x + src->width;
	int y2 = dst->y + dst->height > src->y + src->height ?
		dst->y + dst->height : src->y + src->height;
	*dst = (struct wlr_box){ .x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1 };
}

/* 「整屏都算」用的盒子：故意大到碰得到任何东西（不参与算术，只被 blur_box_touch
 * 比大小，那边是 64 位）。 */
static const struct wlr_box blur_box_all = {
	.x = -0x40000000, .y = -0x40000000,
	.width = 0x7fffffff, .height = 0x7fffffff,
};

static bool dmg_box_empty(const struct wlr_box *b) {
	return b->width <= 0 || b->height <= 0;
}

static int64_t dmg_box_area(const struct wlr_box *b) {
	return dmg_box_empty(b) ? 0 : (int64_t)b->width * (int64_t)b->height;
}

/* b 完全在 a 里面。 */
static bool dmg_box_inside(const struct wlr_box *a, const struct wlr_box *b) {
	return a->x <= b->x && a->y <= b->y &&
		a->x + a->width >= b->x + b->width &&
		a->y + a->height >= b->y + b->height;
}

/* 攒着的损伤区域：**不是一个包围盒**。两个客户端在同一帧里各动窗口的一角（一个
 * 在左上、一个在右下），包围盒会把中间那一大片**没变**的地方也算进去，压着它们的
 * 那个窗口底下的模糊就白重抓了 —— 损伤盒子本身做小之后，这个「并集吃掉中间」的
 * 代价反而变成了主要开销。所以留几个盒子（VIDOW_DMG_BOXES 个），装不下了才合并，
 * 合并到「并进去之后总面积最小」的那个上。 */
static void dmg_add(struct vidow_dmg_set *set, const struct wlr_box *box) {
	if (box == NULL || dmg_box_empty(box)) {
		return;
	}
	for (int i = 0; i < set->n; i++) {
		if (dmg_box_inside(&set->box[i], box)) {
			return; /* 已经算进去了 */
		}
	}
	if (set->n < VIDOW_DMG_BOXES) {
		set->box[set->n++] = *box;
		return;
	}
	int best = 0;
	int64_t best_grow = -1;
	for (int i = 0; i < set->n; i++) {
		struct wlr_box merged = set->box[i];
		blur_box_add(&merged, box);
		int64_t grow = dmg_box_area(&merged) - dmg_box_area(&set->box[i]);
		if (best_grow < 0 || grow < best_grow) {
			best_grow = grow;
			best = i;
		}
	}
	blur_box_add(&set->box[best], box);
}

/* 这块区域跟攒下来的任何一笔损伤碰不碰得到。 */
static bool dmg_touch(const struct vidow_dmg_set *set,
		const struct wlr_box *region) {
	for (int i = 0; i < set->n; i++) {
		if (blur_box_touch(&set->box[i], region)) {
			return true;
		}
	}
	return false;
}

/* 攒下来的损伤的包围盒：只给日志用（真正的判据是上面那个逐盒子的碰法）。 */
struct wlr_box dmg_bounds(const struct vidow_dmg_set *set) {
	struct wlr_box b = {0};
	for (int i = 0; i < set->n; i++) {
		blur_box_add(&b, &set->box[i]);
	}
	return b;
}

void dmg_clear(struct vidow_dmg_set *set) {
	set->n = 0;
}

/* 把「刚变了的这块区域」（布局逻辑坐标）记到每个需要私有底衬的元素头上；skip 那
 * 个元素自己不记（它自己的变化不该让它自己的底衬过期 —— 抓的时候它是藏着的）。
 * 记进来的盒子一直攒着，直到那个元素真的重抓一次才清掉：这样「损伤在屏幕另一头」
 * 攒再多也不会让它重抓，而一旦有一笔碰到它那一块，攒下来的这笔账仍然算数。 */
/* 临时探针（VIDOW_DMG_WHY=1）：谁在每帧标脏、标的多大一块。
 *
 * 静止桌面 300fps、损伤 96%/帧，说明有一个「每帧整屏标脏」的调用者：整屏标
 * 脏 → scene commit 有活 → 渲染 → 提交 → 又标脏，自己驱自己。日志里给的
 * 是**调用者的返回地址**（本进程 PIE，偏移 = 地址 - /proc/<pid>/maps 里
 * vidow 的映射起点），用 `addr2line -e build/vidow -f <偏移>` 反解。 */
void dmg_why_probe(const char *what, const struct wlr_box *box,
		uintptr_t ra) {
	static int64_t last;
	int64_t now = vidow_now_ns();
	if (now - last < 200000000LL) {
		return;
	}
	last = now;
	wlr_log(WLR_INFO, "DMGWHY %s box=%dx%d%+d%+d ra=%#lx", what,
		box != NULL ? box->width : 0, box != NULL ? box->height : 0,
		box != NULL ? box->x : 0, box != NULL ? box->y : 0,
		(unsigned long)ra);
}

static void blur_note_damage(struct vidow_server *server,
		const struct wlr_box *box, const struct vidow_view *skip) {
	if (box == NULL || box->width <= 0 || box->height <= 0) {
		return;
	}
	vidow_stats.dmg_notes++;
	if (getenv("VIDOW_DMG_WHY") != NULL) {
		dmg_why_probe("blur_note_damage", box,
			(uintptr_t)__builtin_return_address(0));
	}
	/* arrange 期间顺手攒一份「这一趟真实变过的世界区域」：arrange 收口时用它
	 * 代替整屏（见 view.c 的 vidow_arrange 和 server.h 的 arrange_damage）。
	 * 整屏哨兵盒（blur_box_all）攒进来没意义 —— 那本来就是「不知道变哪」，
	 * 直接置 force_full，收口时回退整屏。 */
	if (server->arrange_in_progress) {
		if (box->width >= 0x40000000 || box->height >= 0x40000000) {
			server->arrange_force_full = true;
		} else if (server->arrange_damage_any) {
			blur_box_add(&server->arrange_damage, box);
		} else {
			server->arrange_damage = *box;
			server->arrange_damage_any = true;
		}
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view == skip) {
			continue;
		}
		dmg_add(&view->fx.pending_damage, box);
	}
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		dmg_add(&ls->fx.pending_damage, box);
	}
	/* 弹出物背后的模糊也要跟着过期：菜单 / 候选框底下的窗口一动，它自己
	 * 那一块底衬就旧了。弹出物自己交帧时（skip == 它的 view）不记 —— 抓的
	 * 时候它本来就藏着。 */
	struct vidow_popup_blur *pb;
	wl_list_for_each(pb, &server->popup_blurs, link) {
		if (skip != NULL && pb->view == skip) {
			continue;
		}
		dmg_add(&pb->fx.pending_damage, box);
	}
}

void vidow_backdrop_damage_box(struct vidow_server *server,
		const struct wlr_box *box) {
	blur_note_damage(server, box, NULL);
}

/* 只记给**窗口**的底衬，不记给 layer surface。
 *
 * 视口过渡每帧平移整条带子，每个窗口都要报「我换位置了」—— 别的窗口的底衬里
 * 画着它，得重抓，这必须记。但 layer surface（noctalia 的 panel / 通知）**不
 * 跟着视口走**，它们的位置和底衬一点没变；以前一并记给它们，每帧几十笔 × 每个
 * layer，panel 的模糊底衬永远追不上，越堆越多最后卡死（用户：「layer 动画会
 * 卡，而且会死掉（noctalia」）。
 *
 * 而完全不记（5b7d441 的做法）又会漏图：窗口横移之后上一帧画在隔壁屏上的那一
 * 块没人擦，别的窗口底衬里留着它的残影（用户：「窗口聚焦移动的时候也会漏一点
 * 内容到隔壁显示器」）。两头都不对，所以要的是「记给窗口、不记给 layer」。 */
void vidow_backdrop_damage_box_views_only(struct vidow_server *server,
		const struct wlr_box *box) {
	if (box == NULL || box->width <= 0 || box->height <= 0) {
		return;
	}
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		dmg_add(&view->fx.pending_damage, box);
	}
	struct vidow_popup_blur *pb;
	wl_list_for_each(pb, &server->popup_blurs, link) {
		dmg_add(&pb->fx.pending_damage, box);
	}
}

/* 私有底衬要不要因为这次损伤重抓：损伤盒子空 = 没损伤（不用）；否则看它跟
 * 「这张底衬管得着的那块区域」（布局逻辑坐标）碰不碰得到。区域本身已经把模糊
 * 半径算进去了（见 view_region_rect），所以碰到区域就是碰到能影响它的内容。 */
bool blur_damage_hits(const struct vidow_fx_target *fx,
		const struct wlr_box *region) {
	return dmg_touch(&fx->pending_damage, region);
}

/* 窗口「现在画在哪」：布局几何 + 动画偏移（拖拽 / 开合 / overview 都算在这上面）。 */
struct wlr_box vidow_view_screen_box(const struct vidow_view *view) {
	struct wlr_box box = view->geom;
	box.x += (int)lround(view->anim_off_x);
	box.y += (int)lround(view->anim_off_y);
	return box;
}

/* 世界代次前进（损伤区域由调用方决定记哪一块）。 */
void damage_bump(struct vidow_server *server, bool layers) {
	vidow_stats.dmg_bumps++;
	if (getenv("VIDOW_DMG_WHY") != NULL) {
		dmg_why_probe(layers ? "damage_bump(layers)" : "damage_bump(scene)",
			NULL, (uintptr_t)__builtin_return_address(0));
	}
	server->gen_scene++;
	if (layers) {
		server->gen_layer++;
		struct vidow_output *output;
		wl_list_for_each(output, &server->outputs, link) {
			output->backdrop_revision++;
		}
	}
}

/* Layer repaints have an output: preserve the shared caches on other outputs.
 * Keep global scene/layer generations for legacy consumers, but record bounded
 * damage so unrelated private backdrop regions can keep their pixels too. */
void vidow_backdrop_damage_output(struct vidow_output *output) {
	if (output == NULL) {
		return;
	}
	struct vidow_server *server = output->server;
	server->gen_scene++;
	server->gen_layer++;
	output->backdrop_revision++;
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);
	blur_note_damage(server, &box, NULL);
}

/* 只知道「哪块屏、哪一块区域」变了（layer surface 上屏 / 下屏 / 落位 / 内容重画）：
 * 共享底衬必须整块重抓 —— 它是**整屏一份**，没有局部版本，重建判据是
 * output->backdrop_revision（见 fx.c:4381）；但**别人的私有底衬**只在这块盒子真的
 * 碰到它时才过期。
 *
 * 这是把 layer 事件从「整屏标脏」降到「一块盒子」的入口。为什么值得：一条通知
 * 弹出、一个菜单、vshot 的冻结覆盖层上屏，原来都让整屏每个窗口的私有底衬一起
 * 作废重抓。
 *
 * 代价模型（2026-10-28 对着代码核过，别凭印象写）：
 *   - 共享底衬：整屏一份，一次重抓 = 一次全屏 backdrop_render + 整条金字塔，
 *     但它每屏每帧最多一次，所有窗口分摊（shared_blur_rebuild，fx.c:3690）。
 *   - 私有底衬：**不是**再抓一遍屏幕，而是从自己那块 own_bd_tex 上跑
 *     blur_chain / blur_chain_rect，代价正比于窗口矩形（fx.c:4155 一带）。
 *     所以「整屏标脏」真正的代价是**每个窗口各跑一遍自己的金字塔** —— 窗口越
 *     多、2x 屏上越大，越亏。
 *
 * 屏上的真实损伤（要不要重画这一帧）不归这里管：那是 wlroots scene 按节点自己
 * 记的，layer 的 buffer 一交上去 scene 就会标脏。这里只喂模糊那套账本。 */
void vidow_backdrop_damage_output_box(struct vidow_output *output,
		const struct wlr_box *box) {
	if (output == NULL || box == NULL || box->width <= 0 ||
			box->height <= 0) {
		/* 说不清是哪一块：退回整屏，宁可多抓一次也不能糊着旧的 */
		if (output != NULL) {
			vidow_backdrop_damage_output(output);
		}
		return;
	}
	struct vidow_server *server = output->server;
	server->gen_scene++;
	server->gen_layer++;
	output->backdrop_revision++;
	blur_note_damage(server, box, NULL);
}

void vidow_backdrop_damage(struct vidow_server *server, bool layers) {
	damage_bump(server, layers);
	/* 不知道具体变的是哪一块（配置、输出、layer、overview、重排这些）：整屏
	 * 都算。宁可多抓一次，不能糊着旧的 —— 精确的损伤区域见下面两个入口。 */
	blur_note_damage(server, &blur_box_all, NULL);
}

void vidow_backdrop_damage_region(struct vidow_server *server,
		const struct wlr_box *box) {
	damage_bump(server, false);
	blur_note_damage(server, box, NULL);
}

/* 只有某个窗口自己变了的那些改动（它被拖、内容重画、开合动画、果冻形变）：
 * 世界代次照常前进，另外在这个窗口头上单独记一笔。私有底衬抓的是「世界减掉这
 * 个窗口」（抓的时候它自己藏着），所以那张图只跟 gen_scene - self_gen 有关 ——
 * 不记这一笔，拖一个重叠窗口时它自己的底衬每帧都被判过期，白重抓一遍。
 *
 * box == NULL：整块窗口都变了（老行为）。box != NULL：变的**只有**这一块 ——
 * 客户端只重画了窗口里一小块时（见 vidow_view_surface_damage），别人的底衬也
 * 只该因为这一小块过期：屏幕另一头那条 300px 宽的视频进度条，不该让整个窗口
 * 底下那片模糊重抓一遍。 */
void vidow_backdrop_damage_view_rect(struct vidow_view *view,
		const struct wlr_box *box) {
	struct wlr_box b = box != NULL ? *box : vidow_view_screen_box(view);
	/* 果冻在画的时候，屏幕上这一块比窗口框大一圈（留白 + 形变 + 投影）：
	 * 报小了，别人底衬里那一圈就一直是上一版的，松手 / 收尾之后看着像
	 * 滞留像素（见 jelly.c 的 vidow_jelly_pad）。 */
	if (view->jelly != NULL) {
		int pad = (int)ceil(vidow_jelly_pad(view)) + 8;
		b.x -= pad;
		b.y -= pad;
		b.width += 2 * pad;
		b.height += 2 * pad;
	}
	view->self_gen++;
	damage_bump(view->server, false);
	/* 变的是它自己这一块：别人（底衬里画着它）得重抓，它自己不用。 */
	blur_note_damage(view->server, &b, view);
	/* 一次 arrange 里只要有一个「不是正被拖的那个」窗口报了自己变了，这一趟就
	 * 不能整笔算在被拖的那个头上（见 vidow_arrange）。 */
	if (view->server->arrange_self_only && view->server->grab_view != view) {
		view->server->arrange_self_only = false;
	}
}

void vidow_backdrop_damage_view(struct vidow_view *view) {
	vidow_backdrop_damage_view_rect(view, NULL);
}

/* ------------------------------------------------------- 局部重跑的范围 */

/* 「只重跑受损那一小块」的开关：默认开。设了 VIDOW_BLUR_NO_PARTIAL 就整块重跑；
 * VIDOW_BLUR_NO_PARTIAL_FILE 指的文件一出现也整块重跑（跑着跑着切过去，用来比对
 * 「同一个场景、同一个客户端、同一个进程」下局部和整块的输出 —— 见
 * test-blurpartial.sh）。都只给 A/B 比对用。
 *
 * 两个 getenv 各只问一次（每次重抓都要问一遍：一帧里四五个元素重抓就是十来次；
 * getenv 在 profile 里比我们自己的模糊代码还显眼）。**文件存在性每次都看** ——
 * test-blurpartial.sh 就是跑着跑着把文件建出来切过去的。 */
/* 渲染热路径上的两个探针开关，只问一次环境变量。
 *
 * 它们都在「每帧 × 每个窗口」的函数里（view_region_build 的模糊重跑、
 * decor_draw 的 span），原神那种 160fps 满速提交下 getenv 的 environ
 * 线性扫描每秒上千次。环境变量进程起来之后不会变，缓存住就行 —— 和
 * 本文件上面 scanout_off / blur_no_partial 一个规矩。 */

bool blur_trace_on(void) {
	static int done = 0, on = 0;
	if (!done) {
		on = getenv("VIDOW_BLUR_TRACE") != NULL ? 1 : 0;
		done = 1;
	}
	return on != 0;
}

bool span_trace_on(void) {
	static int done = 0, on = 0;
	if (!done) {
		on = getenv("VIDOW_SPAN_TRACE") != NULL;
		done = 1;
	}
	if (!on) {
		/* 文件开关（运行时可开）：查跨屏两半的底衬归属就用它，不用重启。 */
		struct stat st;
		on = stat("/tmp/vidow-span", &st) == 0;
	}
	return on != 0;
}

bool blur_no_partial(void) {
	static int on = -1;
	static int file_read = 0;
	static const char *file = NULL;
	if (on < 0) {
		on = getenv("VIDOW_BLUR_NO_PARTIAL") != NULL ? 1 : 0;
	}
	if (!file_read) {
		file = getenv("VIDOW_BLUR_NO_PARTIAL_FILE");
		file_read = 1;
	}
	if (on != 0) {
		return true;
	}
	return file != NULL && access(file, F_OK) == 0;
}

/* 这次重抓**只需要动**的那一块（区域局部设备像素）：攒下来的损伤盒子跟区域相交
 * 的那部分的并集（往外放 2 像素，抵掉逻辑->设备的取整）。返回 false = 用不上，
 * 调用方整块重跑：
 *   - 一笔都没相交（不该走到这儿，防御）；
 *   - 损伤盖住了区域的一大半（>= 60%）：局部省不了多少，整块重跑还简单些。 */
bool region_update_rect(struct vidow_server *server,
		struct vidow_fx_target *fx, struct vidow_output *output,
		int rx, int ry, int rw, int rh, struct wlr_box *out) {
	double scale = output->wlr_output->scale;
	if (!(scale > 0.0)) {
		scale = 1.0;
	}
	const struct vidow_dmg_set *set = &fx->pending_damage;
	struct wlr_box u = {0};
	for (int i = 0; i < set->n; i++) {
		const struct wlr_box *b = &set->box[i];
		/* 布局逻辑坐标 -> 这块屏的设备像素（减去这块屏的布局原点）-> 区域局部 */
		double lx = (double)(b->x - output->box.x) * scale;
		double ly = (double)(b->y - output->box.y) * scale;
		double hx = (double)(b->x + b->width - output->box.x) * scale;
		double hy = (double)(b->y + b->height - output->box.y) * scale;
		struct wlr_box one = {
			.x = (int)floor(lx) - rx - 2,
			.y = (int)floor(ly) - ry - 2,
			.width = (int)ceil(hx) - (int)floor(lx) + 4,
			.height = (int)ceil(hy) - (int)floor(ly) + 4,
		};
		blur_box_add(&u, &one);
	}
	if (u.width <= 0 || u.height <= 0) {
		return false;
	}
	bool clip_ok = true;
	wlr_scene_node_for_each_buffer(&server->layer_toplevels->node,
		backdrop_clip_check, &clip_ok);
	if (!clip_ok) {
		return false;
	}
	int x1 = u.x < 0 ? 0 : u.x;
	int y1 = u.y < 0 ? 0 : u.y;
	int x2 = u.x + u.width > rw ? rw : u.x + u.width;
	int y2 = u.y + u.height > rh ? rh : u.y + u.height;
	if (x2 <= x1 || y2 <= y1) {
		return false;
	}
	if ((int64_t)(x2 - x1) * (y2 - y1) * 10 >= (int64_t)rw * rh * 6) {
		return false; /* 盖住大半了：整块重跑 */
	}
	*out = (struct wlr_box){
		.x = x1, .y = y1, .width = x2 - x1, .height = y2 - y1,
	};
	return true;
}
