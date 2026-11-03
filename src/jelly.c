/* 拖窗口时的果冻效果（KDE 的 wobbly windows 那一套）。
 *
 * 做法：把正在被拖的那个窗口的内容切成一张网格，每个网格节点挂在一根弹簧上，
 * 抓点那个节点钉死在光标下面（跟手 1:1），越往下的节点跟得越慢 —— 于是
 * 「上面已经走了、下面还没跟上」，窗口像一块布被揪着上边拖着走、松手后晃两下收住。
 *
 * 为什么不改场景节点：wlroots 的场景节点只会把缓冲整块贴上去，一张贴图一个
 * 矩形，没法弯。所以果冻走的是**自己渲染**：源（窗口那份带装饰的离屏图，
 * 也就是 fx 节点正在显示的那块缓冲）按网格一格一格贴进一块自己的缓冲里，
 * 每格的落点由弹簧状态算出来，最后让一个场景节点显示这块缓冲。窗口本来的
 * 节点（fx 节点）在这期间透明度压成 0（见 fx.c 里的 fx->jelly）。
 *
 * 物理（单位：逻辑像素、秒）：
 *
 *   o'' = -ω²(o - d) - c·o'          d = -(base - l)      l' = (base - l) / τ
 *
 *   o    该节点相对「自己该在的位置」的偏移；c 是阻尼、ω 是回正角频率。
 *   base 底座（窗口场景树）现在在哪；l 是「它以为底座在哪」的滞后版本。
 *   τ    跟底座的滞后时间常数：越大越跟不上，于是 d（该落后多少）越大。
 *
 * 抓点那个节点钉住（o = 0）：抓点永远精确在光标下，没有输入延迟 —— KDE 的手感。
 *
 * 数值：显式积分（半隐式欧拉）在这个刚度下必须用小步长，一帧按 1/500 秒切子步
 * （见 JELLY_DT），上限 JELLY_MAX_SUB 个子步，防止卡一下之后一次积太多。 */

#include "server.h"
#include "xtrace.h"

/* fx.c：按屏取模糊底衬的那一格（见 server.h 的 struct vidow_blur_slot）。 */
struct vidow_blur_slot *blur_slot_for(struct vidow_fx_target *fx,
	struct wlr_output *out, bool need);

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/render/pass.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* ---------------------------------------------------------------- 参数 */

/* 网格目标边长（逻辑像素）。格子越小越像布，代价是每格一次贴图绘制。 */
#define JELLY_CELL      40.0
#define JELLY_CELL_MIN  4
#define JELLY_CELL_MAX  24   /* 每轴最多几格（24x24 = 576 次贴图） */

/* 每个节点是一根**独立**的弹簧振子（节点之间没有耦合），但「该落后多少」是
 * **按行号算出来**的：
 *
 *   滞后位置 l_i：  l_i' = (base - l_i) / τ_i        （base = 窗口底座的位置）
 *   目标偏移   d_i：  d_i  = -(base - l_i)
 *   弹簧：          o'' = -ω²(o_i - d_i) - c·o_i'
 *
 * 说人话：越靠下的节点跟底座跟得越慢（τ 越大），于是「底座已经走到哪」和
 * 「它以为底座在哪」差得越多 —— 那个差值就是它该落后的量。顶行 τ ≈ 0
 * （1:1 跟手）、底行 τ = JELLY_TAU，中间线性过渡：窗口像上边被揪着、下边垂着
 * 的一块布，而不是整块平移。
 *
 * **梯度只看行号，不看抓点在哪**（抓标题栏还是抓底边，该软的地方一样软）。
 * 早先是按「到抓点的距离」算的（抓哪儿哪儿硬、四周散开），拖起来像被揪着一个
 * 点甩，而且抓点一换整块布的软硬分布就变了 —— 手感飘。
 *
 * 为什么不是「给每个节点加一个拖拽力」：那样每个节点受到的力一样，匀速拖动时
 * 整张网会**一起**平移（形变是均匀的），而加速的那一小段时间里软节点反而落后得
 * 少（响应慢）—— 梯度是反的，看着不像果冻、像整窗拖后腿（实测截图里就是这样）。
 *
 * ω 决定晃的频率、ζ 决定弹几下收住（阻尼比），τ 决定落后多少（见下面参数）。 */
#define JELLY_TAU       0.022   /* 最下面落后多久：滞后 ≈ V·τ（V=1500 时约 33px） */
#define JELLY_W0        19.0    /* 顶部附近的角频率（rad/s），19 ≈ 3Hz */
#define JELLY_GRADE     2.0     /* 越往下越软：ω 最低降到 1/sqrt(3) ≈ 1.7Hz */
#define JELLY_ZETA      0.35    /* 阻尼比：0.35 ≈ 弹两下收住 */
#define JELLY_TAU_MIN   0.004   /* τ 的下限（顶行）：再小就跟死底座了 */
#define JELLY_MARGIN    72.0    /* 形变缓冲四周留的余量（**逻辑**像素） */

#define JELLY_DT        (1.0 / 240.0)
#define JELLY_MAX_SUB   48          /* 一帧最多积分 200ms 的物理 */
#define JELLY_STILL     1.0         /* 所有偏移小于这个 = 已经静止（逻辑像素） */
#define JELLY_SETTLE_MS 700         /* 松手后最多再晃这么久 */
#define JELLY_KILL_MS   60.0        /* 超时之后偏移按这个时间常数收掉 */

/* ---------------------------------------------------------------- 结构 */

struct vidow_jelly;

/* **一块屏上的那一份果冻。**
 *
 * 果冻的源就是「那块屏自己的装饰缓冲」、底衬也是「那块屏自己的底衬」—— 和
 * 没有果冻时 fx 走的是同一条路，只是画出来的网格被弹簧掰弯了。以前整窗只画
 * 一份（挑一块屏的源铺到两块屏上），两块屏倍率不同时另一半就是错倍率的图：
 * 1x 铺到 2x 屏上每个像素被拉成两行（横纹），两套源轮换就是闪烁（用户
 * 2026-11-03）。
 *
 * 每格有自己的显示节点和两块轮换的形变缓冲；网格和弹簧只有一份（逻辑像素，
 * 两块屏共用），所以形变在两块屏上是连续的、接缝处对得上。
 */
struct jelly_slot {
	struct vidow_jelly *jelly;
	struct wlr_output *out;         /* 这块屏；NULL = 空闲格 */
	struct vidow_blur_slot *bs;     /* fx 里对应那一格（源 + 底衬） */
	struct wlr_scene_buffer *node;  /* 显示形变结果（挂在窗口的场景树下面） */
	struct wl_listener node_destroy;
	struct wlr_buffer *src;         /* 这一格的源：那块屏的装饰缓冲（每帧刷新） */
	int src_w, src_h;               /* 源缓冲尺寸，设备像素 */
	/* 源**内容**的版本号。装饰缓冲每帧在两张之间来回换（wlr_scene_buffer
	 * 上的指针一变），拿指针判「源换了」就会每帧重建这个节点 —— 屏幕上
	 * 一拖就闪（用户 2026-11-03 报的）。只有尺寸 / 倍率变了才真的重建，
	 * 内容变了只标一个「这一帧要重画」。 */
	uint64_t src_rev, src_blur_rev;
	double scale;                   /* 这块屏烘装饰用的倍率 */
	double margin_px;               /* JELLY_MARGIN 换算成设备像素 */
	int buf_w, buf_h;               /* 形变缓冲（设备像素）= src + 2*margin */
	int dst_w, dst_h;               /* 节点尺寸（逻辑像素） */
	int rel_x, rel_y;               /* 源左上角在场景树里的相对位置（逻辑） */
	struct wlr_buffer *target[2];   /* 两块轮换：正在上屏的那块不能再画 */
	int target_index;
	bool need_draw;                 /* 刚建出来 / 换了源：这一帧必须画一次 */
};

#define JELLY_SLOTS 4

struct vidow_jelly {
	struct vidow_server *server;
	struct vidow_view *view;
	struct jelly_slot slot[JELLY_SLOTS];
	/* 正在跟着场景树一起销毁（窗口没了）：这时候不能再回头去动那个视图的
	 * 其它节点（装饰节点也在同一棵树里，正被拆）。 */
	bool dying;

	int cols, rows;      /* 格子数（按窗口的**逻辑**尺寸定，两块屏共用） */
	int nx, ny;          /* 节点数 = cols + 1 / rows + 1 */
	float *verts;        /* 内容顶点：[x, y, u, v] × rows*cols*6 */
	/* 模糊顶点：[x, y, u_content, v_content, u_blur, v_blur] × rows*cols*6。
	 * 位置和内容层共用、内容 UV 共用；只有 blur UV 是按**静止的屏幕坐标**
	 * 另算的 —— 底衬不跟着网格弯（用户 2026-11-03：「果冻晃动的时候底衬
	 * 看起来和背景原来的位置一一对应」）。 */
	float *blur_verts;
	float *ox, *oy;      /* 每个节点的偏移（逻辑像素） */
	float *vx, *vy;      /* 每个节点的速度 */
	float *lx, *ly;      /* 每个节点的「滞后位置」（跟底座跟得慢的那个版本） */
	/* 每节点预算好的常数（见上面那段公式）：g = 行号（归一化，0 顶 1 底），
	 * w2 = ω²、cc = c、tau = τ。网格定下来就不变，不必每帧算。 */
	float *gw2, *gcc, *gtau;
	int pin;             /* 钉住的节点下标，-1 = 松手了（整张网自由回正） */
	/* 抓底衬期间把这窗口整个藏起来了（见 vidow_jelly_capture_visible）：
	 * 这期间别再按裁剪去 enable 节点，不然抓到的底衬里会留着形变图。 */
	bool capture_hidden;

	double strength;         /* 形变倍率（配置 jelly_strength） */
	/* 客户端上一帧提交的表面缓冲：换了就说明内容变了，得让 fx 那条管线重画
	 * 一次（松手之后没有指针事件，没人去催它，果冻会一直采样旧图）。 */
	struct wlr_buffer *src_surface_buffer;
	/* 客户端表面贴图：模糊遮罩用它的 alpha（窗口本身哪里真的存在），
	 * 投影不在客户端表面里，所以阴影不会被圈进模糊。 */
	struct wlr_buffer *mask_buf;
	struct wlr_texture *mask_tex;
	bool mask_owned;
	double base_x, base_y;   /* 上一帧底座（场景树）位置 */
	int64_t last_ns;
	/* 上一帧果冻在屏幕上占的那一整块（布局逻辑坐标，已按 vidow_jelly_pad
	 * 外扩）。形变把它掰出窗口框之外，下一帧只从「窗口上一次画在哪」算损伤
	 * 的话，掰出去又收回来的那几像素永远不会被擦 —— 拖完一回位就成了滞留
	 * 像素。每帧**先把上一帧这块擦掉**，再记这一帧的。 */
	struct wlr_box screen_box;
	bool screen_box_valid;
	int64_t release_ns;      /* 松手时刻，0 = 还在拖 */
	double last_max_off;     /* 上一帧的最大偏移：没动就不重画 */
	double last_sum_off;     /* 上一帧的偏移总量（形状变了也得重画） */
	int base_drawn_x, base_drawn_y; /* 上一帧画的时候底座在哪（挪了就得重画） */
	double kill;             /* 收尾系数：1 = 正常，超时后按指数收到 0 */
	bool drawn_once;
};

/* ---------------------------------------------------------------- 小工具 */

/* 客户端自己的缓冲：wlr_texture_from_buffer() 在 wlr_client_buffer 上会把它
 * 包一层、纹理是缓存好的，不能销毁。fx.c 里有一份一样的。 */
static struct wlr_buffer *jelly_src_buffer(struct wlr_buffer *buffer) {
	struct wlr_client_buffer *client = wlr_client_buffer_get(buffer);
	if (client != NULL && client->source != NULL) {
		return client->source;
	}
	return buffer;
}

static int clampi(int v, int lo, int hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

static double clampd(double v, double lo, double hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

/* 果冻节点不接输入：命中测试照旧走窗口本来的表面节点（fx 节点也是这么干的），
 * 不然拖着拖着 pick-window / 焦点就找不到窗口了。 */
static bool jelly_node_rejects_input(struct wlr_scene_buffer *buffer, double *sx,
		double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

/* 第 j 列节点的 x（**这一格**缓冲的局部坐标，设备像素）= 该在的位置 + 偏移。
 * **不取整**：网格顶点直接拿小数进光栅器，相邻格才严丝合缝（取整就是那条
 * 锯齿边）。偏移是**逻辑**像素（弹簧在逻辑空间里算），乘这一格的倍率。 */
static double jelly_slot_xf(struct vidow_jelly *jelly, struct jelly_slot *slot,
		int row, int col) {
	int k = row * jelly->nx + col;
	double step = (double)slot->src_w / (double)jelly->cols;
	double x = slot->margin_px + step * col +
		(double)jelly->ox[k] * slot->scale * jelly->strength * jelly->kill;
	return clampd(x, 0.0, (double)slot->buf_w);
}

/* 同一格**没形变时**的位置：只有 margin + step*index，不含 ox/oy 位移。
 * 模糊 UV 用它 —— 底衬是屏幕上已经烘好的背景，不参与形变（见 render 里那段）。 */
static double jelly_slot_rest_xf(struct jelly_slot *slot, int col) {
	double step = (double)slot->src_w / (double)slot->jelly->cols;
	return slot->margin_px + step * (double)col;
}

static double jelly_slot_rest_yf(struct jelly_slot *slot, int row) {
	double step = (double)slot->src_h / (double)slot->jelly->rows;
	return slot->margin_px + step * (double)row;
}

static double jelly_slot_yf(struct vidow_jelly *jelly, struct jelly_slot *slot,
		int row, int col) {
	int k = row * jelly->nx + col;
	double step = (double)slot->src_h / (double)jelly->rows;
	double y = slot->margin_px + step * row +
		(double)jelly->oy[k] * slot->scale * jelly->strength * jelly->kill;
	return clampd(y, 0.0, (double)slot->buf_h);
}

/* ---------------------------------------------------------------- 释放 */

static void jelly_slot_free(struct jelly_slot *slot) {
	if (slot == NULL) {
		return;
	}
	if (slot->node != NULL) {
		/* 回调里不摘钩子的话，wlr_scene_node_destroy() 收尾那条
		 * `assert(wl_list_empty(&node->events.destroy.listener_list))`
		 * 就会 SIGABRT（见 jelly_node_destroy）。 */
		wl_list_remove(&slot->node_destroy.link);
		wl_list_init(&slot->node_destroy.link);
		wlr_scene_node_destroy(&slot->node->node);
		slot->node = NULL;
	}
	for (int i = 0; i < 2; i++) {
		if (slot->target[i] != NULL) {
			wlr_buffer_drop(slot->target[i]);
			slot->target[i] = NULL;
		}
	}
	slot->bs = NULL;
	slot->out = NULL;
	slot->src = NULL;
	slot->need_draw = false;
}

static void jelly_free(struct vidow_jelly *jelly) {
	if (jelly == NULL) {
		return;
	}
	struct vidow_view *view = jelly->view;
	for (int i = 0; i < JELLY_SLOTS; i++) {
		jelly_slot_free(&jelly->slot[i]);
	}
	if (view != NULL) {
		/* 果冻这一版在别人底衬里是「窗外多一圈」的：它没了（或者换成不弯的
		 * 装饰节点）之后那一圈也得重抓，否则拖完 / 收尾之后屏幕上留着半截
		 * 滞留像素。损伤盒子按**最后画过的那一整块**报。 */
		if (!jelly->dying && jelly->screen_box_valid) {
			vidow_backdrop_damage_box(view->server, &jelly->screen_box);
		}
		if (view->jelly == jelly) {
			view->jelly = NULL;
		}
		/* 装饰节点重新露出来（内容一模一样，所以看不到接缝）。**当场**重画一次：
		 * 帧循环只在「模糊底衬变了」的时候才会去跑 apply_anim，光排一帧的话
		 * 那个节点会一直停在透明度 0 上 —— 窗口就凭空消失了（拖完一次窗口就
		 * 没了，踩过）。 */
		view->fx.jelly = false;
		if (!jelly->dying) {
			vidow_view_apply_anim(view);
			/* 它自己的私有底衬里也焊着那一版（抓的时候藏着的是这个窗口，
			 * 形变本身不在底衬里）—— 不用重抓，但「这一块变了」得记给人
			 * 别的窗口，下一帧它们才会把这里重新烘一遍。 */
			vidow_backdrop_damage_view(view);
		}
	}
	if (jelly->mask_owned && jelly->mask_tex != NULL) {
		wlr_texture_destroy(jelly->mask_tex);
	}
	jelly->mask_tex = NULL;
	if (jelly->mask_buf != NULL) {
		wlr_buffer_unlock(jelly->mask_buf);
		jelly->mask_buf = NULL;
	}
	free(jelly->verts);
	free(jelly->blur_verts);
	free(jelly->ox);
	free(jelly->oy);
	free(jelly->vx);
	free(jelly->vy);
	free(jelly->gw2);
	free(jelly->gcc);
	free(jelly->gtau);
	free(jelly->lx);
	free(jelly->ly);
	free(jelly);
}

static void jelly_node_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct jelly_slot *slot =
		wl_container_of(listener, slot, node_destroy);
	struct vidow_jelly *jelly = slot->jelly;
	/* 必须自己把钩子摘掉：wlr_scene_node_destroy() 收尾时有一条
	 * `assert(wl_list_empty(&node->events.destroy.listener_list))`
	 * （wlr_scene.c）—— 回调里不摘，节点销毁时就 SIGABRT。拖到一半客户端
	 * 退出、退出时还有窗口正弯着，都会走到这里。 */
	wl_list_remove(&slot->node_destroy.link);
	wl_list_init(&slot->node_destroy.link);
	slot->node = NULL;
	slot->bs = NULL;
	jelly_slot_free(slot);
	for (int i = 0; i < JELLY_SLOTS; i++) {
		if (jelly->slot[i].node != NULL) {
			return;
		}
	}
	jelly->dying = true;
	jelly_free(jelly);
}

/* 结束（松手收尾完成，或者中途发现源没了 / 窗口尺寸变了）。 */
void vidow_jelly_end(struct vidow_view *view) {
	if (view == NULL || view->jelly == NULL) {
		return;
	}
	jelly_free(view->jelly);
}

/* 视图正在被销毁（见 view.c 的 view_destroy）：先把果冻收掉。场景节点归
 * wlroots 所有、比 view 活得久，而果冻结构里攥着 view 指针 —— 不在这里收掉，
 * 节点最后被销毁时那个回调就会去写已经 free 掉的内存。 */
void vidow_jelly_view_gone(struct vidow_view *view) {
	if (view == NULL || view->jelly == NULL) {
		return;
	}
	view->jelly->dying = true;
	jelly_free(view->jelly);
}

/* ---------------------------------------------------------------- 起手 */

double vidow_jelly_pad(const struct vidow_view *view) {
	if (view == NULL || view->jelly == NULL) {
		return 0.0;
	}
	double pad = JELLY_MARGIN;
	if (view->server != NULL) {
		pad += vidow_fx_pad(view->server);
	}
	return pad;
}

/* 建一格：这块屏自己的源缓冲 + 自己的形变缓冲 + 自己的显示节点。
 *
 * 源换了（这块屏重烘了装饰 / 倍率变了）就整格重建 —— 网格和弹簧在 jelly 上，
 * 一点都不碰，所以跨屏那一刻形变是连着的（以前是整窗 rebuild + 搬状态）。 */
static bool jelly_slot_init(struct vidow_jelly *jelly, struct jelly_slot *slot,
		struct vidow_blur_slot *bs, struct wlr_scene_buffer *src_node,
		struct wlr_buffer *src, double scale, int tx, int ty) {
	struct vidow_server *server = jelly->server;
	if (src == NULL || src->width < 8 || src->height < 8 ||
			src_node == NULL) {
		return false;
	}
	if (scale <= 0.0) {
		scale = 1.0;
	}
	slot->jelly = jelly;
	slot->bs = bs;
	slot->out = bs != NULL ? bs->out : NULL;
	slot->src = src;
	slot->src_w = src->width;
	slot->src_h = src->height;
	slot->scale = scale;
	slot->margin_px = JELLY_MARGIN * scale;
	slot->buf_w = slot->src_w + 2 * (int)slot->margin_px;
	slot->buf_h = slot->src_h + 2 * (int)slot->margin_px;
	slot->dst_w = (int)lround((double)slot->buf_w / scale);
	slot->dst_h = (int)lround((double)slot->buf_h / scale);

	for (int i = 0; i < 2; i++) {
		slot->target[i] = vidow_alloc_target(server, slot->buf_w,
			slot->buf_h);
	}
	if (slot->target[0] == NULL || slot->target[1] == NULL) {
		wlr_log(WLR_ERROR, "jelly: 分配 %dx%d 的形变缓冲失败",
			slot->buf_w, slot->buf_h);
		jelly_slot_free(slot);
		return false;
	}

	slot->node = wlr_scene_buffer_create(jelly->view->scene_tree, NULL);
	if (slot->node == NULL) {
		jelly_slot_free(slot);
		return false;
	}
	slot->node->point_accepts_input = jelly_node_rejects_input;

	int fx_x = 0, fx_y = 0;
	wlr_scene_node_coords(&src_node->node, &fx_x, &fx_y);
	slot->rel_x = fx_x - tx;
	slot->rel_y = fx_y - ty;
	/* 节点位置是**逻辑**像素（场景图就是逻辑像素），缓冲里那圈留白是设备像素：
	 * 留白换算成逻辑就是 JELLY_MARGIN 本身，**不是** margin_px。写成 margin_px
	 * 的时候 2 倍屏上整个窗口偏 72 逻辑像素 —— 拖起来「窗口坐标偏移」，松手才
	 * 回位（那个节点只在果冻期间存在）。 */
	wlr_scene_node_set_position(&slot->node->node,
		slot->rel_x - (int)lround(JELLY_MARGIN),
		slot->rel_y - (int)lround(JELLY_MARGIN));
	wlr_scene_buffer_set_dest_size(slot->node, slot->dst_w, slot->dst_h);
	wlr_scene_node_raise_to_top(&slot->node->node);
	slot->node_destroy.notify = jelly_node_destroy;
	wl_signal_add(&slot->node->node.events.destroy, &slot->node_destroy);
	slot->src_rev = bs != NULL ? bs->dec_revision : 0;
	slot->src_blur_rev = bs != NULL ? bs->dec_blur_ver : 0;
	slot->need_draw = true;
	return true;
}

bool vidow_jelly_begin(struct vidow_view *view) {
	struct vidow_server *server = view->server;
	if (!server->config.jelly) {
		return false;
	}
	/* 上一个还没收完（松手之后紧接着又按下去拖）：直接收掉重来，不然新的拖拽
	 * 期间显示的还是上一轮那张弯着的图。 */
	vidow_jelly_end(view);
	if (view->scene_tree == NULL || view->popup) {
		return false;
	}
	/* 入场动画还在缩放 / overview 里：这两条路上装饰节点的位置尺寸每帧都在变，
	 * 网格对不上，干脆不做（拖完这一次还是好的）。 */
	if (fabs(view->anim_scale - 1.0) > 0.05 ||
			fabs(vidow_overview_scale(server) - 1.0) > 0.01) {
		return false;
	}

	int tx = 0, ty = 0;
	wlr_scene_node_coords(&view->scene_tree->node, &tx, &ty);

	struct vidow_jelly *jelly = calloc(1, sizeof(*jelly));
	if (jelly == NULL) {
		return false;
	}
	jelly->server = server;
	jelly->view = view;
	jelly->kill = 1.0;
	jelly->pin = -1;
	jelly->strength = clampd(server->config.jelly_strength, 0.0, 3.0);

	/* **窗口压着的每块屏各一格**：源和底衬都取那块屏自己的，和没有果冻时
	 * fx 画它的方式完全一样（见 struct jelly_slot）。跨屏拖拽中途会多出 /
	 * 少掉一格，那由 jelly_sync_slots() 每帧对账，网格不重建。 */
	struct vidow_fx_target *fx = &view->fx;
	int nslot = 0;
	for (struct vidow_blur_slot *bs = fx->blur_slot;
			bs != NULL && nslot < JELLY_SLOTS; bs = bs->next) {
		if (bs->node == NULL || !bs->node->node.enabled ||
				bs->node->buffer == NULL || !bs->dec_valid) {
			continue;
		}
		if (jelly_slot_init(jelly, &jelly->slot[nslot], bs, bs->node,
				bs->node->buffer, bs->dec_scale, tx, ty)) {
			nslot++;
		}
	}
	if (nslot == 0 && fx->active && fx->node != NULL &&
			fx->node->node.enabled && fx->node->buffer != NULL) {
		/* 没有分格（装饰 / shader 关着，或者老路）：退回单值节点。 */
		if (jelly_slot_init(jelly, &jelly->slot[0], NULL, fx->node,
				fx->node->buffer, fx->scale, tx, ty)) {
			nslot = 1;
		}
	}
	if (nslot == 0) {
		free(jelly);
		return false;
	}

	/* 网格按**逻辑**尺寸定：换源（跨屏换倍率）时 src_w 变了、格子也按逻辑
	 * 算，cols/rows 不变 —— 两块屏的网格一样密，接缝处才拼得上。 */
	struct jelly_slot *s0 = &jelly->slot[0];
	double logical_w = (double)s0->src_w / s0->scale;
	double logical_h = (double)s0->src_h / s0->scale;
	jelly->cols = clampi((int)lround(logical_w / JELLY_CELL),
		JELLY_CELL_MIN, JELLY_CELL_MAX);
	jelly->rows = clampi((int)lround(logical_h / JELLY_CELL),
		JELLY_CELL_MIN, JELLY_CELL_MAX);
	jelly->nx = jelly->cols + 1;
	jelly->ny = jelly->rows + 1;
	size_t n = (size_t)jelly->nx * (size_t)jelly->ny;
	jelly->verts = calloc((size_t)jelly->rows * (size_t)jelly->cols * 24,
		sizeof(float));
	jelly->blur_verts = calloc((size_t)jelly->rows * (size_t)jelly->cols * 36,
		sizeof(float));
	jelly->ox = calloc(n, sizeof(float));
	jelly->oy = calloc(n, sizeof(float));
	jelly->vx = calloc(n, sizeof(float));
	jelly->vy = calloc(n, sizeof(float));
	jelly->gw2 = calloc(n, sizeof(float));
	jelly->gcc = calloc(n, sizeof(float));
	jelly->gtau = calloc(n, sizeof(float));
	jelly->lx = calloc(n, sizeof(float));
	jelly->ly = calloc(n, sizeof(float));
	if (jelly->verts == NULL || jelly->blur_verts == NULL || jelly->ox == NULL || jelly->oy == NULL || jelly->vx == NULL ||
			jelly->vy == NULL || jelly->gw2 == NULL || jelly->gcc == NULL ||
			jelly->gtau == NULL || jelly->lx == NULL || jelly->ly == NULL) {
		wlr_log(WLR_ERROR, "jelly: 分配 %dx%d 网格失败",
			jelly->nx, jelly->ny);
		jelly_free(jelly);
		return false;
	}

	/* 抓点：光标在源缓冲里的位置 -> 最近的网格节点。钉住它，抓点就永远
	 * 精确在光标下面。（用第一格的逻辑几何，两块屏是同一个窗口。） */
	double cell_x = logical_w / (double)jelly->cols;
	double cell_y = logical_h / (double)jelly->rows;
	double sx_log = (double)tx + (double)s0->rel_x;
	double sy_log = (double)ty + (double)s0->rel_y;
	int pj = clampi((int)lround((server->cursor->x - sx_log) / cell_x),
		0, jelly->cols);
	int pi = clampi((int)lround((server->cursor->y - sy_log) / cell_y),
		0, jelly->rows);
	jelly->pin = pi * jelly->nx + pj;

	/* 每节点的弹簧常数（见文件顶上那段公式）：**从上到下**越来越软 —— 顶部跟着
	 * 底座走（几乎不落后），越往下跟得越慢、落后越多。梯度只看行号，跟抓点在哪
	 * 无关：抓在标题栏还是抓在底边，整张网该软的地方一样软。 */
	for (int i = 0; i < jelly->ny; i++) {
		double g = jelly->ny > 1 ?
			(double)i / (double)(jelly->ny - 1) : 0.0;
		double w = JELLY_W0 / sqrt(1.0 + JELLY_GRADE * g);
		double c = 2.0 * JELLY_ZETA * w;
		double tau = JELLY_TAU_MIN + (JELLY_TAU - JELLY_TAU_MIN) * g;
		for (int j = 0; j < jelly->nx; j++) {
			size_t k = (size_t)i * (size_t)jelly->nx + (size_t)j;
			jelly->gw2[k] = (float)(w * w);
			jelly->gcc[k] = (float)c;
			jelly->gtau[k] = (float)tau;
		}
	}

	jelly->base_x = tx;
	jelly->base_y = ty;
	/* 滞后位置从底座当前位置起算：初值留 0 的话，头几帧的「该落后多少」等于
	 * 窗口离场景原点多远（第二块屏上的窗口就是 1280px），会先甩一下再回来。 */
	for (size_t k = 0; k < n; k++) {
		jelly->lx[k] = (float)tx;
		jelly->ly[k] = (float)ty;
	}
	jelly->last_ns = vidow_now_ns();

	view->jelly = jelly;
	/* 装饰节点让位：内容一模一样（都是从同一块缓冲切出来的），只是它现在是
	 * 弯的。fx.c 每帧会把透明度写回去，所以必须是它认的一个标志位；当场跑一次
	 * apply_anim 让它立刻生效，别等下一帧。 */
	view->fx.jelly = true;
	vidow_view_apply_anim(view);
	{
		/* 形变范围比窗口那块大一圈（见上面 tick 里的说明）。 */
		struct wlr_box box = vidow_view_screen_box(view);
		int pad = (int)ceil(vidow_jelly_pad(view)) + 8;
		box.x -= pad;
		box.y -= pad;
		box.width += 2 * pad;
		box.height += 2 * pad;
		vidow_backdrop_damage_box(server, &box);
	}
	vidow_backdrop_damage_view(view); /* 只有它自己在形变 */
	wlr_log(WLR_DEBUG, "jelly: %s 起手（%d 格 x %dx%d 网格 %dx%d，抓点 %d,%d）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		nslot, s0->src_w, s0->src_h, jelly->cols, jelly->rows, pj, pi);
	return true;
}

/* 抓模糊底衬的时候要把这个窗口**整块**藏起来 —— 果冻节点是窗口内容的另一份
 * 拷贝（弯的那份），只关 fx 节点不够：抓出来的底衬里会留着这个窗口自己上一帧
 * 的形变图。透明的窗口一拖就是「背景里错位的竖线 / 重影」、模糊看着也没了。
 * 见 fx.c 的 view_set_capture_visible。 */
void vidow_jelly_capture_visible(struct vidow_view *view, bool on) {
	if (view == NULL || view->jelly == NULL) {
		return;
	}
	/* 抓的时候整窗藏着：这期间 tick 里那套「按屏裁剪 -> enable」别动节点。 */
	view->jelly->capture_hidden = !on;
	for (int i = 0; i < JELLY_SLOTS; i++) {
		struct jelly_slot *slot = &view->jelly->slot[i];
		if (slot->node != NULL) {
			wlr_scene_node_set_enabled(&slot->node->node, on);
		}
	}
}

/* 松手：解掉钉住的那个节点，整张网自己晃回去。窗口的位置照旧归布局管。 */
void vidow_jelly_release(struct vidow_view *view) {
	if (view == NULL || view->jelly == NULL) {
		return;
	}
	struct vidow_jelly *jelly = view->jelly;
	jelly->pin = -1;
	jelly->release_ns = vidow_now_ns();
}

/* ---------------------------------------------------------------- 积分 */

static void jelly_step(struct vidow_jelly *jelly, double base_x, double base_y,
		double dt) {
	size_t n = (size_t)jelly->nx * (size_t)jelly->ny;
	double limit = JELLY_MARGIN;
	for (size_t k = 0; k < n; k++) {
		if ((int)k == jelly->pin) {
			/* 抓点：钉死在光标下面（1:1 跟手，没有输入延迟）。 */
			jelly->ox[k] = 0.0f;
			jelly->oy[k] = 0.0f;
			jelly->vx[k] = 0.0f;
			jelly->vy[k] = 0.0f;
			jelly->lx[k] = (float)base_x;
			jelly->ly[k] = (float)base_y;
			continue;
		}
		double tau = jelly->gtau[k];
		double f = dt / tau;
		if (f > 1.0) {
			f = 1.0;
		}
		jelly->lx[k] += (float)((base_x - jelly->lx[k]) * f);
		jelly->ly[k] += (float)((base_y - jelly->ly[k]) * f);
		/* 该落后多少：底座已经走到哪 - 它以为底座在哪。 */
		double dx = -(base_x - jelly->lx[k]);
		double dy = -(base_y - jelly->ly[k]);

		double w2 = jelly->gw2[k], c = jelly->gcc[k];
		double ax = -w2 * (jelly->ox[k] - dx) - c * jelly->vx[k];
		double ay = -w2 * (jelly->oy[k] - dy) - c * jelly->vy[k];
		double vx = jelly->vx[k] + ax * dt;
		double vy = jelly->vy[k] + ay * dt;
		double ox = jelly->ox[k] + vx * dt;
		double oy = jelly->oy[k] + vy * dt;
		jelly->vx[k] = (float)vx;
		jelly->vy[k] = (float)vy;
		jelly->ox[k] = (float)clampd(ox, -limit, limit);
		jelly->oy[k] = (float)clampd(oy, -limit, limit);
	}
}

/* ---------------------------------------------------------------- 渲染 */

/* 这一格该不该自己叠一层底衬。
 *
 * **底衬要自己采**（和没有果冻时同一个着色器、同一条公式），原因是果冻把
 * 内容网格掰弯了：如果模糊烘在内容图里，底衬会跟着网格一起被拉 —— 用户
 * 2026-11-03：「果冻晃动的时候底衬看起来和背景原来的位置一一对应」。
 *
 * 所以模糊顶点另算一套 UV：位置取**静止**的屏幕坐标（不含弹簧位移），
 * 底衬就是那块屏上原来那张背景图，顶点滑到哪儿就采哪儿的像素。
 *
 * 每格用自己的那张底衬贴图 —— 跨屏时两块屏各有一张，一层贴图一套坐标，
 * 各画各的才不会拿错屏。 */
static bool jelly_slot_blur_ok(struct vidow_jelly *jelly,
		struct jelly_slot *slot) {
	struct vidow_fx_target *fx = &jelly->view->fx;
	if (!fx->blur || fx->blur_region_on || jelly->blur_verts == NULL ||
			!vidow_view_blur_wanted(jelly->view)) {
		return false;
	}
	return slot->bs != NULL && slot->bs->tex != NULL;
}

/* 把**这一格**画出来：内容是那块屏的装饰图，按网格掰弯；底衬是那块屏的
 * 底衬，按**静止的屏幕坐标**贴（不弯）。和没有果冻时是同一个着色器
 * （vidow_fx_draw_mesh_blur），所以静止时逐像素一样。 */
static void jelly_render(struct vidow_jelly *jelly, struct jelly_slot *slot) {
	struct vidow_server *server = jelly->server;
	/* 源每帧都在换（双缓冲）：用**这一帧**挂着的那张，别用上次记下的。 */
	if (slot->bs != NULL && slot->bs->node != NULL &&
			slot->bs->node->buffer != NULL) {
		slot->src = slot->bs->node->buffer;
	}
	struct wlr_buffer *src = slot->src;
	if (src == NULL || slot->node == NULL) {
		return;
	}
	struct wlr_buffer *target = NULL;
	for (int i = 0; i < 2; i++) {
		struct wlr_buffer *cand = slot->target[(slot->target_index + i) % 2];
		if (cand != NULL && cand->n_locks == 0) {
			target = cand;
			slot->target_index = (slot->target_index + i + 1) % 2;
			break;
		}
	}
	if (target == NULL) {
		return; /* 两块都还在上屏：这一帧不画，下一帧再说 */
	}

	struct wlr_texture *tex =
		wlr_texture_from_buffer(server->renderer, jelly_src_buffer(src));
	if (tex == NULL) {
		return;
	}

	struct vidow_fx_target *fx = &jelly->view->fx;
	struct vidow_blur_slot *bs = slot->bs;
	bool blur_ok = jelly_slot_blur_ok(jelly, slot);
	/* 底衬的原点 / 区域（设备像素）。有格子就用**这一格**自己的；老的单份
	 * 路（没有分格）退回 fx 里那一组单值。 */
	int out_x, out_y, rg_x, rg_y, bw, bh;
	if (bs != NULL) {
		out_x = bs->ox;
		out_y = bs->oy;
		rg_x = bs->rx;
		rg_y = bs->ry;
		bw = bs->rw > 0 ? bs->rw : bs->ow;
		bh = bs->rh > 0 ? bs->rh : bs->oh;
	} else {
		out_x = fx->blur_out_w > 0 ? fx->blur_out_x : server->output_x;
		out_y = fx->blur_out_h > 0 ? fx->blur_out_y : server->output_y;
		rg_x = fx->blur_tex_x;
		rg_y = fx->blur_tex_y;
		bw = fx->blur_tex_w > 0 ? fx->blur_tex_w : server->output_w;
		bh = fx->blur_tex_h > 0 ? fx->blur_tex_h : server->output_h;
	}
	if (bw < 1) {
		bw = 1;
	}
	if (bh < 1) {
		bh = 1;
	}
	/* 这一格烘装饰时用的内边距 / 内容尺寸（设备像素，**这块屏自己的**）。
	 * 不能用 fx->content_w 那一组单值：跨屏时它是「上一次烘的那一块」。 */
	int cpad = bs != NULL ? bs->dec_px_pad : fx->px_pad;
	int cw = bs != NULL && bs->dec_content_w > 0 ?
		bs->dec_content_w : fx->content_w;
	int ch = bs != NULL && bs->dec_content_h > 0 ?
		bs->dec_content_h : fx->content_h;

	/* 节点**左上角**是逻辑像素（场景图就是逻辑像素），乘这一格的倍率换到
	 * 设备像素，才和底衬贴图（按设备像素抓）同一个口径。
	 *
	 * 不能拿显示节点自己的坐标：窗口贴近屏幕边缘时那个节点被裁过（位置挪到
	 * 交集、源框切一块，见 tick 里那段），坐标就不再是缓冲的原点了，UV 会
	 * 整块偏出去（实测：窗口左半透出的全是右边那块背景）。源节点的坐标
	 * 没被裁过，减掉那圈留白就是缓冲原点。 */
	int node_x = 0, node_y = 0;
	if (blur_ok) {
		if (bs != NULL && bs->node != NULL) {
			wlr_scene_node_coords(&bs->node->node, &node_x, &node_y);
			node_x -= (int)lround(JELLY_MARGIN);
			node_y -= (int)lround(JELLY_MARGIN);
		} else {
			wlr_scene_node_coords(&slot->node->node, &node_x,
				&node_y);
		}
	}
	double s = slot->scale > 0.0 ? slot->scale : 1.0;

	int n1 = 0;
	for (int i = 0; i < jelly->rows; i++) {
		for (int j = 0; j < jelly->cols; j++) {
			for (int k = 0; k < 6; k++) {
				static const int dx6[6] = {0, 1, 0, 1, 1, 0};
				static const int dy6[6] = {0, 0, 1, 0, 1, 1};
				int col = j + dx6[k];
				int row = i + dy6[k];
				float x = (float)jelly_slot_xf(jelly, slot, row, col);
				float y = (float)jelly_slot_yf(jelly, slot, row, col);
				float u = (float)col / (float)jelly->cols;
				float v = (float)row / (float)jelly->rows;
				float *w = jelly->verts + (size_t)n1 * 4;
				w[0] = x;
				w[1] = y;
				w[2] = u;
				w[3] = v;
				if (blur_ok) {
					/* 底衬 UV 用**静止位置**（不含 ox/oy 位移）：
					 * 顶点滑到哪，就采它此刻压住的那块背景像素。
					 * 拿形变后的坐标算等于让底衬跟着网格弯。 */
					float *b = jelly->blur_verts + (size_t)n1 * 6;
					double sx = (double)node_x * s +
						jelly_slot_rest_xf(slot, col);
					double sy = (double)node_y * s +
						jelly_slot_rest_yf(slot, row);
					b[0] = x;
					b[1] = y;
					b[2] = u;
					b[3] = v;
					b[4] = (float)((sx - (double)out_x -
						(double)rg_x) / (double)bw);
					b[5] = (float)((sy - (double)out_y -
						(double)rg_y) / (double)bh);
				}
				n1++;
			}
		}
	}

	bool ok = false;
	if (blur_ok) {
		/* 模糊只画在窗口可见矩形（含圆角）里面：投影 / 圆角外那圈留白
		 * 不采模糊，否则阴影会被底下的底衬染糊。 */
		struct vidow_look look;
		vidow_view_look(&server->config, &jelly->view->rule_eff,
			jelly->view->fullscreen, &look);
		struct wlr_buffer *mb = jelly->view->scene_surface != NULL &&
			jelly->view->scene_surface->buffer != NULL ?
			jelly->view->scene_surface->buffer->buffer : NULL;
		/* mask 可能借的是 wlr_client_buffer 的缓存贴图：客户端每 commit 一次
		 * dmabuf，贴图会换一张（buffer 指针却可能不变）。只比较 mask_buf 会
		 * 把旧贴图留成悬空指针，下一帧传给 GLES2 就是
		 * `wlr_texture_is_gles2()` 断言 / SIGABRT。 */
		struct wlr_client_buffer *mask_client =
			mb != NULL ? wlr_client_buffer_get(mb) : NULL;
		bool mask_changed = mb != jelly->mask_buf;
		if (!mask_changed && mask_client != NULL) {
			if (mask_client->texture != NULL) {
				mask_changed = mask_client->texture != jelly->mask_tex;
			} else if (!jelly->mask_owned && jelly->mask_tex != NULL) {
				/* 借来的贴图没了：下一帧换成自己建的那张（或者 NULL）。 */
				mask_changed = true;
			}
		}
		if (mask_changed) {
			if (jelly->mask_owned && jelly->mask_tex != NULL) {
				wlr_texture_destroy(jelly->mask_tex);
			}
			/* 锁住这份 mask：旧 buffer 一旦被释放，地址可能被新 buffer 复用，
			 * 光比较 mask_buf 指针会把「新 buffer 的旧地址」当成同一份。 */
			if (mb != jelly->mask_buf) {
				if (jelly->mask_buf != NULL) {
					wlr_buffer_unlock(jelly->mask_buf);
				}
				jelly->mask_buf = mb != NULL ? wlr_buffer_lock(mb) : NULL;
			}
			jelly->mask_tex = vidow_fx_client_texture(server, mb,
				jelly->view->scene_surface != NULL ?
					jelly->view->scene_surface->buffer : NULL,
				&jelly->mask_owned);
		}
		/* 目标缓冲比内容四周多一圈 JELLY_MARGIN，窗口矩形在目标坐标里要
		 * 加上这圈留白；漏了它模糊就会偏进阴影 / 圆角外面。 */
		float shadow_pm[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
		ok = vidow_fx_draw_mesh_blur(server, target, slot->buf_w,
			slot->buf_h, tex, bs->tex,
			(float)server->config.blur_strength, jelly->blur_verts, n1,
			cpad, cpad, cw, ch,
			slot->src_w, slot->src_h,
			(float)(look.corner_radius * s),
			(float)vidow_corner_n_clamp(look.corner_n),
			jelly->mask_tex, fx->tex_off_x, fx->tex_off_y,
			fx->tex_scale_x, fx->tex_scale_y, 1.0f / 255.0f,
			NULL,
			(int)slot->margin_px + cpad, (int)slot->margin_px + cpad,
			cw, ch,
			0.0f, 0.0f, 0.0f, shadow_pm, false, true);
	}
	if (!ok) {
		ok = vidow_fx_draw_mesh(server, target, slot->buf_w, slot->buf_h,
			tex, jelly->verts, n1);
	}
	wlr_texture_destroy(tex);
	if (!ok) {
		return;
	}

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, slot->buf_w, slot->buf_h);
	wlr_scene_buffer_set_buffer_with_damage(slot->node, target, &damage);
	pixman_region32_fini(&damage);
	/* 果冻网格是同一张内容贴图的形变拷贝，编码跟着窗口表面走。 */
	struct vidow_view *jv = jelly->view;
	if (jv != NULL && jv->scene_surface != NULL &&
			jv->scene_surface->buffer != NULL) {
		vidow_color_inherit_tags(slot->node, jv->scene_surface->buffer);
	}
	slot->need_draw = false;
	jelly->drawn_once = true;
}

/* ---------------------------------------------------------------- 每帧 */

/* 一帧最多积分这么多秒：卡了一下（比如切 tty 回来）不能让物理一次跳一大步。 */
static void jelly_advance(struct vidow_jelly *jelly, double base_x,
		double base_y, double dt) {
	int steps = (int)(dt / JELLY_DT) + 1;
	if (steps > JELLY_MAX_SUB) {
		steps = JELLY_MAX_SUB;
	}
	for (int i = 0; i < steps; i++) {
		jelly_step(jelly, base_x, base_y, JELLY_DT);
	}
}

/* 源换了（跨屏：两块屏各烘一份装饰，倍率不同 → 缓冲尺寸 / UV 全都不一样）。
 *
 * 以前这里直接 vidow_jelly_end()，用户看到的就是「拖到另一块屏，果冻没了」。
 * 现在按新源重建一张网格：网格尺寸没变的话把上一帧的弹簧状态整份搬过去
 * （偏移 / 滞后都是**逻辑**像素，跟倍率无关），抓点也保持同一个 —— 跨屏那一刻
 * 只是换了块源缓冲，形变看不出接缝。
 *
 * 两次重建之间至少隔 JELLY_REBASE_MS：光标压在屏边界上时归属屏可能来回翻。 */
/* ---------------------------------------------------------------- 每帧 */

/* 这一帧该有哪些格：窗口压着的每块屏各一格（源 / 底衬都是那块屏自己的）。
 *
 * 跨屏拖拽时中途会**多出一格 / 少掉一格** —— 只建 / 拆那一格，网格和弹簧一点
 * 都不碰，形变因此连着（以前是整窗重建 + 搬状态，节流期内还干脆不画）。
 * 某个格子换了源（那块屏重烘了装饰、倍率变了）也照同样办法就地重建。 */
static bool jelly_sync_slots(struct vidow_jelly *jelly) {
	struct vidow_fx_target *fx = &jelly->view->fx;
	int tx = 0, ty = 0;
	wlr_scene_node_coords(&jelly->view->scene_tree->node, &tx, &ty);

	for (int i = 0; i < JELLY_SLOTS; i++) {
		struct jelly_slot *slot = &jelly->slot[i];
		struct vidow_blur_slot *bs = slot->bs;
		if (bs == NULL) {
			continue;
		}
		struct wlr_buffer *cur = bs->node != NULL &&
			bs->node->node.enabled && bs->node->buffer != NULL &&
			bs->dec_valid ? bs->node->buffer : NULL;
		if (cur == NULL) {
			/* 这块屏不再有装饰（窗口已经完全离开它）：拆掉这一格。 */
			jelly_slot_free(slot);
			continue;
		}
		/* 缓冲每帧在两张之间轮换（装饰是双缓冲的），**不能**按指针判断
		 * 「源换了」：那样每帧都拆掉重建，一拖就闪。只有尺寸 / 倍率变了
		 * 才真的重建这一格。 */
		if (fabs(bs->dec_scale - slot->scale) > 1e-3 ||
				cur->width != slot->src_w ||
				cur->height != slot->src_h) {
			/* 这一格的源换了尺寸 / 倍率：只重建这一格（网格不动）。 */
			jelly_slot_free(slot);
			jelly_slot_init(jelly, slot, bs, bs->node, cur,
				bs->dec_scale, tx, ty);
			continue;
		}
		slot->src = cur;
		/* 源**内容**换了（客户端新帧 / 底衬重抓）：这一格要重画一次。 */
		if (bs->dec_revision != slot->src_rev ||
				bs->dec_blur_ver != slot->src_blur_rev) {
			slot->src_rev = bs->dec_revision;
			slot->src_blur_rev = bs->dec_blur_ver;
			slot->need_draw = true;
		}
		int fx_x = 0, fx_y = 0;
		wlr_scene_node_coords(&bs->node->node, &fx_x, &fx_y);
		slot->rel_x = fx_x - tx;
		slot->rel_y = fx_y - ty;
		continue;
	}

	/* 新压上的屏：补一格。 */
	for (struct vidow_blur_slot *bs = fx->blur_slot; bs != NULL;
			bs = bs->next) {
		if (bs->node == NULL || !bs->node->node.enabled ||
				bs->node->buffer == NULL || !bs->dec_valid) {
			continue;
		}
		bool used = false;
		for (int i = 0; i < JELLY_SLOTS; i++) {
			if (jelly->slot[i].bs == bs) {
				used = true;
			}
		}
		if (used) {
			continue;
		}
		for (int i = 0; i < JELLY_SLOTS; i++) {
			struct jelly_slot *slot = &jelly->slot[i];
			if (slot->bs != NULL || slot->node != NULL) {
				continue;
			}
			jelly_slot_init(jelly, slot, bs, bs->node, bs->node->buffer,
				bs->dec_scale, tx, ty);
			break;
		}
	}

	for (int i = 0; i < JELLY_SLOTS; i++) {
		if (jelly->slot[i].node != NULL) {
			return true;
		}
	}
	return false;
}

void vidow_jelly_tick(struct vidow_server *server, int64_t now_ns) {
	struct vidow_view *view, *tmp;
	wl_list_for_each_safe(view, tmp, &server->views, link) {
		struct vidow_jelly *jelly = view->jelly;
		if (jelly == NULL) {
			continue;
		}
		if (!view->mapped || view->scene_tree == NULL ||
				!jelly_sync_slots(jelly)) {
			vidow_jelly_end(view);
			continue;
		}

		double dt = (double)(now_ns - jelly->last_ns) / 1e9;
		jelly->last_ns = now_ns;
		if (dt <= 0.0) {
			continue;
		}
		if (dt > 0.1) {
			dt = 0.1;
		}

		/* 底座（窗口场景树）现在在哪：整套形变就是「每个节点跟它跟得有多慢」
		 * 算出来的（见 jelly_step）。 */
		int tx = 0, ty = 0;
		wlr_scene_node_coords(&view->scene_tree->node, &tx, &ty);
		/* 一帧里底座跳了 100px 以上：这不是「拖着走」，是布局把它搬走了
		 * （跨屏落位、切工作区）。这时候不该有落后量，直接重置 —— 不然后面
		 * 那半秒里窗口会一直偏着一块（偏移被 JELLY_MARGIN 夹住）。 */
		if (fabs((double)tx - jelly->base_x) > 100.0 ||
				fabs((double)ty - jelly->base_y) > 100.0) {
			size_t nn = (size_t)jelly->nx * (size_t)jelly->ny;
			for (size_t k = 0; k < nn; k++) {
				jelly->lx[k] = (float)tx;
				jelly->ly[k] = (float)ty;
				jelly->ox[k] = 0.0f;
				jelly->oy[k] = 0.0f;
				jelly->vx[k] = 0.0f;
				jelly->vy[k] = 0.0f;
			}
			jelly->last_max_off = 0.0;
			jelly->last_sum_off = 0.0;
		}
		jelly->base_x = tx;
		jelly->base_y = ty;

		/* 每格的节点摆到「源左上角 - 留白」，并且**裁到它自己那块屏**：
		 * 场景图是全局坐标，两块屏都会画每一个节点 —— 不裁的话 1x 那格的
		 * 形变图也会铺到 2x 屏上（半透明窗口叠两层，实测亮 25 个色阶）。
		 * 裁法跟 fx.c 的 apply_anim 一样：位置挪到交集左上角、目标尺寸缩到
		 * 交集大小、源框跟着切一块。 */
		for (int i = 0; !jelly->capture_hidden && i < JELLY_SLOTS; i++) {
			struct jelly_slot *slot = &jelly->slot[i];
			if (slot->node == NULL) {
				continue;
			}
			struct wlr_box nb = {
				.x = slot->rel_x - (int)lround(JELLY_MARGIN),
				.y = slot->rel_y - (int)lround(JELLY_MARGIN),
				.width = slot->dst_w,
				.height = slot->dst_h,
			};
			struct wlr_box ob = {0}, inter;
			if (slot->out != NULL) {
				wlr_output_layout_get_box(server->output_layout,
					slot->out, &ob);
			}
			/* nb 是**场景树局部**坐标（节点是 scene_tree 的孩子），ob 是布局
			 * 全局坐标 —— 两个口径不能直接求交：起点不在 (0,0) 的那块屏永远
			 * 求不出交集，那一格就被整个关掉（那块屏上的果冻直接没了）；起点
			 * 在 (0,0) 的那块屏求出来的也是假的交集（位置被夹到 0、目标尺寸
			 * 被压短），画出来横跨到隔壁屏上、还是错的倍率。拖拽时这两件事
			 * 一起发作，就是「一拖就闪」。先换到全局求交，再换回局部摆节点。 */
			struct wlr_box gb = nb;
			gb.x += tx;
			gb.y += ty;
			/* 没有屏的（老路那格）/ 查不到屏框的：不裁，整块照画。 */
			if (ob.width < 1 || ob.height < 1) {
				wlr_scene_node_set_enabled(&slot->node->node, true);
				wlr_scene_node_set_position(&slot->node->node,
					nb.x, nb.y);
				wlr_scene_buffer_set_dest_size(slot->node,
					nb.width, nb.height);
				wlr_scene_buffer_set_source_box(slot->node, NULL);
				continue;
			}
			if (!wlr_box_intersection(&inter, &gb, &ob)) {
				/* 这块屏上没有它：整个关掉（不然它会画到别的屏上）。 */
				wlr_scene_node_set_enabled(&slot->node->node, false);
				continue;
			}
			wlr_scene_node_set_enabled(&slot->node->node, true);
			bool full = inter.width == gb.width &&
				inter.height == gb.height;
			wlr_scene_node_set_position(&slot->node->node,
				full ? nb.x : inter.x - tx,
				full ? nb.y : inter.y - ty);
			wlr_scene_buffer_set_dest_size(slot->node,
				full ? nb.width : inter.width,
				full ? nb.height : inter.height);
			if (full) {
				wlr_scene_buffer_set_source_box(slot->node, NULL);
			} else {
				double kx = (double)slot->buf_w /
					(double)nb.width;
				double ky = (double)slot->buf_h /
					(double)nb.height;
				struct wlr_fbox sb = {
					.x = (double)(inter.x - gb.x) * kx,
					.y = (double)(inter.y - gb.y) * ky,
					.width = (double)inter.width * kx,
					.height = (double)inter.height * ky,
				};
				wlr_scene_buffer_set_source_box(slot->node, &sb);
			}
		}

		/* 跟手探针（touch /tmp/vidow-x11-trace 开）：每格到底摆在哪、源节点
		 * 在哪、树在哪。拖动时窗口看着不动就是这三者里有一个没跟上。 */
		if (vidow_xtrace_on()) {
			for (int i = 0; i < JELLY_SLOTS; i++) {
				struct jelly_slot *slot = &jelly->slot[i];
				if (slot->node == NULL) {
					continue;
				}
				int jn_x = 0, jn_y = 0, fx_x = 0, fx_y = 0;
				wlr_scene_node_coords(&slot->node->node, &jn_x, &jn_y);
				if (slot->bs != NULL && slot->bs->node != NULL) {
					wlr_scene_node_coords(&slot->bs->node->node,
						&fx_x, &fx_y);
				}
				vidow_gtrace("jelly v=%d out=%s tree=%d,%d src=%d,%d "
					"rel=%d,%d node=%d,%d off=%.0f,%.0f scale=%.2f "
					"srcbuf=%dx%d en=%d dst=%dx%d",
					view->id,
					slot->out != NULL ? slot->out->name : "-",
					tx, ty, fx_x, fx_y, slot->rel_x, slot->rel_y,
					jn_x, jn_y, view->anim_off_x, view->anim_off_y,
					slot->scale, slot->src_w, slot->src_h,
					slot->node->node.enabled, slot->dst_w,
					slot->dst_h);
			}
		}

		/* 客户端提交了新内容：让装饰那趟重画一次，果冻采到的才是新图。 */
		struct wlr_buffer *cur =
			view->scene_surface != NULL && view->scene_surface->buffer != NULL ?
				view->scene_surface->buffer->buffer : NULL;
		if (cur != jelly->src_surface_buffer) {
			jelly->src_surface_buffer = cur;
			vidow_view_apply_anim(view);
		}

		/* 上一帧占过的地方先擦一遍：窗口移动时，形变掰出去的像素可能比这一帧
		 * 更靠外，只从「窗口现在在这」算损伤那块擦不到（见 struct 里
		 * screen_box 的说明）。盒子没变就不用报。 */
		struct wlr_box sbox = vidow_view_screen_box(view);
		int spad = (int)ceil(vidow_jelly_pad(view)) + 8;
		sbox.x -= spad;
		sbox.y -= spad;
		sbox.width += 2 * spad;
		sbox.height += 2 * spad;
		if (jelly->screen_box_valid &&
				!wlr_box_equal(&jelly->screen_box, &sbox)) {
			vidow_backdrop_damage_box(server, &jelly->screen_box);
		}
		jelly->screen_box = sbox;
		jelly->screen_box_valid = true;

		jelly_advance(jelly, (double)tx, (double)ty, dt);

		double max_off = 0.0, sum_off = 0.0;
		size_t n = (size_t)jelly->nx * (size_t)jelly->ny;
		for (size_t k = 0; k < n; k++) {
			double a = fabs(jelly->ox[k]), b = fabs(jelly->oy[k]);
			if (a > max_off) {
				max_off = a;
			}
			if (b > max_off) {
				max_off = b;
			}
			sum_off += a + b;
		}
		if (getenv("VIDOW_JELLY_DEBUG") != NULL) {
			wlr_log(WLR_DEBUG, "jelly tick dt=%.1fms maxoff=%.1f kill=%.2f",
				dt * 1000.0, max_off, jelly->kill);
		}

		/* 松手之后：晃到静止就交接，最多晃 JELLY_SETTLE_MS，超时按指数把
		 * 偏移收掉（收尾不能「啪」地跳一下 —— 交接时装饰节点重新露出来，
		 * 内容一模一样，所以只有偏移真的到 0 了才看不出接缝）。 */
		if (jelly->pin < 0 && jelly->release_ns != 0) {
			double since = (double)(now_ns - jelly->release_ns) / 1e6;
			if (since > JELLY_SETTLE_MS && jelly->kill > 0.001) {
				jelly->kill *= exp(-(dt * 1000.0) / JELLY_KILL_MS);
			}
			if (jelly->drawn_once && jelly->kill * max_off < JELLY_STILL) {
				vidow_jelly_end(view);
				continue;
			}
		}

		/* 最大偏移或者整张网的「总量」变了就得重画：只看最大值会漏掉形状在
		 * 变、最大值没变的那种（波荡过去的时候就是这样）。刚建出来的格子
		 * （need_draw）也要画一次。 */
		bool moved = !jelly->drawn_once ||
			fabs(max_off - jelly->last_max_off) > 0.05 ||
			fabs(sum_off - jelly->last_sum_off) > 0.5 ||
			tx != (int)jelly->base_drawn_x || ty != (int)jelly->base_drawn_y;
		jelly->last_max_off = max_off;
		jelly->last_sum_off = sum_off;
		jelly->base_drawn_x = tx;
		jelly->base_drawn_y = ty;
		for (int i = 0; i < JELLY_SLOTS; i++) {
			if (jelly->slot[i].node != NULL && jelly->slot[i].need_draw) {
				moved = true;
			}
		}
		if (moved) {
			/* 网格在动 -> 别的窗口背后的模糊底衬要重抓（和拖拽时那一下一样）；
			 * 它自己的私有底衬不受影响（抓的时候它藏着）。损伤范围按「果冻
			 * 实际占过的那一整块」报（窗口框外扩 vidow_jelly_pad）—— 只按
			 * max_off 外扩的话，那圈留白 / 投影从来没被擦过：拖完一回位，
			 * 曾过界过的点上就留着半截像素。 */
			struct wlr_box box = vidow_view_screen_box(view);
			int pad = (int)ceil(vidow_jelly_pad(view)) + 8;
			box.x -= pad;
			box.y -= pad;
			box.width += 2 * pad;
			box.height += 2 * pad;
			vidow_backdrop_damage_box(server, &box);
			vidow_backdrop_damage_view(view);
			for (int i = 0; i < JELLY_SLOTS; i++) {
				if (jelly->slot[i].node != NULL) {
					jelly_render(jelly, &jelly->slot[i]);
				}
			}
		}
	}
}

bool vidow_jelly_pending(struct vidow_server *server) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->jelly != NULL) {
			return true;
		}
	}
	return false;
}

/* 抓帧遮挡要一起压：**每块屏那一格**都得压（见 capturemask.c）。 */
void vidow_jelly_set_opacity(struct vidow_view *view, float opacity) {
	if (view == NULL || view->jelly == NULL) {
		return;
	}
	for (int i = 0; i < JELLY_SLOTS; i++) {
		struct jelly_slot *slot = &view->jelly->slot[i];
		if (slot->node != NULL) {
			wlr_scene_buffer_set_opacity(slot->node, opacity);
		}
	}
}
