#include "server.h"
#include "ipc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

/* niri 风格的 overview：把一块屏上的**所有工作区**上下排成一列看全景，每块工作区
 * 缩成一条「这块屏」那么大的区域，点谁就去谁。
 *
 * 场景是全局的，所以这里不动布局、不改客户端尺寸，只算一个「工作区局部 -> 屏幕」
 * 的变换（每块工作区一行，见 server.h 里 struct vidow_overview 的注释），view.c 的
 * vidow_view_apply_anim 负责把它套到每个窗口的场景节点上。
 *
 * 三件事各归各的输入：
 *   - 列表纵向滚动（list_scroll）：滚轮，一次一格；
 *   - 行内横向滚动：跟着**那块工作区自己的视口**（焦点在工作区里换窗口时它自己会
 *     动，见 view.c 的 output_arrange Pass 2）—— 所以「移动窗口焦点的键」动的是行内
 *     预览，不是整条相机乱跑；
 *   - 每行的区域 / 背景板（plate）：让「上下排布」看得见。
 *
 * t = 0 时必须是**恒等变换**：进出的动画都在 t 上插值，t = 0 那一端不能有跳变。 */

/* 行间距（屏幕像素，写死）：列表的「一条一条」就靠它 + 每行的背景板看出来。 */
#define OV_ROW_GAP 12
/* 一行的最小屏幕高（占工作区的比例）。工作区多到纵向排不下时行不再继续缩，列表就
 * 比屏高 —— 滚轮才有得滚（见 vidow_overview_scroll）。 */
#define OV_ROW_MIN_SCALE 0.25
/* 固定行高（占可用高度的比例）：不按行数平分，见下面那段注释。 */
#define OV_ROW_FIXED_SCALE 0.46
/* 背景板的浓度：遮罩 alpha 的多少倍（写死，不另加配置项）。**必须跟着遮罩走**：
 * overview_dim_color 的 alpha = 0（不压暗壁纸）时它得跟着一起没有，壁纸像素原样
 * （test-overview.sh 拿壁纸像素断言这条）。 */
#define OV_PLATE_ALPHA 0.25

bool vidow_overview_active(const struct vidow_server *server) {
	return server->overview.t > 0.0 || server->overview.on;
}

/* 图层规则的 overview：盖在 overview_apply 里那几刀「整棵树一起开关」上面，
 * 逐个 surface 决定它在全景里露不露。
 *
 *   only  —— 只在全景里露出来（桌面时收起来）。**全景专用壁纸走这条**：跑第二个
 *           壁纸客户端（awww-daemon -n ov → namespace "awww-daemonov"），再
 *           vidow.rule{ namespace=…, overview="only" }。
 *           桌面时它的场景节点是关的，所以既不会盖住桌面壁纸，也不会进窗口的
 *           模糊底衬（底衬抓的就是这几棵树）。
 *   never —— 全景里不出现（比如全景里不想看见壁纸，只剩兜底色）。
 *
 * 默认 always（没写规则）保持 overview_apply 那套：background / bottom 层跟着进
 * 全景，top / overlay 层收起。对 top / overlay 层写 always / only 都没有意义 ——
 * 那两棵树在全景里是整棵关掉的，逐 surface 开不回来（要开得先改那几刀）。 */
static bool overview_layers_apply(struct vidow_server *server) {
	bool on = server->overview.t > 0.002;
	bool changed = false;
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->scene == NULL || ls->layer_surface == NULL ||
				ls->layer_surface->surface == NULL ||
				!ls->layer_surface->surface->mapped) {
			continue;
		}
		/* 抓底衬的时候 fx.c 会临时把某些 surface 的节点关掉（它要的底衬不能
		 * 含它自己），记在 capture_hidden 上 —— 那一笔不是我们的，别抢。 */
		if (ls->capture_hidden) {
			continue;
		}
		bool want;
		switch (ls->overview_mode) {
		case VIDOW_OVERVIEW_ONLY:
			want = on;
			break;
		case VIDOW_OVERVIEW_NEVER:
			want = !on;
			break;
		default:
			/* always：规则没写（或者刚从 only / never 改回来）—— 恢复默认的
			 * 「开着」。top / overlay 层在全景里由上面那几刀整棵树关掉，
			 * 逐 surface 点亮它也不起作用，所以这里一律 true 是安全的。 */
			want = true;
			break;
		}
		if (ls->scene->tree->node.enabled == want) {
			continue;
		}
		wlr_scene_node_set_enabled(&ls->scene->tree->node, want);
		changed = true;
	}
	return changed;
}

/* 规则改了就重算一遍（rule.c 的 apply_layer 调）。 */
void vidow_overview_refresh_layers(struct vidow_server *server) {
	/* 开关变了要重抓底衬：全景里看到的那张壁纸是**底衬**（模糊那份画在遮罩
	 * 下面），不重抓就还是上一张 —— 全景里另一张壁纸只在「开全景的那一刻」
	 * 换过来，抓的正是这一笔。 */
	if (overview_layers_apply(server)) {
		vidow_backdrop_damage(server, true);
	}
}

/* ------------------------------------------------- 行表（上下排布） */

/* 记下这块屏的工作区（摆行 / 遮罩都用它）。框是空的 = 这块屏还没就绪。 */
static bool overview_work_area(struct vidow_server *server,
		struct wlr_box *work) {
	struct vidow_overview *ov = &server->overview;
	if (ov->output == NULL) {
		return false;
	}
	struct wlr_box box = vidow_work_area(server, ov->output);
	if (box.width < 1 || box.height < 1) {
		return false;
	}
	ov->out_x = box.x;
	ov->out_y = box.y;
	ov->out_w = box.width;
	ov->out_h = box.height;
	if (work != NULL) {
		*work = box;
	}
	return true;
}

/* 这块工作区有没有东西可看（有窗口的画着，最小化的不算 —— 它在全景里是停着的）。
 * **空的工作区不摆行**：vidow 在每块屏的栈尾永远留一块空工作区（见 workspace.c 的
 * vidow_workspace_ensure_bottom），把它也摆一行的话「一块工作区 + 一个空位」就会
 * 各占半屏，真正要看的东西平白小一半，而那块空的什么信息都没有。当前这块工作区
 * 例外：空也摆（用户就在那儿，得看得见自己在哪）。 */
static bool ws_has_content(struct vidow_server *server,
		struct vidow_workspace *ws) {
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->ws != ws || view->popup || !view->mapped ||
				view->minimized) {
			continue;
		}
		if (view->geom.width < 1 || view->geom.height < 1) {
			continue;
		}
		return true;
	}
	return false;
}

/* 这块屏上有哪些工作区（上下排布的次序 = 工作区在列表里的先后，也就是配置里的声明
 * 顺序）。特殊工作区也算一行 —— 它的 output 只在「正显示着」时才指向这块屏，没显示
 * 的时候自然混不进来（见 workspace.c）。行表按需分配、复用（板子留着，下次进来接
 * 着用）。 */
static int rows_build(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	struct vidow_workspace *shown =
		vidow_workspace_shown_for(server, ov->output);
	int n = 0;
	ov->n_skipped = 0;
	struct vidow_workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->output != ov->output) {
			continue;
		}
		if (ws != shown && !ws_has_content(server, ws)) {
			ov->n_skipped++;
			continue;
		}
		if (n == ov->cap_rows) {
			int cap = ov->cap_rows > 0 ? ov->cap_rows * 2 : 4;
			struct vidow_ov_row *rows =
				realloc(ov->rows, (size_t)cap * sizeof(*rows));
			if (rows == NULL) {
				break; /* 内存没了：就摆这几行 */
			}
			memset(rows + ov->cap_rows, 0,
				(size_t)(cap - ov->cap_rows) * sizeof(*rows));
			ov->rows = rows;
			ov->cap_rows = cap;
		}
		ov->rows[n].ws = ws;
		n++;
	}
	ov->n_rows = n;
	return n;
}

static struct vidow_ov_row *row_of(struct vidow_server *server,
		struct vidow_workspace *ws) {
	struct vidow_overview *ov = &server->overview;
	if (ws == NULL) {
		return NULL;
	}
	for (int i = 0; i < ov->n_rows; i++) {
		if (ov->rows[i].ws == ws) {
			return &ov->rows[i];
		}
	}
	return NULL;
}

/* 这一行现在滚到哪儿了：显示着的那块工作区要用**当前**视口（它跟着焦点走，见
 * output_arrange 的 Pass 2），藏起来的那几块用它们自己离开时记下的。 */
static double row_viewport(struct vidow_server *server,
		struct vidow_workspace *ws) {
	struct vidow_overview *ov = &server->overview;
	if (ws == vidow_workspace_shown_for(server, ov->output)) {
		struct vidow_output *output = vidow_output_lookup(server, ov->output);
		if (output != NULL) {
			return output->viewport_x;
		}
	}
	return ws->viewport_x;
}

/* 每行条带多宽（工作区局部像素）：按列宽比例现算 —— 藏起来的工作区从来没被 arrange
 * 过，col->offset / view->geom 都是老值，照着画会叠在一起。**现在只喂日志**：一行是
 * 「一块缩小版的屏」，条带比屏宽的那些列在屏幕上本来就在屏外（见 row_rect_w）。 */
static void rows_scale(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	int work_w = ov->out_w, work_h = ov->out_h;
	if (ov->n_rows <= 0 || work_w < 1 || work_h < 1) {
		ov->scale = 1.0;
		ov->row_h = 0.0;
		return;
	}
	for (int i = 0; i < ov->n_rows; i++) {
		struct vidow_ov_row *row = &ov->rows[i];
		row->band_w = vidow_workspace_preview_layout(server, row->ws,
			work_w, work_h, NULL, NULL);
	}

	/* 缩略比例**固定**（OV_ROW_FIXED_SCALE）：纵向不按行数平分 —— 早先是
	 * 0.92 / n_rows，工作区一多每行就被摊薄、越开越小（用户：「overview 每个工作区
	 * 的大小应该固定，现在开启多个工作区的时候一直缩小」）。横向也不再按「最宽的
	 * 条带铺得进一行」算 fit_w：那个式子属于「整条带铺满一行」的旧模型，现在一行是
	 * 一块缩小版的屏，条带比屏宽的部分本来就该落在矩形（= 屏）外面。行数超过一屏
	 * 排不下就靠列表滚动看（list_scroll / rows_total）。 */
	ov->scale = OV_ROW_FIXED_SCALE;
	ov->row_h = (double)work_h * ov->scale;
}

/* 这块工作区现在住的那块输出。工作区是全局的一叠（屏拔了整块借住到别的屏上，见
 * workspace.h），所以比例和背景图都得看它**此刻**住的那块屏。没归属（刚建出来还没
 * 认领 / 屏刚拔）就退回 overview 锁的那块 —— 那正是进全景时聚焦的那块屏。 */
static struct vidow_output *row_output(struct vidow_server *server,
		const struct vidow_ov_row *row) {
	struct wlr_output *output = row->ws->output != NULL ?
		row->ws->output : server->overview.output;
	return output != NULL ? vidow_output_lookup(server, output) : NULL;
}

/* 这一行那块「缩小版的屏」有多宽：高就是行高（固定比例），宽 = 高 × 那块输出的逻辑
 * 长宽比 —— 用户要的就是「缩小版的屏幕矩形，而不是一整条横带」。 */
static int row_rect_w(struct vidow_server *server,
		const struct vidow_ov_row *row) {
	struct vidow_overview *ov = &server->overview;
	struct vidow_output *output = row_output(server, row);
	struct wlr_box box = {0};
	if (output != NULL) {
		wlr_output_layout_get_box(server->output_layout,
			output->wlr_output, &box);
	}
	if (box.width < 1 || box.height < 1) {
		box.width = ov->out_w;
		box.height = ov->out_h;
	}
	int w = (int)lround(ov->row_h * (double)box.width / (double)box.height);
	return w > 0 ? w : 1;
}

static double rows_total(const struct vidow_overview *ov) {
	return (double)ov->n_rows * ov->row_h
		+ (double)(ov->n_rows > 0 ? ov->n_rows - 1 : 0) * OV_ROW_GAP;
}

/* 背景图那块 scene buffer 不吃输入：它盖在窗口上面（同一层里后建的在上），默认会
 * 把所有点击吃掉 —— 返回 false 让命中测试继续往下走，落到窗口 / 遮罩上。 */
static bool ov_node_rejects_input(struct wlr_scene_buffer *buffer, double *sx,
		double *sy) {
	(void)buffer;
	(void)sx;
	(void)sy;
	return false;
}

/* 这块工作区的背景图：一块 scene buffer，画在板子上面（都在遮罩上面 —— 矩形就是
 * 「缩小版的屏」，它得跟屏幕上一样亮，不能再被压暗一次）。
 *
 * 图从哪来：vidow 不画壁纸（壁纸永远是 background 层客户端画的，见 README），合成器
 * 手上没有单独的「壁纸贴图」。离它最近的就是 fx.c 抓的那张**底衬**
 * （output->backdrop：把所有窗口藏起来渲染的一帧 = 壁纸 + background / bottom 层），
 * 正好是「这块屏平时显示的样子」，缩进矩形里就是要的那张背景图。模糊那份
 * （output->blur_tex）不能用：那是整屏垫在遮罩下面的糊壁纸。
 *
 * 底衬按设备像素抓，scene buffer 的 dest_size 一设就按逻辑尺寸缩放着画。拿不到
 * （没开 blur_enabled 就没有这张底衬）就什么都不画，退回只有板子。 */
static void row_bg_apply(struct vidow_server *server, struct vidow_ov_row *row) {
	struct vidow_overview *ov = &server->overview;
	bool on = ov->t > 0.002;
	struct vidow_output *output = row_output(server, row);
	struct wlr_buffer *buf = (on && output != NULL) ? output->backdrop : NULL;
	if (row->bg == NULL) {
		if (buf == NULL) {
			return;
		}
		row->bg = wlr_scene_buffer_create(server->layer_overview_dim, NULL);
		if (row->bg == NULL) {
			return;
		}
		/* 跟 fx.c 那块装饰 buffer 一个道理：普通 buffer 默认吃输入，盖在窗口上面
		 * 会把点击全吃掉 —— 返回 false 让命中测试继续往下走。 */
		row->bg->point_accepts_input = ov_node_rejects_input;
		/* 板子先建（plate_apply 在这一句之前），所以背景图盖在板子上面。 */
	}
	if (row->bg->buffer != buf) {
		/* 换图 = 换底衬那一版（窗口动了 / 壁纸重抓都会换缓冲）。set_buffer 会锁住
		 * 缓冲：退出时置 NULL（见 overview_done），不然那张整屏大的底衬会被一直
		 * 锁着不放（4K 上 33MB）。 */
		wlr_scene_buffer_set_buffer(row->bg, buf);
	}
	wlr_scene_node_set_enabled(&row->bg->node, buf != NULL);
	if (buf == NULL) {
		return;
	}
	wlr_scene_buffer_set_dest_size(row->bg, row->rect_w,
		(int)lround(ov->row_h));
	/* 跟窗口一样跟着 t 淡入淡出：不然进出全景的时候它会「啪」一下出现。 */
	wlr_scene_buffer_set_opacity(row->bg, (float)ov->t);
	wlr_scene_node_set_position(&row->bg->node, row->rect_x, row->y);
}

/* 这一行的区域 / 背景板：和遮罩同一层（窗口之下、背景图之下）。它现在是**矩形**
 * （缩小版的屏），不再是铺满一行的一条横带；浓度跟着遮罩的 alpha 走
 * （见 OV_PLATE_ALPHA）。背景图画不出来的地方（没开模糊 = 没底衬、壁纸留白）露的
 * 就是它。 */
static void plate_apply(struct vidow_server *server, struct vidow_ov_row *row) {
	struct vidow_overview *ov = &server->overview;
	if (row->plate == NULL) {
		row->plate = wlr_scene_rect_create(server->layer_overview_dim, 1, 1,
			(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
		if (row->plate == NULL) {
			return;
		}
		wlr_scene_node_set_enabled(&row->plate->node, false);
	}
	const float *dim = server->config.overview_dim_color;
	/* 场景的 rect 颜色是**预乘**的（跟 fx 那套着色器一样：配置里存直乘，用的时候
	 * 乘上 alpha）。踩过一次：直接给直乘颜色，场景当预乘用 —— 板子不减暗，反而
	 * 每通道加了 dim.rgb 那点亮度（0.02×255 ≈ +5）。 */
	float a = (float)(dim[3] * ov->t * OV_PLATE_ALPHA);
	float c[4] = { dim[0] * a, dim[1] * a, dim[2] * a, a };
	int h = (int)lround(ov->row_h);
	wlr_scene_rect_set_color(row->plate, c);
	wlr_scene_rect_set_size(row->plate, row->rect_w, h > 0 ? h : 1);
	wlr_scene_node_set_position(&row->plate->node, row->rect_x, row->y);
	wlr_scene_node_set_enabled(&row->plate->node, ov->t > 0.002 && a > 0.0f);
}

/* 把行摆到屏幕上：每行的顶边（含列表滚动）和条带原点（含行内横向滚动），再把每行的
 * 背景板摆好。t = 1 那一端的几何都在这儿定，vidow_overview_map 只按 t 插值。 */
static void rows_place(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	if (ov->n_rows <= 0 || ov->out_w < 1 || ov->out_h < 1) {
		return;
	}
	double total = rows_total(ov);
	double max_scroll = total - (double)ov->out_h;
	if (max_scroll < 0.0) {
		max_scroll = 0.0;
	}
	if (ov->list_scroll > max_scroll) {
		ov->list_scroll = max_scroll;
	}
	if (ov->list_scroll < 0.0) {
		ov->list_scroll = 0.0;
	}
	/* 排得下就上下居中（好看些），排不下才按 list_scroll 滚。 */
	double top = total <= (double)ov->out_h ?
		(double)ov->out_y + ((double)ov->out_h - total) / 2.0 :
		(double)ov->out_y - ov->list_scroll;

	for (int i = 0; i < ov->n_rows; i++) {
		struct vidow_ov_row *row = &ov->rows[i];
		row->y = (int)lround(top + (double)i * (ov->row_h + OV_ROW_GAP));
		/* 矩形 = 这块屏缩小 0.46 摆在这儿（横向居中）：宽按屏幕长宽比算，不再是
		 * 铺满一行的一条横带。 */
		row->rect_w = row_rect_w(server, row);
		row->rect_x = ov->out_x + (ov->out_w - row->rect_w) / 2;
		/* 行内横向滚动：跟着这块工作区自己的视口（焦点在工作区里换窗口时它自己
		 * 会动）。视口 0 = 条带左边缘贴着矩形左边（屏幕上第一列就在屏幕左边），
		 * 视口越大条带越往左滑 —— 画出来的就是「这块屏上看到的那一屏」。条带比
		 * 屏宽的那些列落在矩形外，跟屏幕上一样（屏幕上它们在屏幕外，这儿没法裁：
		 * 场景没有裁剪节点）。 */
		row->content_x = (double)row->rect_x -
			row_viewport(server, row->ws) * ov->scale;
		plate_apply(server, row);
		row_bg_apply(server, row);
	}
	/* 上次进来时行更多：多出来的板子 / 背景图关掉（留着下次用）。 */
	for (int i = ov->n_rows; i < ov->cap_rows; i++) {
		if (ov->rows[i].plate != NULL) {
			wlr_scene_node_set_enabled(&ov->rows[i].plate->node, false);
		}
		if (ov->rows[i].bg != NULL) {
			wlr_scene_node_set_enabled(&ov->rows[i].bg->node, false);
		}
	}
}

/* 进来先把列表滚到「现在这块工作区」那一行看得见的地方（列表比屏高时才用得上）。 */
static void rows_scroll_to_active(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	struct vidow_workspace *shown =
		vidow_workspace_shown_for(server, ov->output);
	double total = rows_total(ov);
	double max_scroll = total - (double)ov->out_h;
	if (max_scroll <= 0.0) {
		ov->list_scroll = 0.0;
		return;
	}
	for (int i = 0; i < ov->n_rows; i++) {
		if (ov->rows[i].ws != shown) {
			continue;
		}
		double want = (double)i * (ov->row_h + OV_ROW_GAP)
			- ((double)ov->out_h - ov->row_h) / 2.0;
		if (want > max_scroll) {
			want = max_scroll;
		}
		ov->list_scroll = want > 0.0 ? want : 0.0;
		return;
	}
	ov->list_scroll = 0.0;
}

/* 窗口在它那一行里的位置和尺寸（工作区局部坐标，不含行内横向滚动）。
 * 进列的窗口按列宽比例现算（藏起来的工作区从来没被 arrange 过）；浮窗 / 弹出物不进
 * 列，用它们自己的几何（浮窗不跟着条带滚，所以不减视口）。返回 false = 这个窗口在
 * 这一行里没有位置（最小化之类，全景里不画）。 */
struct row_local_ctx {
	struct vidow_view *want;
	bool found;
	int x, y, w, h;
};

static void row_local_cb(struct vidow_view *view, int x, int y, int w, int h,
		void *data) {
	struct row_local_ctx *ctx = data;
	if (view == ctx->want) {
		ctx->found = true;
		ctx->x = x;
		ctx->y = y;
		ctx->w = w;
		ctx->h = h;
	}
}

static bool row_view_rect(struct vidow_server *server,
		const struct vidow_ov_row *row, struct vidow_view *view,
		double *lx, double *ly, int *lw, int *lh) {
	if (view->popup) {
		return false;
	}
	if (view->float_mode != VIDOW_FLOAT_NONE || view->column == NULL) {
		*lx = view->geom.x - server->overview.out_x;
		*ly = view->geom.y - server->overview.out_y;
		*lw = view->geom.width;
		*lh = view->geom.height;
		return view->geom.width > 0 && view->geom.height > 0;
	}
	struct row_local_ctx ctx = { .want = view };
	vidow_workspace_preview_layout(server, row->ws, server->overview.out_w,
		server->overview.out_h, row_local_cb, &ctx);
	if (!ctx.found) {
		return false;
	}
	*lx = ctx.x;
	*ly = ctx.y;
	*lw = ctx.w;
	*lh = ctx.h;
	return true;
}

/* 算好这一帧的行布局，并把所有窗口按它摆一遍。 */
static void overview_apply(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	if (!overview_work_area(server, NULL)) {
		return;
	}

	double t = ov->t;
	if (t < 0.0) {
		t = 0.0;
	}
	if (t > 1.0) {
		t = 1.0;
	}

	/* 这一帧的缩略图：先按工作区列表把行摆出来（t = 1 那一端的几何），再按 t
	 * 插值（在 vidow_overview_map 里）。 */
	rows_build(server);
	rows_scale(server);
	rows_place(server);

	/* 遮罩：压暗壁纸，让缩小的窗口浮在上面。盖住那块屏（用它的整个布局框）。
	 * 颜色和浓度都是配置给的（overview_dim_color，见 17-overview.lua）。 */
	if (ov->dim != NULL) {
		struct wlr_box box = {0};
		wlr_output_layout_get_box(server->output_layout, ov->output, &box);
		if (box.width < 1) {
			box = (struct wlr_box){ .x = ov->out_x, .y = ov->out_y,
				.width = ov->out_w, .height = ov->out_h };
		}
		const float *c = server->config.overview_dim_color;
		float dim[4] = { c[0], c[1], c[2], (float)(c[3] * t) };
		wlr_scene_rect_set_color(ov->dim, dim);
		wlr_scene_rect_set_size(ov->dim, box.width, box.height);
		wlr_scene_node_set_position(&ov->dim->node, box.x, box.y);
		bool want = t > 0.002 && c[3] > 0.0f;
		if (ov->dim->node.enabled != want) {
			wlr_log(WLR_DEBUG, "overview: 遮罩 %s（t=%.3f，alpha=%.2f）",
				want ? "开" : "关", t, c[3] * t);
		}
		wlr_scene_node_set_enabled(&ov->dim->node, want);
	}

	/* 屏幕空间的东西在 overview 里没有意义：弹出物 / 通知 / 输入法候选框收起来。
	 * 关窗残影是世界坐标的、缩不了，也一起收。 */
	bool on = t > 0.002;
	wlr_scene_node_set_enabled(&server->layer_popups->node, !on);
	wlr_scene_node_set_enabled(&server->layer_top->node, !on);
	wlr_scene_node_set_enabled(&server->layer_overlay->node, !on);
	wlr_scene_node_set_enabled(&server->layer_im->node, !on);
	struct vidow_closing *closing;
	wl_list_for_each(closing, &server->closing, link) {
		if (closing->fx.node != NULL) {
			wlr_scene_node_set_enabled(&closing->fx.node->node, !on);
		}
	}

	/* 上面那几刀是整棵树一起开关；图层规则可以逐个 surface 覆盖它 */
	bool layers_changed = overview_layers_apply(server);

	/* 所有窗口重新摆一遍：变换在 vidow_view_apply_anim 里生效 */
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		vidow_view_apply_anim(view);
	}
	vidow_backdrop_damage(server, layers_changed);
}

/* ------------------------------------------------------------- 进出 */

/* 动画引擎给的 t 一般走在 0 → 1（见 anim.c），但**过冲曲线会让它冲过 1**（见下面
 * 的夹取）。方向由 on 决定，起点是「这次动画开始时 t 是多少」（ov->anim_from）。
 * **不能**把 t 直接当进度用 —— 那样退出会把「进入」重放一遍再瞬间跳回原位（用户报
 * 的「点选之后闪弹一下」：先弹回原样、又缩下来、最后啪一下再弹回去）。中途反向
 * （进到一半再按一次）也从当前进度接着走，不跳。 */
static void overview_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_server *server = data;
	struct vidow_overview *ov = &server->overview;
	double from = ov->anim_from;
	double v = ov->on ? from + (1.0 - from) * t : from * (1.0 - t);
	/* ov->t 是**进度（0..1）**，不是位置：它直接当透明度喂场景
	 * （row_apply 里 wlr_scene_buffer_set_opacity(row->bg, ov->t)）、乘遮罩
	 * alpha、还当各种 lerp 的系数，所以**必须夹在 0..1**。
	 * 「t 永远 0 → 1」只在不过冲的曲线下成立，而 easing 是可配的：out_back /
	 * out_elastic / "bezier … y>1" 都会让 t 冲过 1。实测（用户真机）：
	 * easing = "out_back 0.18"（冲到 1.18）时按 Mod+O 直接
	 * `wlr_scene_buffer_set_opacity: Assertion 'opacity >= 0 && opacity <= 1'
	 * failed` —— 整个合成器跟着死。夹住之后过冲段 = 「提前到 1 然后停住」，
	 * 也就是那条曲线在这件事上没有过冲（本该如此：进度没有过冲的余地）。
	 * NaN 也走第一支（!(v >= 0.0) 为真）。 */
	if (!(v >= 0.0)) {
		v = 0.0;
	} else if (v > 1.0) {
		v = 1.0;
	}
	ov->t = v;
	overview_apply(server);
}

static void overview_done(void *data, bool cancelled) {
	struct vidow_server *server = data;
	struct vidow_overview *ov = &server->overview;
	ov->anim = NULL;
	if (cancelled) {
		/* 被紧接着的新动画接管了：t / output 归新的那次管，别在这儿拍端点。 */
		return;
	}
	ov->t = ov->on ? 1.0 : 0.0;
	overview_apply(server);
	if (!ov->on) {
		ov->output = NULL;
		ov->list_scroll = 0.0;
		/* 收掉那张糊壁纸的缓冲（整屏那么大，4K 上两份 33MB）。场景节点留着
		 * ——它挂在 layer_overview_dim 那棵树上，下次进来接着用。 */
		vidow_fx_end(server, &ov->fx);
		ov->fx_on = false;
		/* 每行那块背景图也一样：**必须**把缓冲放掉（set_buffer 会锁住它），
		 * 不然那张整屏大的底衬被我们锁着，fx.c 换了新的一代它也不释放。 */
		for (int i = 0; i < ov->cap_rows; i++) {
			if (ov->rows[i].bg != NULL) {
				wlr_scene_node_set_enabled(&ov->rows[i].bg->node, false);
				wlr_scene_buffer_set_buffer(ov->rows[i].bg, NULL);
			}
		}
	}
	/* 进 / 出都重判一次「谁该出现在场景里」：overview 里**这块屏上所有工作区**
	 * 的窗口都要画（缩到画面里），所以平时按工作区藏起来的那些得先放回来；退出
	 * 之后又要按框剔除一次（不然它们会一直显示着，串到隔壁屏上）。 */
	vidow_workspace_apply_visibility(server);
}

static void overview_start(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	/* 起点先记下来：cancel 会回调 overview_done，把 t 拍成端点值。没在动画里
	 * 就是干净的两端 —— 进从 0 起，退从 1 起。 */
	double from = ov->anim != NULL ? ov->t : (ov->on ? 0.0 : 1.0);
	vidow_anim_cancel(ov->anim);
	ov->anim = NULL;
	ov->anim_from = from;
	ov->t = from;
	ov->anim = vidow_anim_start(server, "overview", server->config.window_move_ms,
		server->config.easing, overview_update, overview_done, server);
	if (ov->anim == NULL) {
		overview_done(server, false);
	}
}

void vidow_overview_toggle(struct vidow_server *server) {
	/* 会话锁（S3）：锁定期间不开概览 / 不响应概览上的点击和滚轮 —— 概览里
	 * 全是窗口预览，锁上以后它既不该出现也不该被操作（方案 §2 环节 3）。 */
	if (server->locked) {
		return;
	}
	struct vidow_overview *ov = &server->overview;
	if (ov->on) {
		vidow_overview_exit(server);
		return;
	}
	if (server->focused_output == NULL) {
		return; /* 一块屏都没有，没得看 */
	}
	ov->on = true;
	ov->output = server->focused_output;
	ov->list_scroll = 0.0;
	/* 切工作区的过渡还在滑就先收住：overview 按每块工作区自己的几何算缩略图，
	 * 正滑着的那块会摆歪（而且它下一帧就要被缩放动画接管）。 */
	vidow_workspace_slide_cancel(server);
	/* 缩下去的窗口不该还挂着指针焦点（否则客户端以为鼠标还在它身上） */
	wlr_seat_pointer_notify_clear_focus(server->seat);
	overview_start(server);

	/* 顺手把每块工作区、每个窗口在 overview 里的位置算出来打一行：一是让日志能
	 * 自证，二是测试要拿它做像素断言（缩放后的矩形）。打的是 t = 1 那一端的几何
	 * （map 按 t 插值，所以这些值跟动画进度无关）。 */
	if (overview_work_area(server, NULL)) {
		rows_build(server);
		rows_scale(server);
		rows_scroll_to_active(server);
		rows_place(server);

		int max_band = 0;
		int n_win = 0;
		struct vidow_view *view;
		wl_list_for_each(view, &server->views, link) {
			if (view->output != ov->output || !view->mapped) {
				continue;
			}
			n_win++;
		}
		for (int i = 0; i < ov->n_rows; i++) {
			if (ov->rows[i].band_w > max_band) {
				max_band = ov->rows[i].band_w;
			}
		}
		wlr_log(WLR_INFO, "overview: 进入（%s 上 %d 个窗口，占 %dx%d 缩到 %.3f，"
			"铺在工作区 %dx%d 里）", ov->output->name, n_win, max_band,
			ov->n_rows * ov->out_h, ov->scale, ov->out_w, ov->out_h);
		/* 每行的矩形（缩小版的屏）：宽按那块屏的长宽比算，行高固定 0.46 —— 日志
		 * 里能看到它、也能看到背景图有没有拿到（拿不到 = 那张底衬还没抓出来）。 */
		for (int i = 0; i < ov->n_rows; i++) {
			struct vidow_ov_row *row = &ov->rows[i];
			struct vidow_output *out = row_output(server, row);
			wlr_log(WLR_INFO, "overview: 工作区 %s 的矩形 %d,%d %dx%d（%s 的屏，"
				"背景图 %s）", row->ws->name[0] != '\0' ? row->ws->name : "(无名)",
				row->rect_x, row->y, row->rect_w, (int)lround(ov->row_h),
				out != NULL ? out->wlr_output->name : "?",
				out != NULL && out->backdrop != NULL ? "有" : "无");
		}
		if (ov->n_skipped > 0) {
			wlr_log(WLR_INFO, "overview: %d 块空工作区没摆行（没东西可看，"
				"摆了白占地方）", ov->n_skipped);
		}
		double total = rows_total(ov);
		if (total > (double)ov->out_h + 1.0) {
			wlr_log(WLR_INFO, "overview: %d 块工作区排下来 %.0f 高，比工作区 %d 高"
				" —— 滚轮上下滚列表", ov->n_rows, total, ov->out_h);
		}
		wl_list_for_each(view, &server->views, link) {
			if (view->output != ov->output || !view->mapped) {
				continue;
			}
			struct vidow_ov_row *row = row_of(server, view->ws);
			double lx = 0, ly = 0;
			int lw = 0, lh = 0;
			if (row == NULL || !row_view_rect(server, row, view, &lx, &ly,
					&lw, &lh)) {
				continue; /* 最小化的：全景里不画 */
			}
			wlr_log(WLR_INFO, "overview: 窗口 %s -> 屏幕 %d,%d %dx%d（缩放 %.3f）",
				vidow_view_title(view) != NULL ? vidow_view_title(view) : "(无题)",
				(int)lround(row->content_x + lx * ov->scale),
				(int)lround((double)row->y + ly * ov->scale),
				(int)lround((double)lw * ov->scale),
				(int)lround((double)lh * ov->scale), ov->scale);
		}
	} else {
		wlr_log(WLR_INFO, "overview: 进入（%s 上没有列）", ov->output->name);
	}
	vidow_ipc_emit_note(server, "overview-toggled");
}

void vidow_overview_exit(struct vidow_server *server) {
	struct vidow_overview *ov = &server->overview;
	if (!ov->on && ov->t <= 0.0) {
		return;
	}
	ov->on = false;
	ov->list_scroll = 0.0;
	overview_start(server);
	wlr_log(WLR_INFO, "overview: 退出");
	vidow_ipc_emit_note(server, "overview-toggled");
}

/* ------------------------------------------------------------- 坐标 */

void vidow_overview_map(struct vidow_server *server, struct vidow_view *view,
		double ox, double oy, double *sx, double *sy) {
	const struct vidow_overview *ov = &server->overview;
	/* 恒等那一端：窗口自己的位置 + 它自己的动画偏移（t = 0 必须一点不差）。 */
	double oxr = (double)(int)lround(ox);
	double oyr = (double)(int)lround(oy);
	double wx = view->geom.x + oxr;
	double wy = view->geom.y + oyr;
	*sx = wx;
	*sy = wy;
	if (ov->t <= 0.0 || ov->output == NULL || view->output != ov->output) {
		return; /* 不在这块屏上：原样（靠 alpha 淡出） */
	}
	const struct vidow_ov_row *row = row_of(server, view->ws);
	double lx = 0, ly = 0;
	int lw = 0, lh = 0;
	if (row == NULL || !row_view_rect(server, row, view, &lx, &ly, &lw, &lh)) {
		return; /* 不在这块屏的工作区里 / 最小化：原样 */
	}
	/* 展开那一端：这一行里的位置。中间在「原来在哪」和「行里在哪」之间线性插值
	 * —— 进出的动画都在 t 上走，t = 0 那一端正好是原样。 */
	double x1 = row->content_x + (lx + oxr) * ov->scale;
	double y1 = (double)row->y + (ly + oyr) * ov->scale;
	*sx = wx + (x1 - wx) * ov->t;
	*sy = wy + (y1 - wy) * ov->t;
}

double vidow_overview_scale(const struct vidow_server *server) {
	const struct vidow_overview *ov = &server->overview;
	if (ov->t <= 0.0) {
		return 1.0;
	}
	/* 缩放也按 t 插值：窗口的内容尺寸（base_dst × 这个值）得跟位置一起走，
	 * 不然 t 小的时候内容已经缩了、位置还在原处。 */
	return 1.0 + (ov->scale - 1.0) * ov->t;
}

/* ------------------------------------------------------------- 交互 */

/* 点一下：命中的那个窗口就选中（焦点给它，然后退出 overview 缩回去）。
 * 点在空白处 = 什么都不选，直接退出。返回 true 表示这次点击被吃掉了。 */
bool vidow_overview_click(struct vidow_server *server, double sx, double sy) {
	/* 会话锁（S3）：锁定期间概览上的点击一律不认（同上）。 */
	if (server->locked) {
		return false;
	}
	struct vidow_overview *ov = &server->overview;
	if (!vidow_overview_active(server)) {
		return false;
	}

	struct wlr_surface *surface = NULL;
	double lx = 0, ly = 0;
	struct vidow_view *hit = vidow_view_at(server, sx, sy, &surface, &lx, &ly);
	if (hit != NULL && hit->output == ov->output) {
		if (hit->popup) {
			/* 弹出物在 overview 里是收着的，不该被点到 */
			return true;
		}
		/* 迷你 / 图标态的窗口：先摊成普通窗口再给焦点（和 Alt+Tab 提交、
		 * IPC focus-window 同一个入口、同一个语义，见 server.h）。
		 * 这里以前用的是那个「什么都不做」的 vidow_focus_view()：它开头就把
		 * MINI / ICON 挡回去了（不变量：focused 永远不指小窗），于是全景里
		 * 小窗那块格子点上去只有日志、焦点一点没动 —— 用户桌上那堆小窗就是
		 * 「点谁都不动」的真凶。 */
		vidow_focus_view_expand_mini(hit);
		/* 点下去就退出，但客户端从没收到这次按下；对应的松手也吃掉，
		 * 免得它收到一个没有按下配对的 release。 */
		ov->swallow_release = true;
		wlr_log(WLR_INFO, "overview: 选中 %s",
			vidow_view_title(hit) != NULL ? vidow_view_title(hit) : "(无题)");
	}
	vidow_overview_exit(server);
	return true;
}

/* 滚轮：滚的是**工作区列表**（上下预览），不是窗口、更不是相机 —— **一次一格，
 * 一格 = 一行 = 一块工作区**（行距 `row_h + OV_ROW_GAP`）。列表排得下的时候夹在 0
 * 上，滚不动。
 *
 * 以前这里 /= 平移相机（ov->pan），列带比屏宽，一直滚就把整条带滚出屏外（用户报的
 * 「滚轮一直滚会左右滚动窗口，会跨屏」）。行内横向预览现在跟**那块工作区自己的
 * 视口**走 —— 也就是「移动窗口焦点」那些键，滚轮不再碰它。
 *
 * 步进而不是像素（用户：「overview滚轮操作应该是巡航一次滚到一个工作区」）：
 * 以前每次写死 60 像素 —— 行距 337.7 的时候要滚 6 下才走一行，而且滚到哪儿停下
 * 全看滚了几格，永远对不齐行。现在把当前位置**先吸附到行距的整数倍**再走 `steps`
 * 步，落点也永远在整数倍上（`row_h` 随屏幕变，所以整倍数是这块屏、这个 scale 下
 * 的行距 —— 见 rows_place 的排版）。到顶 / 到底就停（夹在 [0, max_scroll]）。 */
void vidow_overview_scroll(struct vidow_server *server, int steps) {
	/* 会话锁（S3）：锁定期间不开概览 / 不响应概览上的点击和滚轮 —— 概览里
	 * 全是窗口预览，锁上以后它既不该出现也不该被操作（方案 §2 环节 3）。 */
	if (server->locked) {
		return;
	}
	struct vidow_overview *ov = &server->overview;
	if (!vidow_overview_active(server)) {
		return;
	}
	double pitch = ov->row_h + (double)OV_ROW_GAP;
	if (steps == 0 || pitch <= 0.0) {
		return;
	}
	double max_scroll = rows_total(ov) - (double)ov->out_h;
	if (max_scroll < 0.0) {
		max_scroll = 0.0;
	}
	int idx = (int)lround(ov->list_scroll / pitch) + steps;
	if (idx < 0) {
		idx = 0;
	}
	double want = (double)idx * pitch;
	if (want > max_scroll) {
		want = max_scroll;
	}
	ov->list_scroll = want;
	overview_apply(server);
}

void vidow_overview_update(struct vidow_server *server) {
	if (!vidow_overview_active(server)) {
		return;
	}
	overview_apply(server);
}

bool vidow_overview_init(struct vidow_server *server) {
	/* 层本身由 main.c 按叠放顺序创建（紧贴壁纸之上、窗口之下），这里只放遮罩。
	 * 别再 wlr_scene_tree_create 一次：那会新建一层挂到最后，遮罩就跑窗口上面去了
	 * （踩过一次：overview 里连窗口都被一起压暗）。每行的背景板也挂在这棵树上
	 * （plate_apply 里按需创建），所以它们一样垫在窗口下面。 */
	server->overview.dim = wlr_scene_rect_create(server->layer_overview_dim, 1, 1,
		(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
	if (server->overview.dim == NULL) {
		return false;
	}
	wlr_scene_node_set_enabled(&server->overview.dim->node, false);
	return true;
}
