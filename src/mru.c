/* mru.c — 「最近用过的窗口」台账 + Alt+Tab 切换器（niri 的 recent windows）。
 *
 * ============================ 台账（上半） ============================
 *
 * 台账**不维护链表**：每个窗口记一个「上次真正用过」的时间戳
 * （vidow_view.mru_ts），要列的时候现按它排一遍。这样就不必为「窗口开 / 关 /
 * 换工作区 / 被销毁」维护第二条平行链表 —— 那种链表最容易留悬空指针，时间戳
 * 不会。
 *
 * debounce（mru_debounce_ms，默认 750ms）：
 * 拿到焦点不等于「用过」。键盘一路导航（Mod+H/L 连按）和 focus-follows-mouse
 * 中途扫过的窗口都只是**路过** —— 不 debounce 的话 Alt+Tab 回的就是半路上那个，
 * 而不是你真正待过的那个。所以焦点一到先记成 pending，过了 debounce 才落到
 * 时间戳上；往那个窗口打字算明确使用，debounce 立刻结束。刚开的窗口（时间戳还
 * 是 0）不 debounce：新窗口本来就该在最近列表里。
 *
 * ========================== 切换器（下半） ==========================
 *
 * 交互按 niri 的 recent windows：
 *   按下 Alt+Tab        进选择阶段：焦点**先不动**，选中「最近用过」的下一个；
 *   连按 Tab（会重复）  在清单上前后走（到头回绕）；
 *   松开修饰键          把焦点移到选中的那个，关掉；
 *   Esc                 取消（焦点原地不动），Enter 立刻确认。
 *
 * mru_open_delay_ms（默认 150ms）是「面板什么时候才真的显形」：
 * 快速点一下 Alt+Tab 只是想切到上一个窗口，不该整屏闪一下 —— 所以在延迟到点
 * 之前，画面一点都不动（on 但还没 shown）。到点了才把窗口摆进预览条。
 *
 * 预览是**真实窗口按缩放摆过去**（跟 overview 同一套：改场景节点的位置和内容
 * 缩放），不是离屏抓图 —— 见 view.c 的 view_anim_place。所以预览永远是活的
 * （带装饰、圆角、阴影、模糊），也不多一趟渲染。不在清单里的窗口在切换器开着
 * 期间被停到屏外（见 vidow_view_should_park）。
 *
 * 排版两种（mru_layout）：
 *   strip —— 一行，装不下就横向滚（保证选中的那个可见）；
 *   grid  —— 不滚：按 mru_per_row 换行，行数多了整体缩小预览，直到铺进工作区
 *            （缩到 mru_min_scale 为止）。 */

#include "server.h"
#include "ipc.h"
#include "workspace.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* 一次最多列多少个窗口。再多既没人看，也白占显存（每个预览都是一份离屏重烘）。 */
#define MRU_MAX 64
/* 预览再小也不能小于这个逻辑像素（niri 的 PREVIEW_MIN_SIZE 同此意）。 */
#define MRU_MIN_PX 16.0

/* ------------------------------------------------------------- 台账 */

static const char *mru_name(const struct vidow_view *view) {
	const char *title = vidow_view_title(view);
	return title != NULL && title[0] != '\0' ? title : "(无题)";
}

int64_t vidow_mru_now_ns(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

/* 落账：这个窗口从此刻起算「最近用过」。 */
static void mru_commit_pending(struct vidow_server *server,
		struct vidow_view *view) {
	struct vidow_mru *mru = &server->mru;
	if (view != NULL) {
		view->mru_ts = vidow_mru_now_ns();
	}
	if (mru->pending == view) {
		mru->pending = NULL;
	}
	if (mru->timer != NULL) {
		wl_event_source_timer_update(mru->timer, 0); /* 撤销这次到点 */
	}
}

static int mru_debounce_fire(void *data) {
	struct vidow_server *server = data;
	struct vidow_view *view = server->mru.pending;
	if (view != NULL) {
		wlr_log(WLR_DEBUG, "mru: %s 过了 debounce（%dms），落账",
			mru_name(view), server->config.mru_debounce_ms);
		mru_commit_pending(server, view);
	}
	return 0; /* 一次性 */
}

void vidow_mru_note_focus(struct vidow_server *server, struct vidow_view *view) {
	if (view == NULL || view->popup) {
		return;
	}
	struct vidow_mru *mru = &server->mru;
	if (!server->config.mru_enabled) {
		return;
	}
	if (mru->pending != NULL && mru->pending != view) {
		wlr_log(WLR_DEBUG, "mru: %s 只是路过（没到 debounce 就被换掉）",
			mru_name(mru->pending));
		mru->pending = NULL;
	}
	/* 台账里还没有它（刚开的窗口）：立刻落账，不 debounce。 */
	if (view->mru_ts == 0 || server->config.mru_debounce_ms <= 0 ||
			mru->timer == NULL) {
		mru_commit_pending(server, view);
		return;
	}
	mru->pending = view;
	wl_event_source_timer_update(mru->timer, server->config.mru_debounce_ms);
}

void vidow_mru_note_key(struct vidow_server *server) {
	struct vidow_mru *mru = &server->mru;
	if (mru->pending == NULL) {
		return;
	}
	wlr_log(WLR_DEBUG, "mru: %s 收到按键，debounce 提前结束",
		mru_name(mru->pending));
	mru_commit_pending(server, mru->pending);
}

void vidow_mru_forget(struct vidow_server *server, struct vidow_view *view) {
	struct vidow_mru *mru = &server->mru;
	if (mru->pending == view) {
		mru->pending = NULL;
	}
	if (mru->selected == view) {
		/* 选中的窗口自己关了：退回清单里还能用的那个，别留悬空。 */
		mru->selected = NULL;
	}
	if (mru->original == view) {
		mru->original = NULL;
	}
	for (int i = 0; i < mru->slot_n; i++) {
		if (mru->slot[i].view == view) {
			mru->slot[i].view = NULL;
		}
	}
}

/* 「能出现在切换器里」的窗口：上屏了、不是弹出物、没被最小化。 */
static bool mru_listable(const struct vidow_view *view) {
	return view->mapped && !view->popup && !view->minimized;
}

/* 这次要列的范围（mru_scope，绑定能逐次覆盖）：
 *   all        所有屏、所有工作区（默认：Alt+Tab 就该能切到任何窗口）
 *   output     只看切换器那块屏
 *   workspace  只看那块屏上现在显示着的那个工作区 */
static bool mru_in_scope(struct vidow_server *server,
		const struct vidow_view *view) {
	switch (server->mru.scope) {
	case VIDOW_MRU_OUTPUT:
		return view->output == server->mru.output;
	case VIDOW_MRU_WORKSPACE: {
		struct vidow_workspace *active =
			vidow_workspace_active_for(server, server->mru.output);
		return active != NULL && view->ws == active;
	}
	case VIDOW_MRU_ALL:
	default:
		return true;
	}
}

static int mru_cmp(const void *a, const void *b) {
	const struct vidow_view *va = *(const struct vidow_view *const *)a;
	const struct vidow_view *vb = *(const struct vidow_view *const *)b;
	if (va->mru_ts == vb->mru_ts) {
		return 0;
	}
	return va->mru_ts > vb->mru_ts ? -1 : 1; /* 新的在前 */
}

size_t vidow_mru_collect(struct vidow_server *server, const char *app_id_filter,
		struct vidow_view **out, size_t max) {
	size_t n = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (!mru_listable(view)) {
			continue;
		}
		if (app_id_filter != NULL && app_id_filter[0] != '\0') {
			const char *app_id = vidow_view_app_id(view);
			if (app_id == NULL || strcmp(app_id, app_id_filter) != 0) {
				continue;
			}
		}
		if (n < max) {
			out[n++] = view;
		}
	}
	if (n > 1) {
		qsort(out, n, sizeof(out[0]), mru_cmp);
	}
	return n;
}

/* ------------------------------------------------------- 切换器的清单 */

static bool mru_showing(const struct vidow_server *server) {
	return server->mru.shown;
}

bool vidow_mru_active(const struct vidow_server *server) {
	return server->mru.on;
}

bool vidow_mru_showing(const struct vidow_server *server) {
	return server->mru.shown;
}

/* 这个窗口现在是不是切换器里的一个预览（要在场、按预览的位置摆）。 */
bool vidow_mru_holds(const struct vidow_server *server,
		const struct vidow_view *view) {
	if (!mru_showing(server)) {
		return false;
	}
	for (int i = 0; i < server->mru.slot_n; i++) {
		if (server->mru.slot[i].view == view) {
			return true;
		}
	}
	return false;
}

/* 预览的缩放：先按「不超过最大缩放」和「不超过最大高度」取小的那个，
 * 再兜一个下限（别缩成看不见的几个像素）。 */
static double mru_slot_zoom(const struct vidow_config *cfg, int base_w,
		int base_h) {
	double zoom = cfg->mru_max_scale;
	if (base_h > 0 && cfg->mru_max_height > 0) {
		double by_height = (double)cfg->mru_max_height / (double)base_h;
		if (by_height < zoom) {
			zoom = by_height;
		}
	}
	if (zoom <= 0.0) {
		zoom = 1.0;
	}
	/* 太小了就往回抬一点（前提是不超过最大缩放）。 */
	int small = base_w < base_h ? base_w : base_h;
	if (small > 0) {
		double floor_zoom = MRU_MIN_PX / (double)small;
		if (zoom < floor_zoom) {
			zoom = floor_zoom > cfg->mru_max_scale ? cfg->mru_max_scale :
				floor_zoom;
		}
	}
	if (zoom < cfg->mru_min_scale) {
		zoom = cfg->mru_min_scale;
	}
	return zoom;
}

/* 重算整条清单的排版。窗口列表、配置、工作区、选中项变了都要重算一次。 */
static void mru_layout(struct vidow_server *server) {
	struct vidow_mru *mru = &server->mru;
	struct vidow_config *cfg = &server->config;
	mru->slot_n = 0;
	mru->total_w = 0;
	mru->total_h = 0;
	if (!mru->shown || mru->output == NULL) {
		return;
	}
	struct wlr_box work = vidow_work_area(server, mru->output);
	if (work.width < 1 || work.height < 1) {
		return;
	}
	int gap = cfg->mru_gap < 0 ? 0 : cfg->mru_gap;
	int row_gap = cfg->mru_row_gap < 0 ? 0 : cfg->mru_row_gap;
	int per_row = cfg->mru_per_row < 1 ? 1 : cfg->mru_per_row;

	/* 先把每个预览的尺寸算出来（先按最大缩放，grid 装不下时再统一缩）。 */
	int n = mru->n;
	double shrink = 1.0;
	if (cfg->mru_layout == VIDOW_MRU_GRID) {
		/* 装不下就整体缩小：横着按最宽那一行、竖着按总高度，取更狠的那个比例。
		 * 两趟就够（第一趟估、第二趟拿缩小后的尺寸再确认一次），不搞迭代。 */
		/* 迭代收敛：行高取整、行间距不缩，一趟算不准，多来几趟（窗口数
		 * 有上限，代价可以忽略）。 */
		for (int pass = 0; pass < 8; pass++) {
			double worst_w = 0, sum_h = 0;
			int row_w = 0, row_n = 0, rows = 0, row_h = 0;
			for (int i = 0; i < n; i++) {
				struct vidow_view *view = mru->order[i];
				if (view == NULL) {
					continue;
				}
				int bw = view->base_dst_w > 0 ? view->base_dst_w :
					view->geom.width;
				int bh = view->base_dst_h > 0 ? view->base_dst_h :
					view->geom.height;
				double zoom = mru_slot_zoom(cfg, bw, bh) * shrink;
				if (zoom < cfg->mru_min_scale) {
					zoom = cfg->mru_min_scale;
				}
				int w = (int)lround(bw * zoom);
				int h = (int)lround(bh * zoom);
				row_w += w + (row_n > 0 ? gap : 0);
				row_n++;
				if (h > row_h) {
					row_h = h;
				}
				if (row_n == per_row || i == n - 1) {
					if (row_w > worst_w) {
						worst_w = row_w;
					}
					sum_h += row_h + (rows > 0 ? row_gap : 0);
					rows++;
					row_w = 0;
					row_n = 0;
					row_h = 0;
				}
			}
			if (worst_w <= 0 || sum_h <= 0) {
				break;
			}
			/* 留 2px 余量：预览尺寸是取整的，刚好卡满会溢出一两个像素。 */
			double fit = 1.0;
			if (worst_w > work.width - 2) {
				fit = (double)(work.width - 2) / worst_w;
			}
			if (sum_h > work.height - 2 && sum_h > 0) {
				double by_h = (double)(work.height - 2) / sum_h;
				if (by_h < fit) {
					fit = by_h;
				}
			}
			if (fit >= 1.0) {
				break;
			}
			shrink *= fit;
			if (shrink < cfg->mru_min_scale) {
				shrink = cfg->mru_min_scale;
				break;
			}
		}
	}

	/* 摆位：strip 一行居中（装不下就把条子滚起来），grid 一行 per_row 个、
	 * 每行居中、整体竖向居中。 */
	int n_used = 0;
	for (int i = 0; i < n; i++) {
		struct vidow_view *view = mru->order[i];
		if (view == NULL) {
			continue;
		}
		int bw = view->base_dst_w > 0 ? view->base_dst_w : view->geom.width;
		int bh = view->base_dst_h > 0 ? view->base_dst_h : view->geom.height;
		if (bw < 1 || bh < 1) {
			continue;
		}
		double zoom = mru_slot_zoom(cfg, bw, bh) * shrink;
		if (zoom < cfg->mru_min_scale) {
			zoom = cfg->mru_min_scale;
		}
		struct vidow_mru_slot *slot = &mru->slot[n_used++];
		slot->view = view;
		slot->w = (int)lround(bw * zoom);
		slot->h = (int)lround(bh * zoom);
		slot->zoom = zoom;
	}

	if (cfg->mru_layout == VIDOW_MRU_GRID) {
		/* 分行、算总高，然后把每行居中摆下去。 */
		int rows = (n_used + per_row - 1) / per_row;
		int total_h = 0;
		for (int r = 0; r < rows; r++) {
			int row_h = 0;
			for (int i = r * per_row; i < (r + 1) * per_row && i < n_used;
					i++) {
				if (mru->slot[i].h > row_h) {
					row_h = mru->slot[i].h;
				}
			}
			total_h += row_h + (r > 0 ? row_gap : 0);
		}
		mru->total_w = work.width;
		mru->total_h = total_h;
		int y = work.y + (work.height - total_h) / 2;
		for (int r = 0; r < rows; r++) {
			int row_w = 0, row_h = 0, cnt = 0;
			for (int i = r * per_row; i < (r + 1) * per_row && i < n_used;
					i++) {
				row_w += mru->slot[i].w + (cnt > 0 ? gap : 0);
				if (mru->slot[i].h > row_h) {
					row_h = mru->slot[i].h;
				}
				cnt++;
			}
			int x = work.x + (work.width - row_w) / 2;
			for (int i = r * per_row; i < (r + 1) * per_row && i < n_used;
					i++) {
				/* 行内竖向居中（矮的预览跟高的对齐中线）。 */
				mru->slot[i].x = x;
				mru->slot[i].y = y + (row_h - mru->slot[i].h) / 2;
				x += mru->slot[i].w + gap;
			}
			y += row_h + row_gap;
		}
		mru->scroll = 0.0;
	} else {
		int total_w = 0, max_h = 0;
		for (int i = 0; i < n_used; i++) {
			total_w += mru->slot[i].w + (i > 0 ? gap : 0);
			if (mru->slot[i].h > max_h) {
				max_h = mru->slot[i].h;
			}
		}
		mru->total_w = total_w;
		mru->total_h = max_h;
		int base_x = work.x + (work.width - total_w) / 2;
		if (total_w > work.width) {
			base_x = work.x;
		}
		int y = work.y + (work.height - max_h) / 2;
		int x = base_x;
		for (int i = 0; i < n_used; i++) {
			mru->slot[i].x = x;
			mru->slot[i].y = y + (max_h - mru->slot[i].h) / 2;
			x += mru->slot[i].w + gap;
		}
		/* 滚到「选中的那个整个可见」为止（条子比工作区宽才有得滚）。 */
		double max_scroll = total_w > work.width ?
			(double)(total_w - work.width) : 0.0;
		double scroll = mru->scroll;
		if (scroll > max_scroll) {
			scroll = max_scroll;
		}
		if (scroll < 0.0) {
			scroll = 0.0;
		}
		struct vidow_mru_slot *sel = NULL;
		for (int i = 0; i < n_used; i++) {
			if (mru->slot[i].view == mru->selected) {
				sel = &mru->slot[i];
				break;
			}
		}
		if (sel != NULL && max_scroll > 0.0) {
			double left = sel->x - base_x;
			double right = left + sel->w;
			if (left < scroll) {
				scroll = left;
			} else if (right > scroll + work.width) {
				scroll = right - work.width;
			}
			if (scroll > max_scroll) {
				scroll = max_scroll;
			}
			if (scroll < 0.0) {
				scroll = 0.0;
			}
		}
		mru->scroll = scroll;
		int dx = -(int)lround(scroll);
		for (int i = 0; i < n_used; i++) {
			mru->slot[i].x += dx;
		}
	}
	mru->slot_n = n_used;
	/* 滚出去 / 挤出去的那些预览不画：场景是全局坐标，摆在工作区外面的预览
	 * 会落到隔壁那块屏上。置空 = 这一帧不归切换器管（vidow_mru_place 找不到它
	 * → 按停车处理），选中的那个由上面的滚动保证在画面里。 */
	for (int i = 0; i < n_used; i++) {
		struct vidow_mru_slot *slot = &mru->slot[i];
		struct wlr_box at = { slot->x, slot->y, slot->w, slot->h };
		if (!wlr_box_intersection(&(struct wlr_box){0}, &at, &work)) {
			slot->view = NULL;
		}
	}
}

/* 选中项背后那块板 + 边框，还有压暗的背景。 */
static void mru_scene_apply(struct vidow_server *server) {
	struct vidow_mru *mru = &server->mru;
	struct vidow_config *cfg = &server->config;
	if (mru->backdrop == NULL) {
		return;
	}
	bool on = mru_showing(server) && mru->output != NULL;
	if (!on) {
		wlr_scene_node_set_enabled(&mru->backdrop->node, false);
		if (mru->plate != NULL) {
			wlr_scene_node_set_enabled(&mru->plate->node, false);
		}
		for (int i = 0; i < 4; i++) {
			if (mru->border[i] != NULL) {
				wlr_scene_node_set_enabled(&mru->border[i]->node, false);
			}
		}
		return;
	}
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, mru->output, &box);
	if (box.width < 1) {
		struct wlr_box work = vidow_work_area(server, mru->output);
		box = work;
	}
	float alpha = (float)cfg->mru_backdrop_alpha;
	if (alpha < 0.0f) {
		alpha = 0.0f;
	}
	if (alpha > 1.0f) {
		alpha = 1.0f;
	}
	wlr_scene_rect_set_color(mru->backdrop,
		(float[4]){ 0.02f, 0.02f, 0.03f, alpha });
	wlr_scene_rect_set_size(mru->backdrop, box.width, box.height);
	wlr_scene_node_set_position(&mru->backdrop->node, box.x, box.y);
	wlr_scene_node_set_enabled(&mru->backdrop->node, true);

	struct vidow_mru_slot *sel = NULL;
	for (int i = 0; i < mru->slot_n; i++) {
		if (mru->slot[i].view == mru->selected) {
			sel = &mru->slot[i];
			break;
		}
	}
	if (sel == NULL || mru->plate == NULL) {
		if (mru->plate != NULL) {
			wlr_scene_node_set_enabled(&mru->plate->node, false);
		}
		for (int i = 0; i < 4; i++) {
			if (mru->border[i] != NULL) {
				wlr_scene_node_set_enabled(&mru->border[i]->node, false);
			}
		}
		return;
	}
	int pad = cfg->mru_highlight_padding < 0 ? 0 : cfg->mru_highlight_padding;
	int bw = cfg->mru_highlight_border < 0 ? 0 : cfg->mru_highlight_border;
	const float *color = cfg->mru_highlight_color;
	wlr_scene_rect_set_color(mru->plate,
		(float[4]){ color[0], color[1], color[2], color[3] * 0.35f });
	wlr_scene_rect_set_size(mru->plate, sel->w + pad * 2, sel->h + pad * 2);
	wlr_scene_node_set_position(&mru->plate->node, sel->x - pad, sel->y - pad);
	wlr_scene_node_set_enabled(&mru->plate->node, true);
	/* 边框：四条细矩形（wlr_scene_rect 是实心的，画不出描边）。 */
	int bx = sel->x - pad, by = sel->y - pad;
	int bwi = sel->w + pad * 2, bhi = sel->h + pad * 2;
	struct { int x, y, w, h; } bars[4] = {
		{ bx, by, bwi, bw },                    /* 上 */
		{ bx, by + bhi - bw, bwi, bw },         /* 下 */
		{ bx, by, bw, bhi },                    /* 左 */
		{ bx + bwi - bw, by, bw, bhi },         /* 右 */
	};
	for (int i = 0; i < 4; i++) {
		if (mru->border[i] == NULL) {
			continue;
		}
		wlr_scene_rect_set_color(mru->border[i],
			(float[4]){ color[0], color[1], color[2], color[3] });
		wlr_scene_rect_set_size(mru->border[i], bars[i].w > 0 ? bars[i].w : 1,
			bars[i].h > 0 ? bars[i].h : 1);
		wlr_scene_node_set_position(&mru->border[i]->node, bars[i].x,
			bars[i].y);
		wlr_scene_node_set_enabled(&mru->border[i]->node, bw > 0);
	}
}

/* 清单 / 选中项变了：重排 + 重画 + 把所有窗口按新位置摆一遍。 */
static void mru_refresh(struct vidow_server *server) {
	struct vidow_mru *mru = &server->mru;
	if (mru->n > 0) {
		mru_layout(server);
	}
	mru_scene_apply(server);
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		vidow_view_apply_anim(view);
	}
	vidow_backdrop_damage(server, false);
}

/* 清单里选中项的下标（找不到返回 -1）。 */
static int mru_selected_index(const struct vidow_server *server) {
	const struct vidow_mru *mru = &server->mru;
	for (int i = 0; i < mru->slot_n; i++) {
		if (mru->slot[i].view == mru->selected) {
			return i;
		}
	}
	/* 还没排版（面板没显形）时按清单本身找。 */
	for (int i = 0; i < mru->n; i++) {
		if (mru->order[i] == mru->selected) {
			return i;
		}
	}
	return -1;
}

static void mru_log_state(struct vidow_server *server, const char *what) {
	struct vidow_mru *mru = &server->mru;
	char sel[160];
	snprintf(sel, sizeof(sel), "%s",
		mru->selected != NULL ? mru_name(mru->selected) : "(无)");
	wlr_log(WLR_INFO, "mru: %s（%d 个窗口，选中 #%d %s，面板 %s）",
		what, mru->n, mru_selected_index(server), sel,
		mru->shown ? "已显示" : "还没显示");
}

/* --------------------------------------------------------- 开关 / 选择 */

static int mru_open_timer_fire(void *data) {
	struct vidow_server *server = data;
	struct vidow_mru *mru = &server->mru;
	if (!mru->on || mru->shown) {
		return 0;
	}
	mru->shown = true;
	wlr_log(WLR_INFO, "mru: 面板显形（按住 %dms 之后）",
		server->config.mru_open_delay_ms);
	mru_refresh(server);
	mru_log_state(server, "显示");
	vidow_ipc_emit_note(server, "mru-shown");
	return 0;
}

/* 开清单：把「谁在清单里、按什么顺序」定下来。选中项从当前焦点窗口的**下一个**
 * 开始（Alt+Tab 的语义：切到上一个窗口），往回走就从最后一个开始。 */
static void mru_build_order(struct vidow_server *server) {
	struct vidow_mru *mru = &server->mru;
	struct vidow_view *list[MRU_MAX];
	size_t n = vidow_mru_collect(server, mru->filter_app_id, list, MRU_MAX);
	mru->n = 0;
	for (size_t i = 0; i < n; i++) {
		if (!mru_in_scope(server, list[i])) {
			continue;
		}
		mru->order[mru->n++] = list[i];
	}
	if (n >= MRU_MAX) {
		wlr_log(WLR_INFO, "mru: 窗口多于 %d 个，只列最近这些", MRU_MAX);
	}
}

void vidow_mru_open(struct vidow_server *server, int dir, const char *app_id,
		enum vidow_mru_scope scope) {
	struct vidow_mru *mru = &server->mru;
	if (!server->config.mru_enabled) {
		return;
	}
	if (server->focused_output == NULL) {
		return;
	}
	if (mru->on) {
		/* 已经开着：这一下就是「往前/往后走一格」。按住 Alt 连点 Tab 走的就是
		 * 这条路（键会重复，每次重复都是一次按下）。 */
		vidow_mru_step(server, dir);
		return;
	}
	mru->scope = scope;
	/* overview 和切换器别同时开着：两个都在摆窗口，画面会打架。 */
	if (vidow_overview_active(server)) {
		vidow_overview_exit(server);
	}
	mru->on = true;
	mru->shown = false;
	mru->output = server->focused_output;
	mru->original = server->focused;
	mru->scroll = 0.0;
	snprintf(mru->filter_app_id, sizeof(mru->filter_app_id), "%s",
		app_id != NULL ? app_id : "");
	mru_build_order(server);
	if (mru->n == 0) {
		wlr_log(WLR_INFO, "mru: 没有可切的窗口");
		mru->on = false;
		return;
	}
	/* 选中项：清单里第一个**不是当前焦点**的窗口。 */
	int start = 0;
	for (int i = 0; i < mru->n; i++) {
		if (mru->order[i] != server->focused) {
			start = i;
			break;
		}
	}
	if (dir < 0) {
		start = mru->n - 1;
	}
	mru->selected = mru->order[start];
	if (mru->open_timer != NULL && server->config.mru_open_delay_ms > 0) {
		wl_event_source_timer_update(mru->open_timer,
			server->config.mru_open_delay_ms);
	} else {
		mru_open_timer_fire(server);
	}
	wlr_log(WLR_INFO, "mru: 打开（%d 个窗口，%s，延迟 %dms）", mru->n,
		server->config.mru_layout == VIDOW_MRU_GRID ? "网格" : "一行",
		server->config.mru_open_delay_ms);
	mru_log_state(server, "打开");
	vidow_ipc_emit_note(server, "mru-opened");
}

void vidow_mru_step(struct vidow_server *server, int dir) {
	struct vidow_mru *mru = &server->mru;
	if (!mru->on || mru->n <= 0) {
		return;
	}
	int idx = -1;
	for (int i = 0; i < mru->n; i++) {
		if (mru->order[i] == mru->selected) {
			idx = i;
			break;
		}
	}
	if (idx < 0) {
		idx = 0;
	}
	idx += dir > 0 ? 1 : -1;
	if (idx < 0) {
		idx = mru->n - 1;
	}
	if (idx >= mru->n) {
		idx = 0;
	}
	mru->selected = mru->order[idx];
	if (mru->shown) {
		mru_refresh(server);
	}
	wlr_log(WLR_DEBUG, "mru: 走到 #%d %s", idx, mru_name(mru->selected));
}

/* 关掉（画面恢复）。commit = true 时先把焦点给选中的那个。 */
static void mru_close(struct vidow_server *server, bool commit) {
	struct vidow_mru *mru = &server->mru;
	if (!mru->on && !mru->shown) {
		return;
	}
	struct vidow_view *target = commit ? mru->selected : NULL;
	mru->on = false;
	mru->shown = false;
	mru->slot_n = 0;
	mru->n = 0;
	mru->scroll = 0.0;
	mru->selected = NULL;
	mru->output = NULL;
	if (mru->open_timer != NULL) {
		wl_event_source_timer_update(mru->open_timer, 0);
	}
	mru_scene_apply(server);
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		vidow_view_apply_anim(view);
	}
	vidow_workspace_apply_visibility(server);
	vidow_backdrop_damage(server, false);
	if (target != NULL && target->mapped && !target->minimized) {
		/* 明确选中 = 明确使用：直接落账，不等 debounce。 */
		vidow_focus_view(target);
		vidow_mru_note_focus(server, target);
		mru_commit_pending(server, target);
	}
	wlr_log(WLR_INFO, "mru: %s（%s）", commit ? "提交" : "取消",
		target != NULL ? mru_name(target) : "焦点不动");
	vidow_ipc_emit_note(server, commit ? "mru-committed" : "mru-cancelled");
}

void vidow_mru_commit(struct vidow_server *server) {
	mru_close(server, true);
}

void vidow_mru_cancel(struct vidow_server *server) {
	mru_close(server, false);
}

/* --------------------------------------------------------- 给渲染用 */

enum vidow_mru_place vidow_mru_place(const struct vidow_view *view, int *x,
		int *y, double *zoom) {
	struct vidow_server *server = view->server;
	if (!mru_showing(server)) {
		return VIDOW_MRU_PLACE_NONE;
	}
	for (int i = 0; i < server->mru.slot_n; i++) {
		const struct vidow_mru_slot *slot = &server->mru.slot[i];
		if (slot->view != view) {
			continue;
		}
		*x = slot->x;
		*y = slot->y;
		*zoom = slot->zoom;
		return VIDOW_MRU_PLACE_PREVIEW;
	}
	return VIDOW_MRU_PLACE_HIDE;
}

/* 配置改了 / 屏幕变了：清单还在就重排一遍。 */
void vidow_mru_update(struct vidow_server *server) {
	struct vidow_mru *mru = &server->mru;
	if (!mru_showing(server)) {
		return;
	}
	mru_build_order(server);
	if (mru->selected == NULL && mru->n > 0) {
		mru->selected = mru->order[0];
	}
	mru_refresh(server);
}

bool vidow_mru_init(struct vidow_server *server) {
	struct vidow_mru *mru = &server->mru;
	mru->pending = NULL;
	mru->on = false;
	mru->shown = false;
	mru->n = 0;
	mru->slot_n = 0;
	mru->selected = NULL;
	mru->original = NULL;
	mru->output = NULL;
	mru->order = calloc(MRU_MAX, sizeof(mru->order[0]));
	mru->slot = calloc(MRU_MAX, sizeof(mru->slot[0]));
	if (mru->order == NULL || mru->slot == NULL) {
		return false;
	}
	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	mru->timer = wl_event_loop_add_timer(loop, mru_debounce_fire, server);
	mru->open_timer = wl_event_loop_add_timer(loop, mru_open_timer_fire, server);
	if (mru->timer == NULL || mru->open_timer == NULL) {
		return false;
	}
	/* 面板的东西跟 overview 的遮罩挤同一层（壁纸之上、窗口之下）：压暗背景
	 * 和选中项背后那块板都得在窗口**下面**才看得见。 */
	mru->backdrop = wlr_scene_rect_create(server->layer_overview_dim, 1, 1,
		(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
	mru->plate = wlr_scene_rect_create(server->layer_overview_dim, 1, 1,
		(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
	if (mru->backdrop == NULL || mru->plate == NULL) {
		return false;
	}
	for (int i = 0; i < 4; i++) {
		mru->border[i] = wlr_scene_rect_create(server->layer_overview_dim, 1, 1,
			(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
		if (mru->border[i] == NULL) {
			return false;
		}
		wlr_scene_node_set_enabled(&mru->border[i]->node, false);
	}
	wlr_scene_node_set_enabled(&mru->backdrop->node, false);
	wlr_scene_node_set_enabled(&mru->plate->node, false);
	return true;
}
