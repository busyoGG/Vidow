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
/* 源换了一块屏的倍率之后，隔这么久才允许再换一次。跨屏那一刻两块屏各烘一份
 * 装饰，归属屏可能在光标压边界时来回翻 —— 不加这个底线就是每帧重建一张
 * 整窗的网格。 */
#define JELLY_REBASE_MS 100
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
	float *verts;        /* 内容顶点：[x, y, u, v] × rows*cols*6 */
	/* 模糊顶点：[x, y, u_content, v_content, u_blur, v_blur] × rows*cols*6。
	 * 位置和内容层共用，内容 UV 共用；只有 blur UV 是按屏幕坐标另算的。 */
	float *blur_verts;
	/* 跨屏骑边界时第二趟（另一块屏那一半）的顶点：内容层 + 模糊层。只在
	 * 窗口同时压着两块屏时用（见 jelly_render 里拆两趟的说明）。 */
	float *verts2;
	float *blur_verts2;
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

	/* 上一次「按新源重建」的时刻：跨屏时归属屏的倍率一变就要重建一次（见
	 * jelly_rebase），两块屏来回翻的瞬间别跟着每帧重建。 */
	int64_t rebase_ns;

	double strength;         /* 形变倍率（配置 jelly_strength） */
	/* 客户端上一帧提交的表面缓冲：换了就说明内容变了，得让 fx 那条管线重画
	 * 一次（松手之后没有指针事件，没人去催它，果冻会一直采样旧图）。 */
	struct wlr_buffer *src_surface_buffer;
	/* 客户端表面贴图：模糊遮罩用它的 alpha（窗口本身哪里真的存在），
	 * 投影不在客户端表面里，所以阴影不会被圈进模糊。 */
	struct wlr_buffer *mask_buf;
	struct wlr_texture *mask_tex;
	bool mask_owned;
	/* 果冻期间单独的投影：装饰管线原样烘一张，不参与形变。 */
	struct wlr_buffer *shadow_buf;
	struct wlr_texture *shadow_tex;
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

/* 果冻的**源**：整窗的一份装饰缓冲，外加它属于哪块屏（= 按哪块屏的倍率烘的）。
 *
 * 果冻是整窗一张缓冲、一份源，两块屏同时显示它，所以「取哪一份」只看倍率：
 * **取最高的那一份**。2x 屏按 1:1 采样（原生清晰），1x 屏降采样（略软，但不
 * 会有隔行重复的横纹）。
 *
 * 以前按 fx->bake_output 挑源，而跨屏时 bake_output 每帧在两块屏之间翻
 * （view_bake_for_output 给对面那块屏再烘一份），于是同一张果冻缓冲一帧里
 * 被两套倍率轮流覆盖：1x 那份铺到 2x 屏上是横纹，两套源来回换就是闪烁
 * （用户 2026-11-03 报的那两条）。取「最高倍率」这个规则跟帧序无关，
 * 跨屏来回拖也永远是同一份。
 *
 * 倍率相同（两块 1x 屏）时按 bake_output → view->output 排，纯粹为了挑得
 * 稳定。 */
static struct wlr_scene_buffer *jelly_source_node(struct vidow_view *view,
		double *scale, struct vidow_blur_slot **slot) {
	struct vidow_fx_target *fx = &view->fx;
	if (slot != NULL) {
		*slot = NULL;
	}
	if (!fx->active) {
		return NULL;
	}
	struct vidow_blur_slot *best = NULL;
	double best_scale = -1.0;
	int best_rank = -1;
	for (struct vidow_blur_slot *sl = fx->blur_slot; sl; sl = sl->next) {
		if (sl->node == NULL || !sl->node->node.enabled ||
				sl->node->buffer == NULL || !sl->dec_valid) {
			continue;
		}
		double sc = sl->dec_scale > 0.0 ? sl->dec_scale : 1.0;
		int rank = sl->out == fx->bake_output ? 2 :
			sl->out == view->output ? 1 : 0;
		if (best == NULL || sc > best_scale + 1e-6 ||
				(fabs(sc - best_scale) <= 1e-6 && rank > best_rank)) {
			best = sl;
			best_scale = sc;
			best_rank = rank;
		}
	}
	if (best != NULL) {
		*scale = best->dec_scale;
		if (slot != NULL) {
			*slot = best;
		}
		return best->node;
	}
	if (fx->node != NULL && fx->node->node.enabled &&
			fx->node->buffer != NULL) {
		*scale = fx->scale;
		return fx->node;
	}
	return NULL;
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

/* 同一格**没形变时**的位置：只有 margin + step*index，不含 ox/oy 位移。
 * 模糊 UV 用它 —— 底衬是屏幕上已经烘好的背景，不参与形变（见 render 里那段）。 */
static double jelly_rest_xf(struct vidow_jelly *jelly, int col) {
	double step = (double)jelly->src_w / (double)jelly->cols;
	return jelly->margin_px + step * (double)col;
}

static double jelly_rest_yf(struct vidow_jelly *jelly, int row) {
	double step = (double)jelly->src_h / (double)jelly->rows;
	return jelly->margin_px + step * (double)row;
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
	if (jelly->shadow_tex != NULL) {
		wlr_texture_destroy(jelly->shadow_tex);
		jelly->shadow_tex = NULL;
	}
	if (jelly->shadow_buf != NULL) {
		wlr_buffer_drop(jelly->shadow_buf);
		jelly->shadow_buf = NULL;
	}
	free(jelly->verts);
	free(jelly->blur_verts);
	free(jelly->verts2);
	free(jelly->blur_verts2);
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

/* 果冻画出来的那块东西比窗口本身大多少（**逻辑**像素）。
 *
 * 果冻节点显示的是「整窗装饰 + 四周 JELLY_MARGIN 的形变余量」，网格弯出去
 * 也不会超过这圈余量（jelly_step 里把偏移夹在 ±JELLY_MARGIN）。所以擦旧位置、
 * 报「这一块变了」时按窗口框外扩这么多才是果冻真正占过的地方；只报窗口框的话
 * 那一圈半透明留白 / 投影没人重抓，留在别的窗口底衬里就是滞留像素（用户
 * 2026-10-30：「窗口释放的时候 背景模糊更新会有滞留像素」）。 */
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
	/* 果冻的源是 fx 那份带装饰的离屏图：没有它就没东西可弯（比如装饰和
	 * shader 都关着）。 */
	double source_scale = 1.0;
	struct wlr_scene_buffer *source =
		jelly_source_node(view, &source_scale, NULL);
	if (source == NULL) {
		return false;
	}
	/* 入场动画还在缩放 / overview 里：这两条路上装饰节点的位置尺寸每帧都在变，
	 * 网格对不上，干脆不做（拖完这一次还是好的）。 */
	if (fabs(view->anim_scale - 1.0) > 0.05 ||
			fabs(vidow_overview_scale(server) - 1.0) > 0.01) {
		return false;
	}
	if (source->buffer->width < 8 || source->buffer->height < 8) {
		return false;
	}

	int tx = 0, ty = 0, fx_x = 0, fx_y = 0;
	wlr_scene_node_coords(&view->scene_tree->node, &tx, &ty);
	wlr_scene_node_coords(&source->node, &fx_x, &fx_y);

	struct vidow_jelly *jelly = calloc(1, sizeof(*jelly));
	if (jelly == NULL) {
		return false;
	}
	jelly->server = server;
	jelly->view = view;
	jelly->src_w = source->buffer->width;
	jelly->src_h = source->buffer->height;
	jelly->scale = source_scale > 0.0 ? source_scale : 1.0;
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

	/* 网格：按目标格子大小定，两边都夹住。
	 *
	 * 格子按**源的倍率**换算，所以 cols/rows 只跟窗口的**逻辑**尺寸有关：
	 * 源从 1x 换成 2x（跨屏，见 jelly_source_node）时 src_w 翻倍、cell 也
	 * 翻倍，网格尺寸不变 —— jelly_rebase 才有得搬（它只搬同尺寸的弹簧
	 * 状态，尺寸一变就当场收手，那就是「跨屏果冻丢了」）。 */
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
	jelly->blur_verts = calloc((size_t)jelly->rows * (size_t)jelly->cols * 36,
		sizeof(float));
	jelly->verts2 = calloc((size_t)jelly->rows * (size_t)jelly->cols * 24,
		sizeof(float));
	jelly->blur_verts2 = calloc((size_t)jelly->rows * (size_t)jelly->cols * 36,
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
	if (jelly->verts == NULL || jelly->blur_verts == NULL ||
			jelly->verts2 == NULL || jelly->blur_verts2 == NULL ||
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

/* source 是**这一帧 tick 校验过的那一块**（不是重新挑一遍）：挑源和画源必须
 * 是同一次决定。以前这里自己再调一次 jelly_source_node()，跨屏时那两次调用
 * 可能给出两块不同倍率的缓冲 —— 网格按一块尺寸算、贴图却是另一块，画出来
 * 一格一格错位、两帧一换就是闪烁。 */
static void jelly_render(struct vidow_jelly *jelly,
		struct wlr_scene_buffer *source) {
	struct vidow_server *server = jelly->server;
	struct wlr_buffer *src = source != NULL ? source->buffer : NULL;
	if (jelly->view != NULL && vidow_xtrace_on()) {
		vidow_gtrace("jellydraw v=%d buf=%dx%d srcbuf=%dx%d scale=%.2f",
			jelly->view->id, jelly->buf_w, jelly->buf_h,
			src != NULL ? src->width : 0, src != NULL ? src->height : 0,
			jelly->scale);
	}
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
	 * 直接给小数）—— 相邻格边界严丝合缝，既不会留缝也不会重叠。
	 *
	 * 同一份网格同时算两套 UV：
	 *   内容 UV  = 源缓冲里的位置（跟着网格形变，窗口内容照常弯）；
	 *   模糊 UV  = 顶点**当前屏幕坐标**映射到 blur 贴图（背景不被网格拉长）。
	 * 屏幕坐标 = 果冻节点在布局里的逻辑坐标 × 输出倍率 + 顶点在目标缓冲里的
	 * 物理像素偏移；再减掉输出原点 / 私有底衬区域原点。 */
	struct vidow_fx_target *fx = &jelly->view->fx;
	/* 只在「这个窗口确实该有模糊」时才走屏幕空间那一层：规则关掉模糊 /
	 * 客户端只要一块区域（ext-background-effect）时退回烘进内容图的那一版，
	 * 别把整窗都糊上。 */
	/* **底衬有没有，要看格子，不能只看老的单值字段。**
	 *
	 * 按屏分格之后（server.h 的 vidow_blur_slot），vidow_blur_update 走的是
	 * blur_assign(fx, out, ...)，只写那一格；fx->blur_tex 一直是 NULL。
	 * 以前这里只查 fx->blur_tex，于是果冻永远以为「没有底衬」：不画模糊层，
	 * 而内容图那边（fx.c 的 look.blur）也跟着以为没有底衬、把模糊照旧烘进
	 * 装饰缓冲 —— 那块缓冲标的是**归属屏**的底衬，果冻再把它整块铺到两块屏
	 * 上，另一半就是「用着对面那块屏的底衬」（2026-11-02 复现：跨屏拖拽时
	 * 右半底衬是左屏壁纸的颜色）。 */
	struct vidow_blur_slot *bslot = blur_slot_for(fx, fx->bake_output, false);
	bool has_slot_tex = false;
	for (struct vidow_blur_slot *sl = fx->blur_slot; sl; sl = sl->next) {
		if (sl->tex != NULL) {
			has_slot_tex = true;
			break;
		}
	}
	struct wlr_texture *blur1 = bslot != NULL && bslot->tex != NULL ?
		bslot->tex : fx->blur_tex;
	bool blur_ok = fx->blur && (has_slot_tex || fx->blur_tex != NULL) &&
		jelly->blur_verts != NULL && !fx->blur_region_on &&
		vidow_view_blur_wanted(jelly->view);
	/* 单份模式（只有一块屏 / 另一格还没烘出底衬）用的那套原点与区域：
	 * 优先 bake_output 那一格，没有格子才退回老的单值字段。 */
	int out_x = fx->blur_out_w > 0 ? fx->blur_out_x : server->output_x;
	int out_y = fx->blur_out_h > 0 ? fx->blur_out_y : server->output_y;
	int out_w = fx->blur_out_w > 0 ? fx->blur_out_w : server->output_w;
	int out_h = fx->blur_out_h > 0 ? fx->blur_out_h : server->output_h;
	int rg1_x = fx->blur_tex_x, rg1_y = fx->blur_tex_y;
	if (bslot != NULL && bslot->tex != NULL) {
		out_x = bslot->ox;
		out_y = bslot->oy;
		out_w = bslot->ow;
		out_h = bslot->oh;
		rg1_x = bslot->rx;
		rg1_y = bslot->ry;
	}
	int bw = bslot != NULL && bslot->tex != NULL && bslot->rw > 0 ?
		bslot->rw : (fx->blur_tex_w > 0 ? fx->blur_tex_w : out_w);
	int bh = bslot != NULL && bslot->tex != NULL && bslot->rh > 0 ?
		bslot->rh : (fx->blur_tex_h > 0 ? fx->blur_tex_h : out_h);
	if (bw < 1) bw = 1;
	if (bh < 1) bh = 1;
	/* 节点位置是**逻辑**像素（场景图就是逻辑像素），乘 s 换到输出的设备像素，
	 * 才和底衬贴图（按设备像素抓）同一个口径。实测 2 倍屏：tree=486、节点
	 * =402，差 84 正是 margin(72)+阴影(12) 的逻辑偏移 —— 说明 coords 不含
	 * 倍率，必须乘。 */
	int node_x = 0, node_y = 0;
	if (blur_ok) {
		wlr_scene_node_coords(&jelly->node->node, &node_x, &node_y);
	}
	double s = jelly->scale > 0.0 ? jelly->scale : 1.0;

	/* 跨屏骑边界：底衬按屏各存一份（server.h 的 struct vidow_blur_slot），
	 * 窗口两半各压在一块屏上时，每一半都得采**自己压着的那块屏**的底衬 ——
	 * 只按归属屏算一份 UV 的话，旧屏那一半采到的是新屏的底衬坐标（
	 * 2026-10-30 用户报的「跨界的时候背景出错」）。判出两块屏就拆两趟画：
	 * 每个三角形按静止位置归到它压着的那块屏，第二趟不清屏、画进同一块
	 * 目标缓冲。单屏（绝大多数情况）照旧一条路；三块屏骑一窗回退单份。 */
	struct vidow_blur_slot *rpass[2] = {NULL, NULL};
	int npass = 0;
	if (blur_ok) {
		struct wlr_box jrect = {node_x, node_y, jelly->dst_w, jelly->dst_h};
		struct vidow_output *out;
		wl_list_for_each(out, &server->outputs, link) {
			if (!out->wlr_output->enabled) {
				continue;
			}
			struct wlr_box ob = {0}, hit;
			wlr_output_layout_get_box(server->output_layout,
				out->wlr_output, &ob);
			if (ob.width < 1 || ob.height < 1 ||
					!wlr_box_intersection(&hit, &jrect, &ob)) {
				continue;
			}
			if (npass >= 2) {
				npass = 0;
				break;
			}
			rpass[npass++] = blur_slot_for(fx, out->wlr_output, false);
		}
		/* 格子里的底衬有两种：整屏共享那份（rw == 0，贴图就是整块屏的糊壁纸）
		 * 和私有区域那份（rw > 0，贴图只是窗口背后那一块）。两种都能两趟画 ——
		 * 整屏那份当成「区域 = 0,0 ow×oh」。 */
		if (npass != 2 || rpass[0] == NULL || rpass[1] == NULL ||
				rpass[0]->tex == NULL || rpass[1]->tex == NULL) {
			npass = 0; /* 哪一块还没烘出底衬：这一帧退回单份（旧行为） */
		}
		for (int p = 0; npass == 2 && p < 2; p++) {
			if (rpass[p]->rw < 1 &&
					(rpass[p]->ow < 1 || rpass[p]->oh < 1)) {
				npass = 0; /* 连屏有多大都不知道：没法算 UV */
			}
		}
	}

	int n1 = 0;          /* 第 1 趟（单份模式 = 全部）顶点数 */
	int n2 = 0;          /* 第 2 趟顶点数（仅两份模式） */
	for (int i = 0; i < jelly->rows; i++) {
		for (int j = 0; j < jelly->cols; j++) {
			float xs[6] = {
				(float)jelly_node_xf(jelly, i, j),
				(float)jelly_node_xf(jelly, i, j + 1),
				(float)jelly_node_xf(jelly, i + 1, j),
				(float)jelly_node_xf(jelly, i, j + 1),
				(float)jelly_node_xf(jelly, i + 1, j + 1),
				(float)jelly_node_xf(jelly, i + 1, j),
			};
			float ys[6] = {
				(float)jelly_node_yf(jelly, i, j),
				(float)jelly_node_yf(jelly, i, j + 1),
				(float)jelly_node_yf(jelly, i + 1, j),
				(float)jelly_node_yf(jelly, i, j + 1),
				(float)jelly_node_yf(jelly, i + 1, j + 1),
				(float)jelly_node_yf(jelly, i + 1, j),
			};
			float u0 = (float)j / (float)jelly->cols;
			float u1 = (float)(j + 1) / (float)jelly->cols;
			float v0 = (float)i / (float)jelly->rows;
			float v1 = (float)(i + 1) / (float)jelly->rows;
			float us[6] = { u0, u1, u0, u1, u1, u0 };
			float vs[6] = { v0, v0, v1, v0, v1, v1 };
			/* 6 个顶点依次是：(i,j) (i,j+1) (i+1,j) (i,j+1)
			 * (i+1,j+1) (i+1,j) —— 两个三角形：0-2 和 3-5。 */
			static const int dx6[6] = {0, 1, 0, 1, 1, 0};
			static const int dy6[6] = {0, 0, 1, 0, 1, 1};
			/* 每个顶点的静止屏幕位置。模糊 UV 和跨屏归堆都用它。
			 * **不能用形变后的 xs[]**：xs 里含弹簧位移，拿它算 UV 等于让
			 * 底衬跟着网格一起弯 —— 用户报的「背景跟着窗口扭曲」。底衬
			 * 是屏幕上烘好的背景，不参与形变：顶点滑到哪，就采它此刻压住
			 * 的那块背景像素。
			 *
			 * **归堆必须用布局逻辑坐标，不能先按一个倍率换成设备像素。**
			 * 两块屏倍率不同（1x / 2x）时，「逻辑 × 单倍率」得到的是哪个
			 * 坐标系都不是：它既对不上 1x 屏的设备原点，也对不上 2x 屏的
			 * 设备原点，所有三角形都会掉进「缝隙」分支被塞给第 1 趟 ——
			 * 右半就采了左屏的底衬（2026-11-02 复现：跨屏拖拽时右半
			 * 底衬是左屏壁纸的颜色）。逻辑坐标只有一份，先归堆，再在每一
			 * 趟里按**那块屏自己的倍率**换成设备像素。 */
			float lx6[6], ly6[6];
			/* 单份模式退回去用的设备像素坐标（口径 = 当前源缓冲倍率）。 */
			float sx6[6], sy6[6];
			if (blur_ok) {
				for (int k = 0; k < 6; k++) {
					lx6[k] = (float)node_x +
						(float)(jelly_rest_xf(jelly, j + dx6[k]) / s);
					ly6[k] = (float)node_y +
						(float)(jelly_rest_yf(jelly, i + dy6[k]) / s);
					sx6[k] = (float)((double)node_x * s) +
						(float)jelly_rest_xf(jelly, j + dx6[k]);
					sy6[k] = (float)((double)node_y * s) +
						(float)jelly_rest_yf(jelly, i + dy6[k]);
				}
			}
			for (int t = 0; t < 2; t++) {
				/* 这个三角形归哪块屏：3 个顶点静止位置的均值落进哪块屏
				 * 的**布局框**（逻辑坐标），就用那块屏的底衬。 */
				struct vidow_blur_slot *sl = NULL;
				if (npass == 2) {
					float cx = 0.0f, cy = 0.0f;
					for (int k = 0; k < 3; k++) {
						cx += lx6[t * 3 + k];
						cy += ly6[t * 3 + k];
					}
					cx /= 3.0f;
					cy /= 3.0f;
					for (int p = 0; p < 2 && sl == NULL; p++) {
						struct wlr_box lb = rpass[p]->layout_box;
						if (lb.width <= 0 || lb.height <= 0) {
							wlr_output_layout_get_box(
								server->output_layout,
								rpass[p]->out, &lb);
						}
						if (cx >= (float)lb.x &&
								cx < (float)(lb.x + lb.width) &&
								cy >= (float)lb.y &&
								cy < (float)(lb.y + lb.height)) {
							sl = rpass[p];
						}
					}
					if (sl == NULL) {
						sl = rpass[0]; /* 缝隙上：归第 1 趟，别丢顶点 */
					}
				}
				bool second = sl != NULL && sl == rpass[1];
				float *dv = second ? jelly->verts2 : jelly->verts;
				float *db = second ? jelly->blur_verts2 : jelly->blur_verts;
				int *cnt = second ? &n2 : &n1;
				/* 两份模式：这一趟要按**它自己那块屏**的倍率把逻辑坐标换成
				 * 设备像素，再用那一格的区域做 UV（槽位的 ox / rx 都是这套
				 * 口径，见 server.h 的 vidow_blur_slot）。 */
				struct wlr_box lbsl = {0};
				double sp = 1.0;
				int prx = 0, pry = 0, prw = 1, prh = 1;
				if (npass == 2) {
					lbsl = sl->layout_box;
					if (lbsl.width <= 0 || lbsl.height <= 0) {
						wlr_output_layout_get_box(server->output_layout,
							sl->out, &lbsl);
					}
					if (sl->out != NULL && sl->out->scale > 0.0) {
						sp = sl->out->scale;
					}
					/* rw == 0 = 整屏共享那份：区域就是整块屏。 */
					if (sl->rw > 0) {
						prx = sl->rx;
						pry = sl->ry;
						prw = sl->rw;
						prh = sl->rh;
					} else {
						prw = sl->ow > 0 ? sl->ow : 1;
						prh = sl->oh > 0 ? sl->oh : 1;
					}
				}
				/* 这一趟的底衬原点 / 区域（设备像素）：两份模式按各自
				 * 的屏，单份模式照旧用 fx 里 bake_output 那一份。 */
				int uo_x = npass == 2 ? sl->ox : out_x;
				int uo_y = npass == 2 ? sl->oy : out_y;
				int ut_x = npass == 2 ? sl->rx : rg1_x;
				int ut_y = npass == 2 ? sl->ry : rg1_y;
				int ut_w = npass == 2 ? sl->rw : bw;
				int ut_h = npass == 2 ? sl->rh : bh;
				for (int k = 0; k < 3; k++) {
					int kk = t * 3 + k;
					float *w = dv + (size_t)*cnt * 4;
					w[0] = xs[kk];
					w[1] = ys[kk];
					w[2] = us[kk];
					w[3] = vs[kk];
					if (blur_ok) {
						float *b = db + (size_t)*cnt * 6;
						b[0] = xs[kk];
						b[1] = ys[kk];
						b[2] = us[kk];
						b[3] = vs[kk];
						if (npass == 2) {
							b[4] = (float)(((double)lx6[kk] -
								(double)lbsl.x) * sp -
								(double)prx) / (float)prw;
							b[5] = (float)(((double)ly6[kk] -
								(double)lbsl.y) * sp -
								(double)pry) / (float)prh;
						} else {
							b[4] = (sx6[kk] - (float)uo_x -
								(float)ut_x) / (float)ut_w;
							b[5] = (sy6[kk] - (float)uo_y -
								(float)ut_y) / (float)ut_h;
						}
					}
					(*cnt)++;
				}
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
		/* 目标缓冲比 fx 缓冲四周多一圈 JELLY_MARGIN，窗口矩形在目标
		 * 坐标里要加上这圈留白；漏了它模糊就会偏进阴影 / 圆角外面。 */
		/* 遮罩用**内容 UV**（源缓冲坐标），不用形变后的目标坐标：果冻把
		 * 网格拉弯时窗口形状跟着弯，但投影永远在内容矩形外面，按源坐标
		 * 裁就不会把阴影圈进模糊里。 */
		float shadow_pm[4] = {
			look.shadow_color[0] * look.shadow_color[3],
			look.shadow_color[1] * look.shadow_color[3],
			look.shadow_color[2] * look.shadow_color[3],
			look.shadow_color[3],
		};

		if (npass == 2) {
			/* 两趟：每趟只画自己那一半三角形，用那块屏自己的底衬贴图和
			 * 原点；第二趟不清屏（clear=false），接着第一趟画进同一块
			 * 目标缓冲。 */
			ok = vidow_fx_draw_mesh_blur(server, target, jelly->buf_w,
				jelly->buf_h, tex, rpass[0]->tex,
				(float)server->config.blur_strength, jelly->blur_verts, n1,
				fx->px_pad, fx->px_pad, fx->content_w, fx->content_h,
				fx->w, fx->h, (float)(look.corner_radius * s),
				(float)vidow_corner_n_clamp(look.corner_n),
				jelly->mask_tex, fx->tex_off_x, fx->tex_off_y,
				fx->tex_scale_x, fx->tex_scale_y, 1.0f / 255.0f,
				jelly->shadow_tex,
				(int)jelly->margin_px + fx->px_pad,
				(int)jelly->margin_px + fx->px_pad,
				fx->content_w, fx->content_h,
				(float)(look.shadow_offset_x * s),
				(float)(look.shadow_offset_y * s),
				(float)(look.shadow_radius * s), shadow_pm, false, true);
			if (ok) {
				ok = vidow_fx_draw_mesh_blur(server, target, jelly->buf_w,
					jelly->buf_h, tex, rpass[1]->tex,
					(float)server->config.blur_strength,
					jelly->blur_verts2, n2,
					fx->px_pad, fx->px_pad, fx->content_w, fx->content_h,
					fx->w, fx->h, (float)(look.corner_radius * s),
					(float)vidow_corner_n_clamp(look.corner_n),
					jelly->mask_tex, fx->tex_off_x, fx->tex_off_y,
					fx->tex_scale_x, fx->tex_scale_y, 1.0f / 255.0f,
					jelly->shadow_tex,
					(int)jelly->margin_px + fx->px_pad,
					(int)jelly->margin_px + fx->px_pad,
					fx->content_w, fx->content_h,
					(float)(look.shadow_offset_x * s),
					(float)(look.shadow_offset_y * s),
					(float)(look.shadow_radius * s), shadow_pm, false,
					false);
			}
		} else {
		ok = vidow_fx_draw_mesh_blur(server, target, jelly->buf_w,
			jelly->buf_h, tex, blur1,
			(float)server->config.blur_strength, jelly->blur_verts, n1,
			fx->px_pad, fx->px_pad, fx->content_w, fx->content_h,
			fx->w, fx->h, (float)(look.corner_radius * s),
			(float)vidow_corner_n_clamp(look.corner_n),
			jelly->mask_tex, fx->tex_off_x, fx->tex_off_y,
			fx->tex_scale_x, fx->tex_scale_y, 1.0f / 255.0f,
			jelly->shadow_tex,
			(int)jelly->margin_px + fx->px_pad,
			(int)jelly->margin_px + fx->px_pad,
			fx->content_w, fx->content_h,
			(float)(look.shadow_offset_x * s),
			(float)(look.shadow_offset_y * s),
			(float)(look.shadow_radius * s), shadow_pm, false, true);
		}
	}
	if (!ok) {
		ok = vidow_fx_draw_mesh(server, target, jelly->buf_w, jelly->buf_h,
			tex, jelly->verts, n1);
	}
	wlr_texture_destroy(tex);
	if (!ok) {
		return;
	}

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, jelly->buf_w, jelly->buf_h);
	wlr_scene_buffer_set_buffer_with_damage(jelly->node, target, &damage);
	pixman_region32_fini(&damage);
	/* 果冻网格是同一张内容贴图的形变拷贝，编码跟着窗口表面走。 */
	struct vidow_view *jv = jelly->view;
	if (jv != NULL && jv->scene_surface != NULL &&
			jv->scene_surface->buffer != NULL) {
		vidow_color_inherit_tags(jelly->node, jv->scene_surface->buffer);
	}
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
static bool jelly_rebase(struct vidow_view *view) {
	struct vidow_jelly *old = view->jelly;
	if (old == NULL) {
		return false;
	}
	int onx = old->nx, ony = old->ny, opin = old->pin;
	size_t on = (size_t)onx * (size_t)ony;
	float *state = malloc(on * 6 * sizeof(float));
	if (state == NULL) {
		return false;
	}
	float *src[6] = {old->ox, old->oy, old->vx, old->vy, old->lx, old->ly};
	for (int a = 0; a < 6; a++) {
		memcpy(state + (size_t)a * on, src[a], on * sizeof(float));
	}
	double omax = old->last_max_off, osum = old->last_sum_off;
	int64_t orel = old->release_ns;

	bool ok = vidow_jelly_begin(view);
	struct vidow_jelly *nj = view->jelly;
	if (ok && nj != NULL && nj->nx == onx && nj->ny == ony) {
		float *dst[6] = {nj->ox, nj->oy, nj->vx, nj->vy, nj->lx, nj->ly};
		for (int a = 0; a < 6; a++) {
			memcpy(dst[a], state + (size_t)a * on, on * sizeof(float));
		}
		if (opin >= 0 && (size_t)opin < on) {
			nj->pin = opin;
		}
		nj->last_max_off = omax;
		nj->last_sum_off = osum;
		nj->release_ns = orel;
	}
	free(state);
	if (nj != NULL) {
		nj->rebase_ns = vidow_now_ns();
	}
	return nj != NULL;
}

void vidow_jelly_tick(struct vidow_server *server, int64_t now_ns) {
	struct vidow_view *view, *tmp;
	wl_list_for_each_safe(view, tmp, &server->views, link) {
		struct vidow_jelly *jelly = view->jelly;
		if (jelly == NULL) {
			continue;
		}
		double source_scale = 1.0;
		struct wlr_scene_buffer *source =
			jelly_source_node(view, &source_scale, NULL);
		/* Validate the same instance we sample, not the last output baked. */
		if (!view->mapped || view->scene_tree == NULL || source == NULL) {
			vidow_jelly_end(view);
			continue;
		}
		if (source->buffer->width != jelly->src_w ||
				source->buffer->height != jelly->src_h ||
				fabs(source_scale - jelly->scale) > 1e-3) {
			/* 源换了一块屏的倍率（跨屏）：按新源重建，别把果冻收掉
			 * （见 jelly_rebase）。节流期内（光标压在边界上、归属屏来回翻）
			 * **等一等**，别收手 —— 收了手果冻就没了，等归属稳定下来再重建，
			 * 用户看到的就是「跨过去之后果冻丢了」（2026-10-30）。重建真的
			 * 失败（起手条件不满足）才收。 */
			if (now_ns - jelly->rebase_ns >=
					(int64_t)JELLY_REBASE_MS * 1000000LL) {
				if (!jelly_rebase(view)) {
					vidow_jelly_end(view);
				}
			}
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
		wlr_scene_node_coords(&source->node, &fx_x, &fx_y);
		jelly->last_rel_x = fx_x - tx;
		jelly->last_rel_y = fx_y - ty;
		wlr_scene_node_set_position(&jelly->node->node,
			jelly->last_rel_x - (int)lround(JELLY_MARGIN),
			jelly->last_rel_y - (int)lround(JELLY_MARGIN));
		/* 跟手探针（touch /tmp/vidow-x11-trace 开）：果冻节点到底摆在哪、
		 * 源节点在哪、树在哪。拖动时窗口看着不动就是这三者里有一个没跟上。 */
		if (vidow_xtrace_on()) {
			int jn_x = 0, jn_y = 0, tr_x = 0, tr_y = 0;
			wlr_scene_node_coords(&jelly->node->node, &jn_x, &jn_y);
			wlr_scene_node_coords(&view->scene_tree->node, &tr_x, &tr_y);
			vidow_gtrace("jelly v=%d tree=%d,%d src=%d,%d rel=%d,%d "
				"node=%d,%d rel_now=%d,%d off=%.0f,%.0f scale=%.2f "
				"srcbuf=%dx%d",
				view->id, tx, ty, fx_x, fx_y,
				jelly->last_rel_x, jelly->last_rel_y, jn_x, jn_y,
				jelly->node->node.x, jelly->node->node.y,
				view->anim_off_x, view->anim_off_y, jelly->scale,
				source->buffer != NULL ? source->buffer->width : 0,
				source->buffer != NULL ? source->buffer->height : 0);
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
			/* 节点在树里的相对位置固定，只有内容在变：重画一次。 */
			jelly_render(jelly, source);
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
