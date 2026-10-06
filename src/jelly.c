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

#include <math.h>
#include <stdlib.h>

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

struct vidow_jelly {
	struct vidow_server *server;
	struct vidow_view *view;
	/* 显示形变结果的那个场景节点（挂在窗口的场景树下面，所以跟着窗口走）。 */
	struct wlr_scene_buffer *node;
	struct wl_listener node_destroy;
	/* 正在跟着场景树一起销毁（窗口没了）：这时候不能再回头去动那个视图的
	 * 其它节点（装饰节点也在同一棵树里，正被拆）。 */
	bool dying;

	int cols, rows;      /* 格子数 */
	int nx, ny;          /* 节点数 = cols + 1 / rows + 1 */
	float *verts;        /* 网格顶点：[x, y, u, v] × rows*cols*6，见 jelly_render */
	float *ox, *oy;      /* 每个节点的偏移（逻辑像素） */
	float *vx, *vy;      /* 每个节点的速度 */
	float *lx, *ly;      /* 每个节点的「滞后位置」（跟底座跟得慢的那个版本） */
	/* 每节点预算好的常数（见上面那段公式）：g = 行号（归一化，0 顶 1 底），
	 * w2 = ω²、cc = c、tau = τ。网格定下来就不变，不必每帧算。 */
	float *gw2, *gcc, *gtau;
	int pin;             /* 钉住的节点下标，-1 = 松手了（整张网自由回正） */

	/* 源（fx 目标）的几何，设备像素 */
	int src_w, src_h;
	double scale;
	/* 自己的形变缓冲：两块轮着用（正在上屏的那块不能再往里画） */
	struct wlr_buffer *target[2];
	int target_index;
	int buf_w, buf_h;    /* 设备像素 */
	int dst_w, dst_h;    /* 场景节点尺寸（逻辑单位） */
	double margin_px;    /* JELLY_MARGIN 换算成**设备**像素（缓冲里那圈留白） */

	int last_rel_x, last_rel_y; /* 源缓冲左上角在场景树里的相对位置 */

	double strength;         /* 形变倍率（配置 jelly_strength） */
	/* 客户端上一帧提交的表面缓冲：换了就说明内容变了，得让 fx 那条管线重画
	 * 一次（松手之后没有指针事件，没人去催它，果冻会一直采样旧图）。 */
	struct wlr_buffer *src_surface_buffer;
	double base_x, base_y;   /* 上一帧底座（场景树）位置 */
	int64_t last_ns;
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

/* 源：窗口那份带装饰的离屏图，也就是 fx 节点正在显示的缓冲。 */
static struct wlr_buffer *jelly_source(struct vidow_jelly *jelly) {
	struct vidow_fx_target *fx = &jelly->view->fx;
	if (!fx->active || fx->node == NULL) {
		return NULL;
	}
	return fx->node->buffer;
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

/* 第 j 列节点的 x（设备像素，缓冲局部坐标）= 该在的位置 + 偏移。**不取整**：
 * 网格顶点直接拿小数进光栅器，相邻格才严丝合缝（取整就是那条锯齿边）。 */
static double jelly_node_xf(struct vidow_jelly *jelly, int row, int col) {
	int k = row * jelly->nx + col;
	double step = (double)jelly->src_w / (double)jelly->cols;
	double x = jelly->margin_px + step * col +
		(double)jelly->ox[k] * jelly->scale * jelly->strength * jelly->kill;
	return clampd(x, 0.0, (double)jelly->buf_w);
}

static double jelly_node_yf(struct vidow_jelly *jelly, int row, int col) {
	int k = row * jelly->nx + col;
	double step = (double)jelly->src_h / (double)jelly->rows;
	double y = jelly->margin_px + step * row +
		(double)jelly->oy[k] * jelly->scale * jelly->strength * jelly->kill;
	return clampd(y, 0.0, (double)jelly->buf_h);
}

/* ---------------------------------------------------------------- 释放 */

static void jelly_free(struct vidow_jelly *jelly) {
	if (jelly == NULL) {
		return;
	}
	struct vidow_view *view = jelly->view;
	if (jelly->node != NULL) {
		wl_list_remove(&jelly->node_destroy.link);
		wlr_scene_node_destroy(&jelly->node->node);
		jelly->node = NULL;
	}
	for (int i = 0; i < 2; i++) {
		if (jelly->target[i] != NULL) {
			wlr_buffer_drop(jelly->target[i]);
			jelly->target[i] = NULL;
		}
	}
	if (view != NULL) {
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
		}
	}
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
	struct vidow_jelly *jelly =
		wl_container_of(listener, jelly, node_destroy);
	/* 必须自己把钩子摘掉：wlr_scene_node_destroy() 收尾时有一条
	 * `assert(wl_list_empty(&node->events.destroy.listener_list))`
	 * （wlr_scene.c）—— 回调里不摘，节点销毁时就 SIGABRT。拖到一半客户端
	 * 退出、退出时还有窗口正弯着，都会走到这里。 */
	wl_list_remove(&jelly->node_destroy.link);
	wl_list_init(&jelly->node_destroy.link);
	jelly->node = NULL;
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
	struct vidow_fx_target *fx = &view->fx;
	/* 果冻的源是 fx 那份带装饰的离屏图：没有它就没东西可弯（比如装饰和
	 * shader 都关着）。 */
	if (!fx->active || fx->node == NULL || fx->node->buffer == NULL) {
		return false;
	}
	/* 入场动画还在缩放 / overview 里：这两条路上装饰节点的位置尺寸每帧都在变，
	 * 网格对不上，干脆不做（拖完这一次还是好的）。 */
	if (fabs(view->anim_scale - 1.0) > 0.05 ||
			fabs(vidow_overview_scale(server) - 1.0) > 0.01) {
		return false;
	}
	if (fx->w < 8 || fx->h < 8) {
		return false;
	}

	int tx = 0, ty = 0, fx_x = 0, fx_y = 0;
	wlr_scene_node_coords(&view->scene_tree->node, &tx, &ty);
	wlr_scene_node_coords(&fx->node->node, &fx_x, &fx_y);

	struct vidow_jelly *jelly = calloc(1, sizeof(*jelly));
	if (jelly == NULL) {
		return false;
	}
	jelly->server = server;
	jelly->view = view;
	jelly->src_w = fx->w;
	jelly->src_h = fx->h;
	jelly->scale = fx->scale > 0.0 ? fx->scale : 1.0;
	jelly->last_rel_x = fx_x - tx;
	jelly->last_rel_y = fx_y - ty;
	jelly->kill = 1.0;
	jelly->pin = -1;
	jelly->strength = clampd(server->config.jelly_strength, 0.0, 3.0);

	jelly->margin_px = JELLY_MARGIN * jelly->scale;
	jelly->buf_w = jelly->src_w + 2 * (int)jelly->margin_px;
	jelly->buf_h = jelly->src_h + 2 * (int)jelly->margin_px;
	jelly->dst_w = (int)lround((double)jelly->buf_w / jelly->scale);
	jelly->dst_h = (int)lround((double)jelly->buf_h / jelly->scale);

	/* 网格：按目标格子大小定，两边都夹住。 */
	double cell = JELLY_CELL * jelly->scale;
	jelly->cols = clampi((int)lround((double)jelly->src_w / cell),
		JELLY_CELL_MIN, JELLY_CELL_MAX);
	jelly->rows = clampi((int)lround((double)jelly->src_h / cell),
		JELLY_CELL_MIN, JELLY_CELL_MAX);
	jelly->nx = jelly->cols + 1;
	jelly->ny = jelly->rows + 1;
	size_t n = (size_t)jelly->nx * (size_t)jelly->ny;
	jelly->verts = calloc((size_t)jelly->rows * (size_t)jelly->cols * 24,
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
	for (int i = 0; i < 2; i++) {
		jelly->target[i] = vidow_alloc_target(server, jelly->buf_w,
			jelly->buf_h);
	}
	if (jelly->verts == NULL ||
			jelly->ox == NULL || jelly->oy == NULL || jelly->vx == NULL ||
			jelly->vy == NULL || jelly->gw2 == NULL || jelly->gcc == NULL ||
			jelly->gtau == NULL || jelly->lx == NULL || jelly->ly == NULL ||
			jelly->target[0] == NULL ||
			jelly->target[1] == NULL) {
		wlr_log(WLR_ERROR, "jelly: 分配 %dx%d 的形变缓冲失败",
			jelly->buf_w, jelly->buf_h);
		jelly->node = NULL;
		jelly_free(jelly);
		return false;
	}

	jelly->node = wlr_scene_buffer_create(view->scene_tree, NULL);
	if (jelly->node == NULL) {
		jelly->node = NULL;
		jelly_free(jelly);
		return false;
	}
	jelly->node->point_accepts_input = jelly_node_rejects_input;
	/* 节点位置是**逻辑**像素（场景图就是逻辑像素），缓冲里那圈留白是设备像素：
	 * 留白换算成逻辑就是 JELLY_MARGIN 本身，**不是** margin_px。写成 margin_px
	 * 的时候 2 倍屏上整个窗口偏 72 逻辑像素 —— 拖起来「窗口坐标偏移」，松手才
	 * 回位（那个节点只在果冻期间存在）。 */
	wlr_scene_node_set_position(&jelly->node->node,
		jelly->last_rel_x - (int)lround(JELLY_MARGIN),
		jelly->last_rel_y - (int)lround(JELLY_MARGIN));
	wlr_scene_buffer_set_dest_size(jelly->node, jelly->dst_w, jelly->dst_h);
	wlr_scene_node_raise_to_top(&jelly->node->node);
	jelly->node_destroy.notify = jelly_node_destroy;
	wl_signal_add(&jelly->node->node.events.destroy, &jelly->node_destroy);

	/* 抓点：光标在源缓冲里的位置 -> 最近的网格节点。钉住它，抓点就永远
	 * 精确在光标下面。 */
	double lx = (server->cursor->x - fx_x) * jelly->scale;
	double ly = (server->cursor->y - fx_y) * jelly->scale;
	int pj = clampi((int)lround(lx / ((double)jelly->src_w / jelly->cols)),
		0, jelly->cols);
	int pi = clampi((int)lround(ly / ((double)jelly->src_h / jelly->rows)),
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
	vidow_backdrop_damage(server, false);
	wlr_log(WLR_DEBUG, "jelly: %s 起手（%dx%d 网格 %dx%d，抓点 %d,%d）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		jelly->src_w, jelly->src_h, jelly->cols, jelly->rows, pj, pi);
	return true;
}

/* 抓模糊底衬的时候要把这个窗口**整块**藏起来 —— 果冻节点是窗口内容的另一份
 * 拷贝（弯的那份），只关 fx 节点不够：抓出来的底衬里会留着这个窗口自己上一帧
 * 的形变图。透明的窗口一拖就是「背景里错位的竖线 / 重影」、模糊看着也没了。
 * 见 fx.c 的 view_set_capture_visible。 */
void vidow_jelly_capture_visible(struct vidow_view *view, bool on) {
	if (view == NULL || view->jelly == NULL || view->jelly->node == NULL) {
		return;
	}
	wlr_scene_node_set_enabled(&view->jelly->node->node, on);
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

static void jelly_render(struct vidow_jelly *jelly) {
	struct vidow_server *server = jelly->server;
	struct wlr_buffer *src = jelly_source(jelly);
	if (src == NULL) {
		return; /* 源这一帧没准备好：保持上一帧的画面 */
	}
	struct wlr_buffer *target = NULL;
	for (int i = 0; i < 2; i++) {
		struct wlr_buffer *cand = jelly->target[(jelly->target_index + i) % 2];
		if (cand != NULL && cand->n_locks == 0) {
			target = cand;
			jelly->target_index = (jelly->target_index + i + 1) % 2;
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

	/* 网格：每格两个三角形，顶点是**相邻格共用**的那几个（节点坐标不取整，
	 * 直接给小数）—— 相邻格边界严丝合缝，既不会留缝也不会重叠。 */
	int n = 0;
	for (int i = 0; i < jelly->rows; i++) {
		for (int j = 0; j < jelly->cols; j++) {
			float x0 = (float)jelly_node_xf(jelly, i, j);
			float x1 = (float)jelly_node_xf(jelly, i, j + 1);
			float x2 = (float)jelly_node_xf(jelly, i + 1, j);
			float x3 = (float)jelly_node_xf(jelly, i + 1, j + 1);
			float y0 = (float)jelly_node_yf(jelly, i, j);
			float y1 = (float)jelly_node_yf(jelly, i, j + 1);
			float y2 = (float)jelly_node_yf(jelly, i + 1, j);
			float y3 = (float)jelly_node_yf(jelly, i + 1, j + 1);
			float u0 = (float)j / (float)jelly->cols;
			float u1 = (float)(j + 1) / (float)jelly->cols;
			float v0 = (float)i / (float)jelly->rows;
			float v1 = (float)(i + 1) / (float)jelly->rows;
			float *w = jelly->verts + (size_t)n * 4;
			/* 左下三角 (x0,y0)-(x1,y1)-(x2,y2) */
			w[0] = x0;  w[1] = y0;  w[2] = u0; w[3] = v0;
			w[4] = x1;  w[5] = y1;  w[6] = u1; w[7] = v0;
			w[8] = x2;  w[9] = y2;  w[10] = u0; w[11] = v1;
			/* 右上三角 (x1,y1)-(x3,y3)-(x2,y2) */
			w[12] = x1; w[13] = y1; w[14] = u1; w[15] = v0;
			w[16] = x3; w[17] = y3; w[18] = u1; w[19] = v1;
			w[20] = x2; w[21] = y2; w[22] = u0; w[23] = v1;
			n += 6;
		}
	}
	bool ok = vidow_fx_draw_mesh(server, target, jelly->buf_w, jelly->buf_h,
		tex, jelly->verts, n);
	wlr_texture_destroy(tex);
	if (!ok) {
		return;
	}

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, jelly->buf_w, jelly->buf_h);
	wlr_scene_buffer_set_buffer_with_damage(jelly->node, target, &damage);
	pixman_region32_fini(&damage);
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

void vidow_jelly_tick(struct vidow_server *server, int64_t now_ns) {
	struct vidow_view *view, *tmp;
	wl_list_for_each_safe(view, tmp, &server->views, link) {
		struct vidow_jelly *jelly = view->jelly;
		if (jelly == NULL) {
			continue;
		}
		struct vidow_fx_target *fx = &view->fx;
		/* 源换了尺寸（窗口改了大小 / 输出换了倍率）：网格对不上，直接收掉。 */
		if (!fx->active || fx->node == NULL || fx->w != jelly->src_w ||
				fx->h != jelly->src_h ||
				fabs(fx->scale - jelly->scale) > 1e-3) {
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

		/* 源缓冲左上角在场景树里的相对位置每帧对一次：入场动画的缩放、
		 * CSD 留白、输出倍率变化都在这里面（节点位置跟着它摆，源和结果才
		 * 严丝合缝地叠在一起）。 */
		int fx_x = 0, fx_y = 0;
		wlr_scene_node_coords(&fx->node->node, &fx_x, &fx_y);
		jelly->last_rel_x = fx_x - tx;
		jelly->last_rel_y = fx_y - ty;
		wlr_scene_node_set_position(&jelly->node->node,
			jelly->last_rel_x - (int)lround(JELLY_MARGIN),
			jelly->last_rel_y - (int)lround(JELLY_MARGIN));

		/* 客户端提交了新内容：让装饰那趟重画一次，果冻采到的才是新图。 */
		struct wlr_buffer *cur =
			view->scene_surface != NULL && view->scene_surface->buffer != NULL ?
				view->scene_surface->buffer->buffer : NULL;
		if (cur != jelly->src_surface_buffer) {
			jelly->src_surface_buffer = cur;
			vidow_view_apply_anim(view);
		}

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

		/* 网格几乎没动、底座也停着：这一帧没什么可重画的（松手后收尾的尾巴
		 * 上大部分帧都落在这一支里）。 */
		/* 最大偏移或者整张网的「总量」变了就得重画：只看最大值会漏掉形状在
		 * 变、最大值没变的那种（波荡过去的时候就是这样）。 */
		bool moved = !jelly->drawn_once ||
			fabs(max_off - jelly->last_max_off) > 0.05 ||
			fabs(sum_off - jelly->last_sum_off) > 0.5 ||
			tx != (int)jelly->base_drawn_x || ty != (int)jelly->base_drawn_y;
		jelly->last_max_off = max_off;
		jelly->last_sum_off = sum_off;
		jelly->base_drawn_x = tx;
		jelly->base_drawn_y = ty;
		if (moved) {
			/* 网格在动 -> 背后的模糊底衬要重抓（和拖拽时那一下一样）。 */
			vidow_backdrop_damage(server, false);
			/* 节点在树里的相对位置固定，只有内容在变：重画一次。 */
			jelly_render(jelly);
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

/* 形变那个场景节点（抓帧遮挡要一起压，见 capturemask.c）。 */
struct wlr_scene_buffer *vidow_jelly_node(struct vidow_view *view) {
	return view != NULL && view->jelly != NULL ? view->jelly->node : NULL;
}
