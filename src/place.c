/* place：**「这块 surface 现在算不算在屏上、在哪几块屏的哪一块」的唯一答案。**
 *
 * 为什么要有这个文件：同样的判断在别处有 8 份实现（view_shows / should_park /
 * view_on_screen / in_output_box / view_visible_box / sliding_out / workspace_view_visible
 * / scene_parked，layer 那边还有自己一套）。它们之间只要有一处不一致，屏幕上就是
 * 碎片、残影、窗口跑到隔壁屏、滚出屏还不放补帧。2026-10-31 用户：
 * 「相同的判断需要统一在一个逻辑里面，不要碎片化」。
 *
 * 这里先只做**几何事实**（第 1 步，行为不变）：归属 home、世界坐标框 world、
 * 和每块屏的交集 hits。策略（工作区是否可见 / 最小化 / overview / 停泊）在第 2 步
 * 收进来，一起变成 place 的字段。
 *
 * 不变量（第 2 步之后全部成立，测试按它断言）：
 *   I1 n == 0  ⇔ 不参与任何渲染流程（节点关、补帧停、不进 MRU、不模糊、不记账）
 *   I2 n >= 1  ⇔ 在屏上，哪怕只露出 1px（不允许「半出屏就整块消失」）
 *   I3 每块屏只画 hits[i].box（world ∩ 该屏框），倍率用 hits[i].out 的
 *   I4 模糊区域 = hits[i].box ∩ 内容包围盒；包围盒空 = 不模糊
 *   I5 损伤 = 本帧 place 与上帧 place 的差，按屏分桶
 *   I6 浮窗 / 正被拖的窗口：home 不参与决定 hits，交集有几块算几块
 */
#include "server.h"

#include <stdlib.h>
#include <unistd.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

static void place_reset(struct vidow_place *p) {
	*p = (struct vidow_place){0};
}

/* 世界坐标框 vs 屏。only != NULL 时只跟那一块比 —— **平铺窗口只属于自己的屏**
 * （用户 2026-10-31：「平铺窗口不许溢出到所在显示器之外」）：滚动条带里滚到
 * 右边框外的列，几何框会落进隔壁屏的布局坐标里，跟隔壁比就「有点交集」了，
 * 于是被画到隔壁显示器上（这就是用户报的「滚出屏的窗口出现在另一块屏」）。
 * 只跟归属屏比，滚出去 = 没有交集 = 不存在，滚回来自然恢复。
 *
 * 浮窗 / 正被拖的窗口 / 弹窗不认归属（I6）：它们物理上就是能跨屏的，
 * 交集有几块算几块，每块屏各自按自己的倍率画自己那一半。 */
static void place_hits(struct vidow_server *server, struct vidow_place *p) {
	p->n = 0;
	if (p->world.width <= 0 || p->world.height <= 0 ||
			server->output_layout == NULL) {
		return;
	}
	struct vidow_output *o;
	wl_list_for_each(o, &server->outputs, link) {
		struct wlr_box out = {0};
		wlr_output_layout_get_box(server->output_layout, o->wlr_output,
			&out);
		if (out.width < 1 || out.height < 1) {
			continue;
		}
		struct wlr_box inter;
		if (!wlr_box_intersection(&inter, &p->world, &out)) {
			continue;
		}
		if (p->n >= VIDOW_PLACE_MAX_HITS) {
			break;
		}
		p->hits[p->n].out = o->wlr_output;
		p->hits[p->n].box = inter;
		p->hits[p->n].output_box = out;
		p->n++;
	}
}

struct wlr_box vidow_view_screen_box(const struct vidow_view *view);

/* 浮窗 / 正被拖的窗口 / 弹窗：物理上能跨屏（I6）。两类「正在拖」都得认 ——
 * 平铺窗口拖拽走 drag.c（view->dragging），浮窗拖动 / 列宽拖动走 input.c 那条
 * 抓取（server->grab_view）。只看其中一个会漏掉另一半。 */
static bool place_rides_outputs(const struct vidow_server *server,
		const struct vidow_view *view) {
	return view->popup || view->float_mode != VIDOW_FLOAT_NONE ||
		view->dragging || server->grab_view == view;
}

/* **策略层**：所有「该不该画」的原因在这儿合成成 resident + reason。
 *
 * 顺序照依赖：越早成立的理由越硬（未 map 的谈不上工作区；小窗快照根本不是这棵
 * 树在画；overview 能豁免工作区隐藏，但豁免不了最小化）。以前这些原因散在
 * vidow_view_should_park / workspace_view_visible / view_on_screen 三处，各自的
 * 顺序还不一样 —— 那正是碎片来源。 */
static void place_view_policy(struct vidow_server *server,
		struct vidow_view *view, struct vidow_place *p) {
	if (!view->mapped) {
		p->resident = false;
		p->reason = VIDOW_PLACE_UNMAPPED;
		return;
	}
	if (vidow_mini_uses_snapshot(view)) {
		/* 图标态：真实节点停到停车位，屏上由快照节点画（mini_snapshot_place）。 */
		p->resident = false;
		p->reason = VIDOW_PLACE_MINI;
		return;
	}
	bool visible = vidow_workspace_view_visible(server, view);
	if (!visible && !vidow_view_sliding_out(server, view) &&
			!(vidow_overview_active(server) && !view->minimized)) {
		p->resident = false;
		p->reason = view->minimized ? VIDOW_PLACE_MINIMIZED :
			VIDOW_PLACE_WS_HIDDEN;
		return;
	}
	/* 策略上要画。几何：overview 把窗口全缩进画面，再按几何框判会把「缩之前的
	 * 位置在屏外」的窗口误停；浮窗 / 拖拽看全部 hits（I6）；平铺 / 全屏只认
	 * 归属那块屏（I3：不许溢出到隔壁）。 */
	if (!vidow_overview_active(server) || view->minimized) {
		bool here = place_rides_outputs(server, view) ?
			vidow_place_any(p) : vidow_place_home_hit(p) != NULL;
		if (!here) {
			p->resident = false;
			p->reason = VIDOW_PLACE_OFFSCREEN;
			return;
		}
	}
	p->resident = true;
	p->reason = VIDOW_PLACE_OK;
}

void vidow_place_of_view(struct vidow_server *server, struct vidow_view *view,
		struct vidow_place *p) {
	place_reset(p);
	p->home = vidow_view_owner_output(view);
	/* 世界框：布局几何 + 动画偏移（拖拽 / 开合 / overview 都算在这上面）。 */
	p->world = vidow_view_screen_box(view);
	/* **hits 永远是纯几何**：和哪几块屏有交集就算几块。哪些消费者只认
	 * 「归属那块」由它自己挑访问器（见 server.h 的 vidow_place_home_hit）：
	 *   平铺 / 全屏渲染  → 只认 home（不许溢出到隔壁屏）
	 *   补帧 / 冻结      → 认任意一块（画在谁都算在屏上）
	 *   浮窗 / 拖拽      → 认全部（I6）
	 * 判据只有这一份，差的只是「看哪一行」。 */
	place_hits(server, p);
	place_view_policy(server, view, p);
}

void vidow_place_of_layer(struct vidow_server *server,
		struct vidow_layer_surface *ls, struct vidow_place *p) {
	place_reset(p);
	p->home = ls->output != NULL ? ls->output->wlr_output : NULL;
	/* geom 是布局坐标（见 layer.c 的 arrange，节点坐标已经含动画位移），
	 * 这里再加上动画偏移 —— 动画中途那一块也是「看得见」的一部分。 */
	p->world = (struct wlr_box){
		.x = ls->geom.x + (int)lround(ls->anim_off_x),
		.y = ls->geom.y + (int)lround(ls->anim_off_y),
		.width = ls->geom.width,
		.height = ls->geom.height,
	};
	place_hits(server, p);
	/* layer：没 map / 没 configure 过的层不画；其余只要跟自己那块屏有交集就画
	 * （layer 物理上不跨屏，geom 溢出隔壁屏那部分不画 —— 见第 3 步的夹取）。 */
	if (ls->layer_surface->surface == NULL ||
			!ls->layer_surface->surface->mapped ||
			ls->geom.width <= 0 || ls->geom.height <= 0) {
		p->resident = false;
		p->reason = VIDOW_PLACE_UNMAPPED;
	} else if (vidow_place_home_hit(p) == NULL) {
		p->resident = false;
		p->reason = VIDOW_PLACE_OFFSCREEN;
	} else {
		p->resident = true;
		p->reason = VIDOW_PLACE_OK;
	}
}

/* ------------------------------------------------------------ 对账（第 1 步）

 * 开关：/tmp/vidow-place-check（文件存在就开）。只在 arrange 收口时跑，2 秒最多
 * 一组日志 —— 它要做的是**找出新旧判据不一致的地方**，不是在线上做判断。
 *
 * 第 2 步之后策略已经收进 place，vidow_view_should_park / view_on_screen /
 * vidow_view_in_output_box / view_visible_box 全都改读 place —— 探针改成断言
 * **不变量**：scene_parked 必须等于 !resident；resident 必须有交集（overview
 * 例外）。任何一条不成立都是真 bug（碎片 / 残影的来源）。 */
static bool place_check_on(void) {
	static int64_t last;
	static bool on;
	int64_t now = vidow_now_ns();
	if (now - last < 500000000LL) {
		return on;
	}
	last = now;
	on = access("/tmp/vidow-place-check", F_OK) == 0;
	return on;
}

static const char *place_why(struct vidow_server *server,
		struct vidow_view *view) {
	if (!view->mapped) {
		return "未 map";
	}
	if (vidow_mini_uses_snapshot(view)) {
		return "mini 快照";
	}
	if (view->minimized) {
		return "最小化";
	}
	if (!vidow_workspace_view_visible(server, view)) {
		return vidow_overview_active(server) ? "overview 里" : "工作区被藏";
	}
	if (vidow_overview_active(server)) {
		return "overview";
	}
	return "未知";
}

void vidow_place_check(struct vidow_server *server, const char *tag) {
	if (server == NULL || !place_check_on()) {
		return;
	}
	int n_view = 0, n_bad = 0, n_resident = 0, n_layer = 0, n_overflow = 0;
	struct vidow_view *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->popup || view->scene_tree == NULL) {
			continue;
		}
		struct vidow_place p;
		vidow_place_of_view(server, view, &p);
		n_view++;
		if (p.resident) {
			n_resident++;
		}
		/* 不变量：场景节点的开关必须**就是** place 的结论（vidow_view_set_parked
		 * 的入口只该由它驱动）。不一致 = 场景里画着 place 说「不存在」的东西，
		 * 或者反过来 —— 碎片 / 残影就是这样来的。 */
		bool parked = view->scene_parked;
		if (parked == p.resident) {
			n_bad++;
			if (n_bad <= 8) {
				wlr_log(WLR_INFO, "PLACECHK %s: view %s geom=%dx%d%+d%+d "
					"hits=%d resident=%d reason=%d 但 parked=%d",
					tag, vidow_view_title(view) ?: "?",
					p.world.width, p.world.height, p.world.x, p.world.y,
					p.n, (int)p.resident, (int)p.reason, (int)parked);
			}
		}
		/* 不变量：resident 就必须在至少一块屏上有像素（overview 例外：那时
		 * 窗口被缩进画面，几何框还是缩之前的位置）。 */
		if (p.resident && p.n == 0 && !vidow_overview_active(server)) {
			n_bad++;
			if (n_bad <= 8) {
				wlr_log(WLR_INFO, "PLACECHK %s: view %s resident 但 hits=0",
					tag, vidow_view_title(view) ?: "?");
			}
		}
	}
	/* layer：geom 有没有越出它那块屏（现在只靠 wlroots 每屏裁剪兜着；
	 * 「逻辑上也不许溢出」是第 3 步）。 */
	struct vidow_layer_surface *ls;
	wl_list_for_each(ls, &server->layers, link) {
		if (ls->output == NULL || ls->geom.width <= 0 ||
				ls->geom.height <= 0) {
			continue; /* 还没 configure 过：摆都还没摆，不算溢出 */
		}
		struct vidow_place p;
		vidow_place_of_layer(server, ls, &p);
		n_layer++;
		struct wlr_box out = {0};
		wlr_output_layout_get_box(server->output_layout,
			ls->output->wlr_output, &out);
		struct wlr_box inter = out;
		bool hit = wlr_box_intersection(&inter, &p.world, &out);
		if (!hit || inter.width != p.world.width ||
				inter.height != p.world.height) {
			n_overflow++;
			if (n_overflow <= 6) {
				wlr_log(WLR_INFO, "PLACECHK %s: layer %s geom=%dx%d%+d%+d "
					"越出 %s 的框 %dx%d%+d%+d（可见 %dx%d）", tag,
					ls->layer_surface->namespace ?: "?",
					p.world.width, p.world.height, p.world.x, p.world.y,
					ls->output->wlr_output->name, out.width, out.height,
					out.x, out.y, hit ? inter.width : 0,
					hit ? inter.height : 0);
			}
		}
	}
	if (n_bad > 0 || n_overflow > 0) {
		wlr_log(WLR_INFO, "PLACECHK %s: views=%d 在屏=%d 不一致=%d | "
			"layers=%d 溢出=%d", tag, n_view, n_resident, n_bad,
			n_layer, n_overflow);
	}
}
