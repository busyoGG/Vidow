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

#include <drm_fourcc.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
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

/* 屏幕顺序：先按屏（左→右），再按工作区序号，最后按窗口在屏上的位置（左→右、
 * 一列里上→下）。同一个工作区里，这就是你眼睛看到的那一排。 */
static int mru_cmp_screen(const void *a, const void *b) {
	const struct vidow_view *va = *(const struct vidow_view *const *)a;
	const struct vidow_view *vb = *(const struct vidow_view *const *)b;
	struct wlr_box boa = {0}, bob = {0};
	if (va->output != NULL) {
		wlr_output_layout_get_box(va->server->output_layout, va->output, &boa);
	}
	if (vb->output != NULL) {
		wlr_output_layout_get_box(vb->server->output_layout, vb->output, &bob);
	}
	int oa = boa.x, ob = bob.x;
	if (oa != ob) {
		return oa < ob ? -1 : 1;
	}
	int wa = va->ws != NULL ? vidow_workspace_index(va->ws) : 0;
	int wb = vb->ws != NULL ? vidow_workspace_index(vb->ws) : 0;
	if (wa != wb) {
		return wa < wb ? -1 : 1;
	}
	struct wlr_box ba = vidow_view_screen_box(va);
	struct wlr_box bb = vidow_view_screen_box(vb);
	if (ba.x != bb.x) {
		return ba.x < bb.x ? -1 : 1;
	}
	if (ba.y != bb.y) {
		return ba.y < bb.y ? -1 : 1;
	}
	return 0;
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

/* 卡片的内边距：预览是方的，圆角一旦超出内边距，窗口就会把圆弧盖掉（看着就是
 * 「缺角」），所以内边距至少给到半径那么大 —— 半径 r 时圆弧正好贴着预览的角。 */
static int mru_card_pad(const struct vidow_config *cfg) {
	int pad = cfg->mru_card_padding < 0 ? 0 : cfg->mru_card_padding;
	int r = cfg->mru_card_radius < 0 ? 0 : cfg->mru_card_radius;
	return r > pad ? r : pad;
}

/* 把一张卡片摆在 (x, y)（卡片尺寸 cw×ch）：卡片记在 cx/cy/cw/ch（鼠标命中按它
 * 算），预览缩进内边距摆在卡片里。 */
static void mru_slot_place(struct vidow_mru_slot *slot, int x, int y, int cw,
		int ch, int pad) {
	slot->cx = x;
	slot->cy = y;
	slot->cw = cw;
	slot->ch = ch;
	slot->x = x + pad;
	slot->y = y + pad;
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
	/* 每一项背后那张卡片比预览大一圈（内边距 pad）。排版按**卡片**算：居中、
	 * 「装不下就整体缩」、相邻之间隔多少，量的都是卡片的边；预览摆在卡片里缩进
	 * pad。下面这一段里的 w / h 都是卡片尺寸。 */
	int pad = mru_card_pad(cfg);

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
				int w = (int)lround(bw * zoom) + pad * 2;
				int h = (int)lround(bh * zoom) + pad * 2;
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
		/* 分行、算总高，然后把每行居中摆下去（摆的都是卡片）。 */
		int rows = (n_used + per_row - 1) / per_row;
		int total_h = 0;
		for (int r = 0; r < rows; r++) {
			int row_h = 0;
			for (int i = r * per_row; i < (r + 1) * per_row && i < n_used;
					i++) {
				int ch = mru->slot[i].h + pad * 2;
				if (ch > row_h) {
					row_h = ch;
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
				int cw = mru->slot[i].w + pad * 2;
				int ch = mru->slot[i].h + pad * 2;
				row_w += cw + (cnt > 0 ? gap : 0);
				if (ch > row_h) {
					row_h = ch;
				}
				cnt++;
			}
			int x = work.x + (work.width - row_w) / 2;
			for (int i = r * per_row; i < (r + 1) * per_row && i < n_used;
					i++) {
				int cw = mru->slot[i].w + pad * 2;
				int ch = mru->slot[i].h + pad * 2;
				/* 行内竖向居中（矮的那张跟高的对齐中线）。 */
				mru_slot_place(&mru->slot[i], x, y + (row_h - ch) / 2, cw,
					ch, pad);
				x += cw + gap;
			}
			y += row_h + row_gap;
		}
		mru->scroll = 0.0;
	} else {
		int total_w = 0, max_h = 0;
		for (int i = 0; i < n_used; i++) {
			int cw = mru->slot[i].w + pad * 2;
			int ch = mru->slot[i].h + pad * 2;
			total_w += cw + (i > 0 ? gap : 0);
			if (ch > max_h) {
				max_h = ch;
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
			int cw = mru->slot[i].w + pad * 2;
			int ch = mru->slot[i].h + pad * 2;
			mru_slot_place(&mru->slot[i], x, y + (max_h - ch) / 2, cw, ch,
				pad);
			x += cw + gap;
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
			double left = sel->cx - base_x;
			double right = left + sel->cw;
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
			mru_slot_place(&mru->slot[i], mru->slot[i].cx + dx,
				mru->slot[i].cy, mru->slot[i].cw, mru->slot[i].ch,
				pad);
		}
	}
	mru->slot_n = n_used;
	/* 滚出去 / 挤出去的那些预览不画：场景是全局坐标，摆在工作区外面的预览
	 * 会落到隔壁那块屏上。置空 = 这一项不画（缩略图也不画），选中的那个由上面
	 * 的滚动保证在画面里。
	 *
	 * 判的是**整张卡片**（`cx/cy/cw/ch`，比缩略图大一圈卡片边距），不是缩略图：
	 * 卡片的底、描边和四个圆角是各自独立的场景节点（`wlr_scene_rect` / 自己的
	 * 小 buffer），裁不了 —— 只判缩略图的话，半张卡片露在工作区外的那一半就
	 * 画到隔壁显示器上了（用户报的「开大量窗口、滚动清单时，超出视口的窗口会
	 * 在另一个显示器显示」：量到 22627 个像素落在隔壁屏上）。 */
	for (int i = 0; i < n_used; i++) {
		struct vidow_mru_slot *slot = &mru->slot[i];
		struct wlr_box card = { slot->cx, slot->cy, slot->cw, slot->ch };
		if (!wlr_box_contains_box(&work, &card)) {
			slot->view = NULL;
		}
	}
}

/* ------------------------------------------------- 卡片（超椭圆圆角底 + 描边）
 *
 * 每一项背后一张卡片，样子照 portal 的选择器（tools/vidowportal.c 的 card_draw）：
 * 超椭圆（squircle）圆角、一层淡底、一圈细描边；鼠标底下那张描边换成强调色，
 * 选中的那张描边加粗、底也染一点强调色 —— portal 的 checked 就是这个配方。
 *
 * 圆角只能自己画：wlr_scene_rect 是实心矩形，wlr_scene 没有圆角 API（wlroots
 * 0.20 的 wlr_scene_buffer 也没有 corner_radius），所以四个角各挂一个小节点，里面
 * 是**按覆盖率算出来的像素**（4x4 超采样抗锯齿），中间那几条直边用普通矩形铺 ——
 * 九宫格那套：角不拉伸、边拉伸。四个角共用同一张图，靠 transform 翻过去用。
 *
 * 像素怎么进场景：场景节点只认 wlr_buffer，而 wlroots 没有「从 CPU 像素造
 * wlr_buffer」的 API（wlr_texture_from_pixels 给的是纹理，喂不进节点），所以这里
 * 自己实现一个最小 wlr_buffer（像素在堆上，begin_data_ptr_access 指过去）。GLES2
 * 和 pixman 两条渲染器都是从这条路把像素上传成纹理的，和客户端递上来的 shm 缓冲
 * 同一条 —— 不用 memfd、不用 GL 上下文、不碰渲染线程。 */

/* 超椭圆的指数（portal 的 SQUIRCLE_N 同值）：n = 2 是普通圆角，4 = 「方中带圆」。 */
#define MRU_SQUIRCLE_N 4.0
/* 圆角覆盖率的超采样倍数：4x4 = 16 级灰阶，够圆角用了。 */
#define MRU_SS 4

struct vidow_mru_buf {
	struct wlr_buffer base;
	uint32_t *px;
};

struct vidow_mru_card {
	struct vidow_fx_target fx;          /* 预览：走装饰管线画（形状和真窗口一样） */
	bool fx_on;                         /* 装饰管线这条能走（没装饰时退回下面那条） */
	struct wlr_scene_buffer *thumb;     /* 退回那条：直接摆客户端那份 buffer */
	struct wlr_scene_buffer *fill_c[4]; /* 四个圆角（底） */
	struct wlr_scene_rect *fill_e[3];   /* 中 / 左 / 右三条直边 */
	struct wlr_scene_buffer *ring_c[4]; /* 四个圆角（描边） */
	struct wlr_scene_rect *ring_e[4];   /* 上下左右四条直边 */
	struct wlr_buffer *fill_buf;        /* 圆角那张图（四个角共用） */
	struct wlr_buffer *ring_buf;
	int px;                             /* 上次画的圆角边长（设备像素） */
	int bw;                             /* 上次画的描边宽（设备像素），0 = 没描边 */
	uint32_t fill_rgba;                 /* 上次用的颜色（直通色） */
	uint32_t ring_rgba;
};

static void mru_card_hide(struct vidow_mru_card *card, bool free_buf);

static void mru_buf_destroy(struct wlr_buffer *buffer) {
	struct vidow_mru_buf *b = wl_container_of(buffer, b, base);
	free(b->px);
	free(b);
}

static bool mru_buf_begin_access(struct wlr_buffer *buffer, uint32_t flags,
		void **data, uint32_t *format, size_t *stride) {
	(void)flags;
	struct vidow_mru_buf *b = wl_container_of(buffer, b, base);
	*data = b->px;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)buffer->width * 4;
	return true;
}

static void mru_buf_end_access(struct wlr_buffer *buffer) {
	(void)buffer;
}

static const struct wlr_buffer_impl mru_buf_impl = {
	.destroy = mru_buf_destroy,
	.begin_data_ptr_access = mru_buf_begin_access,
	.end_data_ptr_access = mru_buf_end_access,
};

/* 造一张 w×h 的 CPU 像素缓冲（全透明，预乘 alpha）。用完 wlr_buffer_drop() 收。 */
static struct wlr_buffer *mru_buf_create(int w, int h) {
	if (w < 1 || h < 1) {
		return NULL;
	}
	struct vidow_mru_buf *b = calloc(1, sizeof(*b));
	if (b == NULL) {
		return NULL;
	}
	b->px = calloc((size_t)w * (size_t)h, sizeof(b->px[0]));
	if (b->px == NULL) {
		free(b);
		return NULL;
	}
	wlr_buffer_init(&b->base, &mru_buf_impl, w, h);
	return &b->base;
}

static double mru_clamp01(double v) {
	return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
}

/* 直通色 → 0xAARRGGBB（预乘在算像素的时候做）。 */
static uint32_t mru_rgba(const float c[4]) {
	uint32_t a = (uint32_t)lround(mru_clamp01(c[3]) * 255.0);
	uint32_t r = (uint32_t)lround(mru_clamp01(c[0]) * 255.0);
	uint32_t g = (uint32_t)lround(mru_clamp01(c[1]) * 255.0);
	uint32_t b = (uint32_t)lround(mru_clamp01(c[2]) * 255.0);
	return (a << 24) | (r << 16) | (g << 8) | b;
}

/* 预乘 alpha 的 over：先铺 base（覆盖率 cov_base），再把 top（覆盖率 cov_top）
 * 压上去。两个颜色都是直通值。 */
static uint32_t mru_px_over(const float base[4], double cov_base,
		const float top[4], double cov_top) {
	double ta = (double)top[3] * cov_top;
	double ba = (double)base[3] * cov_base;
	double k = 1.0 - ta;
	double a = ta + ba * k;
	double r = (double)top[0] * ta + (double)base[0] * ba * k;
	double g = (double)top[1] * ta + (double)base[1] * ba * k;
	double b = (double)top[2] * ta + (double)base[2] * ba * k;
	return ((uint32_t)lround(mru_clamp01(a) * 255.0) << 24) |
		((uint32_t)lround(mru_clamp01(r) * 255.0) << 16) |
		((uint32_t)lround(mru_clamp01(g) * 255.0) << 8) |
		(uint32_t)lround(mru_clamp01(b) * 255.0);
}

/* 超椭圆的内部判定：|dx/r|^n + |dy/r|^n <= 1（dx / dy 是离弧心的距离）。 */
static bool mru_squircle_in(double dx, double dy, double r) {
	double a = dx / r, b = dy / r;
	double a2 = a * a, b2 = b * b;
	return a2 * a2 + b2 * b2 <= 1.0;   /* n = MRU_SQUIRCLE_N = 4 */
}

/* 圆角那张图：n×n 设备像素（弧心在 (n, n)），只画**左上**那个角，别的角靠
 * transform 翻过去。bw = 描边宽（设备像素，0 = 只画底）。底色在里、描边压在上面。 */
static void mru_corner_pixels(uint32_t *out, int n, int bw, const float fill[4],
		const float ring[4]) {
	double r = (double)n;
	double ri = (double)(n - bw);
	for (int j = 0; j < n; j++) {
		for (int i = 0; i < n; i++) {
			int n_fill = 0, n_ring = 0;
			for (int sy = 0; sy < MRU_SS; sy++) {
				for (int sx = 0; sx < MRU_SS; sx++) {
					double x = (double)i +
						((double)sx + 0.5) / MRU_SS;
					double y = (double)j +
						((double)sy + 0.5) / MRU_SS;
					double dx = r - x, dy = r - y;
					if (!mru_squircle_in(dx, dy, r)) {
						continue;
					}
					n_fill++;
					if (bw > 0 && ri > 0.0 &&
							mru_squircle_in(dx, dy, ri)) {
						continue;
					}
					n_ring++;
				}
			}
			if (n_fill == 0) {
				continue;   /* 圆角外：透明（calloc 出来的就是 0） */
			}
			double cov = (double)n_fill / (MRU_SS * MRU_SS);
			double cov_ring = (double)n_ring / (MRU_SS * MRU_SS);
			out[j * n + i] = mru_px_over(fill, cov, ring, cov_ring);
		}
	}
}

/* 重画一张卡片的圆角图（尺寸或样式变了才调）。四个角共用同一张图：右上 = 水平
 * 翻、左下 = 垂直翻、右下 = 转 180°。 */
static void mru_card_rebuild(struct vidow_server *server,
		struct vidow_mru_card *card, int px, int bw, const float fill[4],
		const float ring[4]) {
	static const enum wl_output_transform tr[4] = {
		WL_OUTPUT_TRANSFORM_NORMAL,      /* 左上 */
		WL_OUTPUT_TRANSFORM_FLIPPED,     /* 右上：水平翻 */
		WL_OUTPUT_TRANSFORM_FLIPPED_180, /* 左下：垂直翻 */
		WL_OUTPUT_TRANSFORM_180,         /* 右下：转 180° */
	};
	(void)server;
	struct wlr_buffer *fb = mru_buf_create(px, px);
	if (fb != NULL) {
		struct vidow_mru_buf *b = wl_container_of(fb, b, base);
		mru_corner_pixels(b->px, px, 0, fill, fill);
	}
	struct wlr_buffer *rb = bw > 0 ? mru_buf_create(px, px) : NULL;
	if (rb != NULL) {
		struct vidow_mru_buf *b = wl_container_of(rb, b, base);
		mru_corner_pixels(b->px, px, bw, fill, ring);
	}
	for (int k = 0; k < 4; k++) {
		wlr_scene_buffer_set_buffer(card->fill_c[k], fb);
		wlr_scene_buffer_set_transform(card->fill_c[k], tr[k]);
		wlr_scene_buffer_set_buffer(card->ring_c[k], rb);
		wlr_scene_buffer_set_transform(card->ring_c[k], tr[k]);
	}
	if (card->fill_buf != NULL) {
		wlr_buffer_drop(card->fill_buf);
	}
	if (card->ring_buf != NULL) {
		wlr_buffer_drop(card->ring_buf);
	}
	card->fill_buf = fb;
	card->ring_buf = rb;
	card->px = px;
	card->bw = bw;
	card->fill_rgba = mru_rgba(fill);
	card->ring_rgba = mru_rgba(ring);
}

/* 第 i 张卡片（按需建，建了留着复用）。节点顺序 = 画图顺序：先底、再描边。 */
static struct vidow_mru_card *mru_card_get(struct vidow_server *server, int i) {
	struct vidow_mru *mru = &server->mru;
	if (i < 0 || i >= MRU_MAX) {
		return NULL;
	}
	if (mru->card == NULL) {
		mru->card = calloc(MRU_MAX, sizeof(mru->card[0]));
		if (mru->card == NULL) {
			return NULL;
		}
	}
	if (mru->card[i] != NULL) {
		return mru->card[i];
	}
	struct vidow_mru_card *card = calloc(1, sizeof(*card));
	if (card == NULL) {
		return NULL;
	}
	card->bw = -1;
	bool ok = true;
	for (int k = 0; k < 4; k++) {
		card->fill_c[k] = wlr_scene_buffer_create(mru->tree, NULL);
		ok = ok && card->fill_c[k] != NULL;
	}
	for (int k = 0; k < 3; k++) {
		card->fill_e[k] = wlr_scene_rect_create(mru->tree, 1, 1,
			(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
		ok = ok && card->fill_e[k] != NULL;
	}
	/* 缩略图夹在「底」和「描边」之间：底中间那条大矩形铺满整张卡片，缩略图得
	 * 压在它上面；描边在最外圈，压不压无所谓，但排最后总没错。 */
	card->thumb = wlr_scene_buffer_create(mru->tree, NULL);
	ok = ok && card->thumb != NULL;
	for (int k = 0; k < 4; k++) {
		card->ring_c[k] = wlr_scene_buffer_create(mru->tree, NULL);
		ok = ok && card->ring_c[k] != NULL;
	}
	for (int k = 0; k < 4; k++) {
		card->ring_e[k] = wlr_scene_rect_create(mru->tree, 1, 1,
			(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
		ok = ok && card->ring_e[k] != NULL;
	}
	if (!ok) {
		return NULL;
	}
	/* 预览走装饰管线（和屏幕上那扇窗口同一条）：圆角 / 边框 / 阴影 / 模糊都照
	 * 窗口自己的规则来，所以「预览的形状 = 窗口的形状」。装饰关着 / 没 GL 的
	 * 时候 vidow_fx_begin 返回 false，那就退回「直接摆客户端 buffer」那条。 */
	card->fx_on = vidow_fx_begin(server, &card->fx, mru->tree, NULL);
	mru_card_hide(card, false);
	mru->card[i] = card;
	if (i + 1 > mru->card_n) {
		mru->card_n = i + 1;
	}
	return card;
}

/* 摆一条矩形（尺寸非正 = 这一条不画）。 */
static void mru_rect_put(struct wlr_scene_rect *rc, int x, int y, int w, int h) {
	if (rc == NULL) {
		return;
	}
	if (w <= 0 || h <= 0) {
		wlr_scene_node_set_enabled(&rc->node, false);
		return;
	}
	wlr_scene_rect_set_size(rc, w, h);
	wlr_scene_node_set_position(&rc->node, x, y);
	wlr_scene_node_set_enabled(&rc->node, true);
}

/* 摆一个圆角节点（has_img = 这张卡片画过角图；尺寸非正或没图 = 不画）。
 *
 * 判据必须是「卡片自己有没有图」，不能看 node->buffer：wlroots 上传完纹理就把
 * buffer unlock 掉，锁归零 → 发 release → 场景把 node->buffer 清成 NULL（纹理还
 * 留着，照样能画）。要是拿 node->buffer 当判据，第二次刷新就会把角节点关掉，
 * 圆角就「缺角」了。 */
static void mru_buf_put(struct wlr_scene_buffer *node, bool has_img, int x,
		int y, int w, int h) {
	if (node == NULL) {
		return;
	}
	if (w <= 0 || h <= 0 || !has_img) {
		wlr_scene_node_set_enabled(&node->node, false);
		return;
	}
	wlr_scene_buffer_set_dest_size(node, w, h);
	wlr_scene_node_set_position(&node->node, x, y);
	wlr_scene_node_set_enabled(&node->node, true);
}

/* 把一张卡片摆到 box 上（逻辑像素）。 */
static void mru_card_place(struct vidow_server *server,
		struct vidow_mru_card *card, const struct wlr_box *box,
		const float fill[4], const float ring[4], int bw) {
	struct vidow_config *cfg = &server->config;
	struct vidow_mru *mru = &server->mru;
	double scale = 1.0;
	if (mru->output != NULL && mru->output->scale > 0.0) {
		scale = mru->output->scale;
	}
	int r = cfg->mru_card_radius;
	if (r < 0) {
		r = 0;
	}
	if (r > box->width / 2) {
		r = box->width / 2;
	}
	if (r > box->height / 2) {
		r = box->height / 2;
	}
	if (bw < 0) {
		bw = 0;
	}
	if (bw > r) {
		bw = r;
	}
	int x = box->x, y = box->y, w = box->width, h = box->height;
	int px = (int)lround((double)r * scale);
	int bwp = (int)lround((double)bw * scale);
	bool round = px >= 2 && w > 0 && h > 0;
	if (round && (card->px != px || card->bw != bwp ||
			card->fill_rgba != mru_rgba(fill) ||
			card->ring_rgba != mru_rgba(ring))) {
		mru_card_rebuild(server, card, px, bwp, fill, ring);
	}
	/* 底：圆角那四块由角节点补，中间三条直边铺满剩下的地方。场景的矩形颜色是
	 * 预乘 alpha 的（wlr_render_color 的约定），自己乘一遍。 */
	float fill_pm[4] = { fill[0] * fill[3], fill[1] * fill[3],
		fill[2] * fill[3], fill[3] };
	if (round) {
		mru_rect_put(card->fill_e[0], x + r, y, w - 2 * r, h);
		mru_rect_put(card->fill_e[1], x, y + r, r, h - 2 * r);
		mru_rect_put(card->fill_e[2], x + w - r, y + r, r, h - 2 * r);
	} else {
		mru_rect_put(card->fill_e[0], x, y, w, h);
		mru_rect_put(card->fill_e[1], 0, 0, 0, 0);
		mru_rect_put(card->fill_e[2], 0, 0, 0, 0);
	}
	for (int k = 0; k < 3; k++) {
		if (card->fill_e[k] != NULL) {
			wlr_scene_rect_set_color(card->fill_e[k], fill_pm);
		}
	}
	int cxs[4] = { x, x + w - r, x, x + w - r };
	int cys[4] = { y, y, y + h - r, y + h - r };
	for (int k = 0; k < 4; k++) {
		mru_buf_put(card->fill_c[k], card->fill_buf != NULL, cxs[k],
			cys[k], round ? r : 0, round ? r : 0);
		mru_buf_put(card->ring_c[k], card->ring_buf != NULL, cxs[k],
			cys[k], round && bwp > 0 ? r : 0,
			round && bwp > 0 ? r : 0);
	}
	/* 描边：直边四条（圆角那四段由角节点补）。 */
	float ring_pm[4] = { ring[0] * ring[3], ring[1] * ring[3],
		ring[2] * ring[3], ring[3] };
	for (int k = 0; k < 4; k++) {
		if (card->ring_e[k] != NULL) {
			wlr_scene_rect_set_color(card->ring_e[k], ring_pm);
		}
	}
	if (bwp > 0) {
		if (round) {
			mru_rect_put(card->ring_e[0], x + r, y, w - 2 * r, bw);
			mru_rect_put(card->ring_e[1], x + r, y + h - bw, w - 2 * r,
				bw);
			mru_rect_put(card->ring_e[2], x, y + r, bw, h - 2 * r);
			mru_rect_put(card->ring_e[3], x + w - bw, y + r, bw,
				h - 2 * r);
		} else {
			/* 没圆角（半径 0）：四条边围一圈，四角别重叠（重叠会叠两次色）。 */
			mru_rect_put(card->ring_e[0], x, y, w, bw);
			mru_rect_put(card->ring_e[1], x, y + h - bw, w, bw);
			mru_rect_put(card->ring_e[2], x, y + bw, bw, h - 2 * bw);
			mru_rect_put(card->ring_e[3], x + w - bw, y + bw, bw,
				h - 2 * bw);
		}
	} else {
		for (int k = 0; k < 4; k++) {
			mru_rect_put(card->ring_e[k], 0, 0, 0, 0);
		}
	}
}

/* 缩略图：把**客户端那份 buffer** 直接摆进预览框里 —— 场景会拿它的纹理画，
 * 等于「把窗口内容缩小一份」，而窗口本身一动没动（这就是切换器要的效果：面板是
 * 新内容，原来那块屏照旧）。宽高比按 buffer 自己的算：塞不下就等比缩到框里居中，
 * 不拉伸（窗口逻辑尺寸和 buffer 尺寸偶尔对不上，别赌）。 */
static void mru_card_thumb_off(struct vidow_mru_card *card) {
	if (card->fx.node != NULL) {
		wlr_scene_node_set_enabled(&card->fx.node->node, false);
	}
	if (card->thumb != NULL) {
		wlr_scene_buffer_set_buffer(card->thumb, NULL);
		wlr_scene_node_set_enabled(&card->thumb->node, false);
	}
}

static void mru_card_thumb(struct vidow_server *server,
		struct vidow_mru_card *card, const struct vidow_view *view,
		const struct wlr_box *box, double zoom) {
	if (card->thumb == NULL || box->width <= 0 || box->height <= 0) {
		return;
	}
	struct wlr_scene_buffer *src = view->scene_surface != NULL ?
		view->scene_surface->buffer : NULL;
	struct wlr_buffer *buf = src != NULL ? src->buffer : NULL;
	if (buf == NULL || buf->width <= 0 || buf->height <= 0) {
		/* 还没上屏 / 客户端刚把 buffer 收走：留着卡片底，这一帧不画预览。 */
		mru_card_thumb_off(card);
		return;
	}

	/* 首选：装饰管线把窗口重画一遍（缩到预览那么大）。离屏那张图里已经有圆角、
	 * 边框、阴影 —— 预览的形状和屏幕上那扇窗口一模一样，而且这些参数来自窗口
	 * 自己的规则（不是这儿另算一份）。 */
	if (card->fx_on && card->fx.node != NULL && zoom > 0.0) {
		struct vidow_fx_geo geo = vidow_view_fx_geo(view);
		if (geo.w > 0 && geo.h > 0) {
			int cw = (int)lround((double)geo.w * zoom);
			int ch = (int)lround((double)geo.h * zoom);
			int px = box->x + (box->width - cw) / 2;
			int py = box->y + (box->height - ch) / 2;
			double out_scale = 1.0;
			if (server->mru.output != NULL && server->mru.output->scale > 0.0) {
				out_scale = server->mru.output->scale;
			}
			card->fx.anim_scale = 1.0;
			/* 「客户端声明的有效源区域」抄窗口那一份（没裁过的那版）：照场景节点
			 * 自己的 src_box 会把「裁到屏里剩下的那一小条」当成整块内容。 */
			card->fx.content_box = view->fx.content_box;
			wlr_scene_node_set_enabled(&card->fx.node->node, true);
			if (vidow_fx_frame(server, &card->fx, src, &geo, px, py,
					px, py, 1.0, server->focused == view,
					view->fullscreen, out_scale * zoom, zoom,
					&view->rule_eff)) {
				if (card->thumb->buffer != NULL) {
					wlr_scene_buffer_set_buffer(card->thumb, NULL);
				}
				wlr_scene_node_set_enabled(&card->thumb->node, false);
				return;
			}
			wlr_scene_node_set_enabled(&card->fx.node->node, false);
		}
	}
	/* 只取「可见窗口」那一块：自己画装饰的客户端（GTK）会把表面做大一圈画阴影，
	 * 那圈留白不要 —— 取法跟装饰着色器一致（归一化坐标 × buffer 尺寸）。全 0 =
	 * 没算（X11 整块就是内容），那就整块铺。 */
	struct vidow_fx_geo geo = vidow_view_fx_geo(view);
	struct wlr_fbox crop = {
		.x = geo.tex_u0 * (double)buf->width,
		.y = geo.tex_v0 * (double)buf->height,
		.width = geo.tex_du * (double)buf->width,
		.height = geo.tex_dv * (double)buf->height,
	};
	bool cropped = crop.width >= 1.0 && crop.height >= 1.0;
	double ar = cropped ? crop.width / crop.height :
		(double)buf->width / (double)buf->height;
	int tw = box->width, th = (int)lround((double)box->width / ar);
	if (th > box->height) {
		th = box->height;
		tw = (int)lround((double)box->height * ar);
	}
	if (tw < 1) {
		tw = 1;
	}
	if (th < 1) {
		th = 1;
	}
	/* buffer 一样就别再 set 一次（set 会把缓存的纹理丢掉重新来）。 */
	if (card->thumb->buffer != buf) {
		wlr_scene_buffer_set_buffer(card->thumb, buf);
	}
	/* 注意：别抄场景节点自己的 src_box —— 那里面还含着「窗口被裁到屏里」那块
	 * （列排到屏外的窗口只剩一条），缩略图要的是整扇窗口。 */
	wlr_scene_buffer_set_source_box(card->thumb, cropped ? &crop : NULL);
	wlr_scene_buffer_set_dest_size(card->thumb, tw, th);
	wlr_scene_node_set_position(&card->thumb->node, box->x + (box->width - tw) / 2,
		box->y + (box->height - th) / 2);
	wlr_scene_node_set_enabled(&card->thumb->node, true);
}

/* 收起一张卡片。free_buf = true 连圆角那张图一起放掉（面板关了就别占着内存，
 * 下次显形重画 —— 一次重画就是几张小图，量可以忽略）。 */
static void mru_card_hide(struct vidow_mru_card *card, bool free_buf) {
	if (card == NULL) {
		return;
	}
	if (card->fx.node != NULL) {
		wlr_scene_node_set_enabled(&card->fx.node->node, false);
	}
	if (card->thumb != NULL) {
		wlr_scene_node_set_enabled(&card->thumb->node, false);
	}
	for (int k = 0; k < 4; k++) {
		if (card->fill_c[k] != NULL) {
			wlr_scene_node_set_enabled(&card->fill_c[k]->node, false);
		}
		if (card->ring_c[k] != NULL) {
			wlr_scene_node_set_enabled(&card->ring_c[k]->node, false);
		}
	}
	for (int k = 0; k < 3; k++) {
		if (card->fill_e[k] != NULL) {
			wlr_scene_node_set_enabled(&card->fill_e[k]->node, false);
		}
	}
	for (int k = 0; k < 4; k++) {
		if (card->ring_e[k] != NULL) {
			wlr_scene_node_set_enabled(&card->ring_e[k]->node, false);
		}
	}
	if (!free_buf) {
		return;
	}
	/* 离屏那张装饰图放掉（节点留着复用，下次显形 fx_begin 再开）。 */
	vidow_fx_end(NULL, &card->fx);
	if (card->thumb != NULL) {
		/* 缩略图那份 buffer 是客户端的，不是我们的：只是不再引用它。 */
		wlr_scene_buffer_set_buffer(card->thumb, NULL);
	}
	for (int k = 0; k < 4; k++) {
		if (card->fill_c[k] != NULL) {
			wlr_scene_buffer_set_buffer(card->fill_c[k], NULL);
		}
		if (card->ring_c[k] != NULL) {
			wlr_scene_buffer_set_buffer(card->ring_c[k], NULL);
		}
	}
	if (card->fill_buf != NULL) {
		wlr_buffer_drop(card->fill_buf);
		card->fill_buf = NULL;
	}
	if (card->ring_buf != NULL) {
		wlr_buffer_drop(card->ring_buf);
		card->ring_buf = NULL;
	}
	card->px = 0;
	card->bw = -1;
}

/* 全部收起（面板关了）。 */
static void mru_cards_hide(struct vidow_server *server, bool free_buf) {
	struct vidow_mru *mru = &server->mru;
	if (mru->card == NULL) {
		return;
	}
	for (int i = 0; i < mru->card_n; i++) {
		mru_card_hide(mru->card[i], free_buf);
	}
}

/* 这一项现在该长什么样：底色 / 描边色 / 描边宽（逻辑像素）。
 *   选中 —— 底 = 卡片底混一点强调色（portal 的 accent_soft 同配方），描边 = 强调色
 *           且加粗；
 *   悬停 —— 描边 = 强调色半透明（portal 的 hover 同此），底不动；
 *   其余 —— 卡片底 + 卡片描边色。 */
static void mru_card_style(const struct vidow_server *server,
		const struct vidow_view *view, float fill[4], float ring[4],
		int *bw) {
	const struct vidow_config *cfg = &server->config;
	const struct vidow_mru *mru = &server->mru;
	const float *hl = cfg->mru_highlight_color;
	memcpy(fill, cfg->mru_card_color, sizeof(float) * 4);
	memcpy(ring, hl, sizeof(float) * 4);
	if (view == mru->selected) {
		for (int i = 0; i < 3; i++) {
			fill[i] = cfg->mru_card_color[i] * 0.88f + hl[i] * 0.12f;
		}
		*bw = cfg->mru_highlight_border;
	} else if (view == mru->hover) {
		memcpy(ring, cfg->mru_hover_color, sizeof(float) * 4);
		*bw = cfg->mru_hover_border;
	} else {
		memcpy(ring, cfg->mru_card_border_color, sizeof(float) * 4);
		*bw = cfg->mru_card_border;
	}
}

/* 每项背后那张卡片 + 压暗的背景。 */
static void mru_scene_apply(struct vidow_server *server) {
	struct vidow_mru *mru = &server->mru;
	struct vidow_config *cfg = &server->config;
	if (mru->backdrop == NULL) {
		return;
	}
	bool on = mru_showing(server) && mru->output != NULL;
	if (!on) {
		wlr_scene_node_set_enabled(&mru->backdrop->node, false);
		mru_cards_hide(server, true);
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
		(float[4]){ 0.02f * alpha, 0.02f * alpha, 0.03f * alpha, alpha });
	wlr_scene_rect_set_size(mru->backdrop, box.width, box.height);
	wlr_scene_node_set_position(&mru->backdrop->node, box.x, box.y);
	wlr_scene_node_set_enabled(&mru->backdrop->node, true);

	/* 每一项背后一张卡片（滚出去 / 挤出去的那些不画）。 */
	for (int i = 0; i < mru->slot_n; i++) {
		const struct vidow_mru_slot *slot = &mru->slot[i];
		struct vidow_mru_card *card;
		if (slot->view == NULL) {
			/* 这一项滚出去了：它那张卡片收起来（图还留着，滚回来就不用重画）。 */
			if (mru->card != NULL && mru->card[i] != NULL) {
				mru_card_hide(mru->card[i], false);
			}
			continue;
		}
		card = mru_card_get(server, i);
		if (card == NULL) {
			continue;
		}
		float fill[4], ring[4];
		int bw = 0;
		mru_card_style(server, slot->view, fill, ring, &bw);
		struct wlr_box at = { slot->cx, slot->cy, slot->cw, slot->ch };
		mru_card_place(server, card, &at, fill, ring, bw);
		struct wlr_box thumb = { slot->x, slot->y, slot->w, slot->h };
		mru_card_thumb(server, card, slot->view, &thumb, slot->zoom);
	}
	/* 上一轮排了、这一轮没排到的（窗口少了 / 清单短了）也收起来。 */
	for (int i = mru->slot_n; i < mru->card_n; i++) {
		if (mru->card != NULL && mru->card[i] != NULL) {
			mru_card_hide(mru->card[i], false);
		}
	}
}

/* 清单 / 选中项变了：重排 + 重画自己那一层（窗口不动，所以不用碰 view）。 */
static void mru_refresh(struct vidow_server *server) {
	struct vidow_mru *mru = &server->mru;
	if (mru->n > 0) {
		mru_layout(server);
	}
	mru_scene_apply(server);
	/* 窗口一个都不动：切换器是浮层，只管自己那层（见 mru_scene_apply）。 */
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
	/* 清单按**屏幕顺序**排（左→右、同列上→下）：面板摆出来就是屏幕的小地图，
	 * 连点 Tab 也顺着这个顺序走。**一进来选中哪一扇**不在这儿定 —— 那看「最近
	 * 用过」，见 mru_pick_initial()。 */
	mru->n = 0;
	for (size_t i = 0; i < n; i++) {
		if (!mru_in_scope(server, list[i])) {
			continue;
		}
		mru->order[mru->n++] = list[i];
	}
	if (mru->n > 1) {
		qsort(mru->order, mru->n, sizeof(mru->order[0]), mru_cmp_screen);
	}
	if (n >= MRU_MAX) {
		wlr_log(WLR_INFO, "mru: 窗口多于 %d 个，只列这些", MRU_MAX);
	}
}

/* 一进来选中哪一扇：还是按**最近用过**（vidow_mru_collect 给的就是那个顺序），
 * 从最近的那头找第一个不是当前焦点的；dir < 0（Alt+Shift+Tab）从最旧的那头起。
 * 面板的顺序是屏幕顺序，选中的那一格跟着使用时间走 —— 两个顺序是分开的。 */
static struct vidow_view *mru_pick_initial(struct vidow_server *server, int dir) {
	struct vidow_mru *mru = &server->mru;
	struct vidow_view *list[MRU_MAX];
	size_t n = vidow_mru_collect(server, mru->filter_app_id, list, MRU_MAX);
	struct vidow_view *keep[MRU_MAX];
	size_t m = 0;
	for (size_t i = 0; i < n; i++) {
		if (mru_in_scope(server, list[i])) {
			keep[m++] = list[i];
		}
	}
	if (m == 0) {
		return NULL;
	}
	if (dir < 0) {
		return keep[m - 1];
	}
	for (size_t i = 0; i < m; i++) {
		if (keep[i] != server->focused) {
			return keep[i];
		}
	}
	return keep[0];
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
	/* 选中项：**最近用过**的那一扇（不是屏幕顺序里的第一个）—— Alt+Tab 一下
	 * 还是回到刚才那扇，接着按 Tab 才顺着屏幕往右走。 */
	mru->selected = mru_pick_initial(server, dir);
	if (mru->selected == NULL) {
		mru->selected = mru->order[0];
	}
	if (mru->open_timer != NULL && server->config.mru_open_delay_ms > 0) {
		wl_event_source_timer_update(mru->open_timer,
			server->config.mru_open_delay_ms);
	} else {
		mru_open_timer_fire(server);
	}
	wlr_log(WLR_INFO, "mru: 打开（%d 个窗口，%s，按屏幕顺序，延迟 %dms）", mru->n,
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
	mru->hover = NULL;
	mru->output = NULL;
	if (mru->open_timer != NULL) {
		wl_event_source_timer_update(mru->open_timer, 0);
	}
	mru_scene_apply(server);
	vidow_backdrop_damage(server, false);
	if (target != NULL && target->mapped && !target->minimized) {
		/* 明确选中 = 明确使用：直接落账，不等 debounce。 */
		vidow_focus_view(target);
		/* 切换器也是键盘选窗口，所以一样带光标过去（niri 的
		 * confirm_mru → focus_window 就是这么做的；默认关）。 */
		vidow_warp_cursor_to_focus(server);
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

/* --------------------------------------------------------------- 鼠标
 *
 * 切换器开着的时候指针归它管（见 input.c）：移动 = 点亮鼠标底下那一项，左键点
 * 一下 = 直接切过去（不必等松修饰键）。命中按**卡片**算 —— 卡片比预览大一圈，
 * 圆角外面那点边距也算点得中，跟 portal 的卡片一个手感。
 *
 * 悬停只改样式、不改选中：选中的那个还是键盘（Alt+Tab）在走，鼠标扫过去不该
 * 把它顶掉 —— 不然「按住 Alt 时指针恰好停在某一项上」会莫名其妙改选中项。 */

/* 鼠标底下那一项（没有 = NULL）。 */
static struct vidow_view *mru_slot_at(const struct vidow_server *server,
		double sx, double sy) {
	const struct vidow_mru *mru = &server->mru;
	for (int i = 0; i < mru->slot_n; i++) {
		const struct vidow_mru_slot *slot = &mru->slot[i];
		if (slot->view == NULL) {
			continue;
		}
		if (sx >= slot->cx && sx < slot->cx + slot->cw &&
				sy >= slot->cy && sy < slot->cy + slot->ch) {
			return slot->view;
		}
	}
	return NULL;
}

bool vidow_mru_hover(struct vidow_server *server, double sx, double sy) {
	struct vidow_mru *mru = &server->mru;
	if (!mru_showing(server)) {
		return false;
	}
	struct vidow_view *view = mru_slot_at(server, sx, sy);
	if (view == mru->hover) {
		return false;
	}
	mru->hover = view;
	mru_scene_apply(server);
	/* 这一下没有动画在跑，帧得自己催（场景损伤只记损伤，不排帧）。 */
	vidow_output_schedule_frame(server, mru->output, "切换器悬停");
	return true;
}

bool vidow_mru_click(struct vidow_server *server, double sx, double sy) {
	struct vidow_mru *mru = &server->mru;
	if (!mru_showing(server)) {
		return false;
	}
	/* 这次按下被切换器吃了，对应的松手也得吃掉：客户端收到一个没有按下配对的
	 * 松手会懵（和 overview 的 swallow_release 同一回事）。 */
	mru->swallow_release = true;
	struct vidow_view *view = mru_slot_at(server, sx, sy);
	if (view == NULL) {
		wlr_log(WLR_INFO, "mru: 点在空白处（窗口不动）");
		return true;
	}
	mru->selected = view;
	wlr_log(WLR_INFO, "mru: 点选 %s", mru_name(view));
	vidow_mru_commit(server);
	return true;
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
	mru->hover = NULL;
	mru->swallow_release = false;
	mru->card = NULL;
	mru->card_n = 0;
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
	/* 切换器是**浮层**：它不改窗口的排版、不搬窗口、不缩放窗口（niri 的窗口切换
	 * 器也是这样 —— 面板是新内容，原来那块屏照旧）。所以面板得有自己的、压在窗口
	 * 之上的一层：场景树按创建顺序叠（先建的在下），这里是最后建的，就在最上面。 */
	mru->tree = wlr_scene_tree_create(&server->scene->tree);
	if (mru->tree == NULL) {
		return false;
	}
	mru->backdrop = wlr_scene_rect_create(mru->tree, 1, 1,
		(float[4]){ 0.0f, 0.0f, 0.0f, 0.0f });
	if (mru->backdrop == NULL) {
		return false;
	}
	wlr_scene_node_set_enabled(&mru->backdrop->node, false);
	return true;
}
