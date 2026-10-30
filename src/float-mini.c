#include "server.h"
#include "float-internal.h"
#include "ipc.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/util/log.h>

/* 迷你小窗（mini window）这一整套：甩出（S5）、三态（S1）、宽高比（S3b）、
 * 形态转换的过渡动画（S5）。
 *
 * 为什么单独一个文件：它和「浮动窗口」共用 VIDOW_FLOAT_* 那套状态，但没有任何
 * 反向依赖 —— 只调 server.h 的公共 API（vidow_mini_* / vidow_view_* / IPC 广播），
 * 所以搬迁是零成本。
 *
 * 甩出的三条设计决定（速度采样窗口、边缘判据、惯性时长）都在下面那段注释里。 */
/* -------------------------------------------------- 甩出（mini fling，S5）
 *
 * 用户要的「像安卓那样惯性甩出」：把迷你小窗快速甩向边缘 → 飞过去收成图标，
 * 不用慢慢拖到底。三条设计决定：
 *
 *  1) **速度按最近 ~100ms 的采样算**（8 格采样环，vidow_pointer_motion 每帧推
 *     一格），不是单帧增量 —— 单帧增量在高刷新率下小得量不出意图、在低刷新率
 *     下抖得离谱。至少 3 个点、且最后一点离松手不超过 80ms 才算数（指针停住
 *     一会儿再松手不是甩）。
 *  2) **方向取最近的那条边**（±45° 容差：横向速度占优就取左 / 右边）。落点
 *     判据沿用已定的那条：那条边外侧有相邻输出 → 换屏保持形态；没有 → 收成
 *     图标贴那条边。
 *  3) **不做真物理**：时长由速度推（越快越短，夹在 110ms..mini_transition_ms），
 *     位置曲线照旧 out_cubic（单调、**不过冲**）—— 边缘是吸附目标，不是自由
 *     物体的落点，越界再弹回来是错的。
 */
void vidow_mini_fling_sample(struct vidow_server *server, double x, double y) {
	if (server == NULL) {
		return;
	}
	for (int i = 1; i < 8; i++) {
		server->mini_fling_pts[i - 1] = server->mini_fling_pts[i];
	}
	server->mini_fling_pts[7].x = x;
	server->mini_fling_pts[7].y = y;
	server->mini_fling_pts[7].ns = vidow_now_ns();
	if (server->mini_fling_n < 8) {
		server->mini_fling_n++;
	}
}

bool vidow_mini_fling_velocity(struct vidow_server *server, double *vx,
		double *vy) {
	/* 至少 2 个点：快速一甩（144Hz 下 8 格采样环只覆盖 ~55ms）常常只有 2-3 个
	 * 点，要求 3 个 + dt ≥ 20ms 会让最快的那些甩动直接「没有速度」。 */
	if (server == NULL || server->mini_fling_n < 2) {
		return false;
	}
	int n = server->mini_fling_n;
	/* 采样环永远是「最新的那一格在 [7]、往前依次更早」：有效的是
	 * [8-n .. 7]，别拿 n-1 当末尾（那是「环形缓冲从头读」的写法，这里不是）。 */
	int last = 7;
	int oldest = 8 - n;
	if (vidow_now_ns() - server->mini_fling_pts[last].ns > 80000000LL) {
		return false;
	}
	int first = last;
	for (int i = last; i >= oldest; i--) {
		if (server->mini_fling_pts[last].ns -
				server->mini_fling_pts[i].ns > 100000000LL) {
			break;
		}
		first = i;
	}
	int64_t dt = server->mini_fling_pts[last].ns -
		server->mini_fling_pts[first].ns;
	if (dt < 8000000LL) {
		return false;
	}
	double secs = (double)dt / 1e9;
	*vx = (server->mini_fling_pts[last].x -
		server->mini_fling_pts[first].x) / secs;
	*vy = (server->mini_fling_pts[last].y -
		server->mini_fling_pts[first].y) / secs;
	return true;
}

/* 速度方向最近的那条边（±45° 容差）。 */
static enum vidow_mini_edge fling_edge(double vx, double vy) {
	double ax = fabs(vx), ay = fabs(vy);
	if (ax == 0.0 && ay == 0.0) {
		return VIDOW_MINI_EDGE_NONE;
	}
	if (ax >= ay) {
		return vx > 0.0 ? VIDOW_MINI_EDGE_RIGHT : VIDOW_MINI_EDGE_LEFT;
	}
	return vy > 0.0 ? VIDOW_MINI_EDGE_BOTTOM : VIDOW_MINI_EDGE_TOP;
}

bool vidow_mini_fling(struct vidow_server *server, struct vidow_view *view,
		double vx, double vy) {
	if (server == NULL || view == NULL || view->popup ||
			view->mini == VIDOW_MINI_NONE) {
		return false;
	}
	int threshold = server->config.mini_fling_speed;
	if (threshold <= 0) {
		return false;
	}
	double speed = hypot(vx, vy);
	if (speed < (double)threshold) {
		return false;
	}
	enum vidow_mini_edge edge = fling_edge(vx, vy);
	if (edge == VIDOW_MINI_EDGE_NONE) {
		return false;
	}
	/* 时长：越快越短。180000/speed 在 800px/s（阈值）时是 225ms、1600px/s 时
	 * 是 112ms；夹在 [110, mini_transition_ms] —— 再快也得看得见那一下，
	 * 再慢也不比普通转换更久（它是「甩」，不是「慢慢飞」）。 */
	int ms = (int)lround(180000.0 / speed);
	int cap = server->config.mini_transition_ms;
	if (cap <= 0) {
		cap = 260;
	}
	if (ms > cap) {
		ms = cap;
	}
	if (ms < 110) {
		ms = 110;
	}

	struct wlr_output *across =
		vidow_mini_output_across(server, view->output, edge);
	if (across != NULL) {
		/* 跨屏边：换屏、保持形态（锚点是比例，位置天然对得上）。 */
		wlr_log(WLR_INFO, "mini fling: %s 以 %.0f px/s 甩向 %s 边（外侧是 %s）"
			"→ 换屏保持 %s，%dms",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			speed,
			edge == VIDOW_MINI_EDGE_LEFT ? "左" :
				edge == VIDOW_MINI_EDGE_RIGHT ? "右" :
				edge == VIDOW_MINI_EDGE_TOP ? "上" : "下",
			across->name, vidow_mini_state_name(view->mini), ms);
		server->mini_transition_override_ms = ms;
		vidow_mini_move_to_output(server, view, across);
		return true;
	}

	/* 内边缘：收成图标，贴这条边 —— 沿边的那一轴按窗口**现在**的位置记锚点
	 * （甩出去的那一下不该把图标挪到屏幕另一头）。 */
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width > 0 && work.height > 0) {
		int iw = server->config.mini_icon_size;
		double span_x = (double)(work.width - iw);
		double span_y = (double)(work.height - iw);
		double cx = view->float_geom.x + view->float_geom.width / 2.0;
		double cy = view->float_geom.y + view->float_geom.height / 2.0;
		if (edge == VIDOW_MINI_EDGE_LEFT || edge == VIDOW_MINI_EDGE_RIGHT) {
			view->mini_ax = edge == VIDOW_MINI_EDGE_LEFT ? 0.0 : 1.0;
			view->mini_ay = span_y > 1.0 ?
				(cy - iw / 2.0 - (double)work.y) / span_y : 0.0;
		} else {
			view->mini_ay = edge == VIDOW_MINI_EDGE_TOP ? 0.0 : 1.0;
			view->mini_ax = span_x > 1.0 ?
				(cx - iw / 2.0 - (double)work.x) / span_x : 0.0;
		}
		if (view->mini_ax < 0.0) {
			view->mini_ax = 0.0;
		} else if (view->mini_ax > 1.0) {
			view->mini_ax = 1.0;
		}
		if (view->mini_ay < 0.0) {
			view->mini_ay = 0.0;
		} else if (view->mini_ay > 1.0) {
			view->mini_ay = 1.0;
		}
	}
	view->mini_edge = edge;
	wlr_log(WLR_INFO, "mini fling: %s 以 %.0f px/s 甩向 %s 边（内边缘）→ 收成"
		"图标，%dms",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		speed,
		edge == VIDOW_MINI_EDGE_LEFT ? "左" :
			edge == VIDOW_MINI_EDGE_RIGHT ? "右" :
			edge == VIDOW_MINI_EDGE_TOP ? "上" : "下",
		ms);
	server->mini_transition_override_ms = ms;
	vidow_mini_set_state(view, VIDOW_MINI_ICON);
	return true;
}

/* 跨屏边松手：换屏、保持形态。锚点是「相对那块屏工作区的比例」，所以位置和
 * 尺寸一个字都不用换算（vidow_mini_resolve_geom 会按新屏的工作区算出来）——
 * 这就是当初选比例锚点而不是绝对坐标的理由。 */
void vidow_mini_move_to_output(struct vidow_server *server,
		struct vidow_view *view, struct wlr_output *to) {
	if (server == NULL || view == NULL || to == NULL ||
			view->output == to) {
		return;
	}
	struct wlr_output *from = view->output;
	view->output = to;
	/* 跟屏的记账（热插拔按输出名认）：换屏就是换这块 memory。 */
	struct vidow_output *out = vidow_output_lookup(server, to);
	view->mini_home = out != NULL ? out->memory : NULL;
	/* 迷你 / 图标态的位置不靠 home_dx/home_dy（那是给普通浮窗的），FREE 才是
	 * 普通浮窗的摆放路径 —— 顺手把它刷新一下，免得以后从 FREE 转出来时用旧屏
	 * 的偏移摆一次（拖了等于没拖那个坑）。 */
	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, to, &box);
	if (box.width > 0) {
		view->home_dx = view->float_geom.x - box.x;
		view->home_dy = view->float_geom.y - box.y;
	}
	vidow_mini_resolve_geom(view);
	vidow_arrange(server);
	wlr_log(WLR_INFO, "mini: %s 从 %s 挪到 %s（形态 %s 不变，锚点 %.3f,%.3f）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		from != NULL ? from->name : "-", to->name,
		vidow_mini_state_name(view->mini), view->mini_ax, view->mini_ay);
	vidow_ipc_emit_window(server, "window-mini-changed", view);
}

/* 工具条按钮的动作。只读原则照 S2：这些动作是合成器自己的形态操作，不往客户端
 * 转任何事件（工具条本身也不是表面）。 */
void vidow_mini_toolbar_press(struct vidow_server *server,
		struct vidow_view *view, int button) {
	if (server == NULL || view == NULL) {
		return;
	}
	switch (button) {
	case VIDOW_MINI_BTN_EXPAND:
		/* 放大一档：图标 → 迷你 → 自由（自由是最上面那一档）。 */
		if (view->mini == VIDOW_MINI_ICON) {
			vidow_mini_set_state(view, VIDOW_MINI_MINI);
		} else if (view->mini == VIDOW_MINI_MINI) {
			vidow_mini_set_state(view, VIDOW_MINI_FREE);
			vidow_focus_view(view);
		}
		break;
	case VIDOW_MINI_BTN_ICON:
		vidow_mini_set_state(view, VIDOW_MINI_ICON);
		break;
	case VIDOW_MINI_BTN_RESTORE:
		vidow_mini_set_state(view, VIDOW_MINI_NONE);
		break;
	case VIDOW_MINI_BTN_CLOSE:
		vidow_view_close(view);
		break;
	default:
		return;
	}
	wlr_log(WLR_INFO, "mini toolbar: %s 点了按钮 %d（现在 %s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		button, vidow_mini_state_name(view->mini));
}

/* ------------------------------------------------- 小窗（mini window，S1）
 *
 * 数据模型见 server.h 的 enum vidow_mini_state 顶上那段。这里只有三件事：
 *   1) 形态字段的读写（vidow_mini_set_state）
 *   2) 比例锚点 -> float_geom 的换算（vidow_mini_resolve_geom）
 *   3) 进 / 出小窗态时的账：从工作区摘出来（view->ws = NULL）并把那块工作区
 *      的 floats 减掉 —— 不减的话一块只剩小窗的工作区永远收不掉
 *      （ws_keeps/ws_empty 看 ws->floats，见 workspace.c）。
 *
 * S1 只做数据模型 + 配置 + IPC：可见性旁路、焦点守卫、输入路由、渲染与动画
 * 一律还没动（S2–S5）。所以小窗态现在会被现有的可见性判据当成「工作区不亮着」
 * 而停到屏外 —— 这是预期的，S2 的旁路一上来它就留在屏上了。
 */

/* S3：小窗态要不要保留客户端原生分辨率（默认 true）。规则 > 全局键，
 * 和 mini_aspect 那套三级优先级无关 —— 这个只有两级（规则写了就是它）。
 *
 * **只管 MINI / ICON**（缩略图语义：保留原生 buffer，合成器缩着画，便宜且
 * 「内容比例不能变」正是它们要的）。FREE 恒为 false：它是自由摆放的正常窗口，
 * 盒子多大就 configure 客户端多大（客户端跟着重新排版）—— 保留缓冲那条路上，
 * 改盒子只改留白、内容比例一动不动，正是用户报的「改大小被限制了比例」。 */
bool vidow_mini_preserve_buffer(const struct vidow_view *view) {
	if (view == NULL) {
		return true;
	}
	if (view->mini == VIDOW_MINI_FREE) {
		return false;
	}
	if (view->mini_preserve_buffer_set) {
		return view->mini_preserve_buffer;
	}
	return view->server->config.mini_preserve_buffer;
}

const char *vidow_mini_state_name(enum vidow_mini_state state) {
	switch (state) {
	case VIDOW_MINI_FREE:
		return "free";
	case VIDOW_MINI_MINI:
		return "mini";
	case VIDOW_MINI_ICON:
		return "icon";
	case VIDOW_MINI_NONE:
		break;
	}
	return "none";
}

bool vidow_mini_parse_state(const char *text, enum vidow_mini_state *out) {
	if (text == NULL) {
		return false;
	}
	if (strcmp(text, "none") == 0) {
		*out = VIDOW_MINI_NONE;
	} else if (strcmp(text, "free") == 0) {
		*out = VIDOW_MINI_FREE;
	} else if (strcmp(text, "mini") == 0) {
		*out = VIDOW_MINI_MINI;
	} else if (strcmp(text, "icon") == 0) {
		*out = VIDOW_MINI_ICON;
	} else {
		return false;
	}
	return true;
}

const char *vidow_mini_aspect_name(enum vidow_mini_aspect aspect) {
	switch (aspect) {
	case VIDOW_MINI_ASPECT_AUTO:
		return "auto";
	case VIDOW_MINI_ASPECT_LOCK:
		return "lock";
	case VIDOW_MINI_ASPECT_FREE:
		return "free";
	case VIDOW_MINI_ASPECT_UNSET:
		break;
	}
	return "unset";
}

bool vidow_mini_parse_aspect(const char *text, enum vidow_mini_aspect *out) {
	if (text == NULL) {
		return false;
	}
	if (strcmp(text, "auto") == 0) {
		*out = VIDOW_MINI_ASPECT_AUTO;
	} else if (strcmp(text, "lock") == 0) {
		*out = VIDOW_MINI_ASPECT_LOCK;
	} else if (strcmp(text, "free") == 0) {
		*out = VIDOW_MINI_ASPECT_FREE;
	} else {
		return false;
	}
	return true;
}

/* ------------------------------------------- 宽高比（mini window，S3b，§11.3）
 *
 * 三级显式优先级：键位（运行时）> 窗口规则 > 全局键。**三个取值都是显式选择**
 * —— `free` 就是真的不锁（压过客户端的 PAspect），`auto` 才是「我选择按自动
 * 信号走」。判策略只有下面这一个出口，别在拖拽 / 摆放 / IPC 里各判一份。 */

/* 内容比例（w/h）。优先场景里的 1:1 尺寸（base_dst_*，X11 那条已经除过
 * x11_scale），没有就取表面自己的逻辑尺寸 —— 两者是同一个比例。**不要**直接
 * 拿 surface->current.width 混 X11 的缩放桥（计划 §11.3 那条）。 */
static bool mini_native_ratio(const struct vidow_view *view, double *ratio) {
	int nw = view->base_dst_w, nh = view->base_dst_h;
	/* FREE 的「内容比例」就是**当前**表面比例：FREE 不保留原生 buffer（盒子多大
	 * 就 configure 客户端多大），base_dst_* 那份 1:1 基线只在小窗态之外记（见
	 * view.c 的 view_dst_capture），进 FREE 之后它就是过期的 —— 拿它当内容比例
	 * 会把锁比例算错（实测锁着改窄：客户端被 configure 成 404 宽、高按过期的
	 * 0.89 算成 452，而表面自己的比例是 1.53）。 */
	if (view->mini == VIDOW_MINI_FREE || nw <= 0 || nh <= 0) {
		struct wlr_surface *surface = vidow_view_surface(view);
		if (surface != NULL && surface->current.width > 0 &&
				surface->current.height > 0) {
			nw = surface->current.width;
			nh = surface->current.height;
		}
	}
	if (nw <= 0 || nh <= 0) {
		return false;
	}
	*ratio = (double)nw / (double)nh;
	return true;
}

enum vidow_mini_aspect vidow_mini_aspect_effective(const struct vidow_view *view) {
	if (view == NULL) {
		return VIDOW_MINI_ASPECT_AUTO;
	}
	if (view->mini_aspect_runtime != VIDOW_MINI_ASPECT_UNSET) {
		return view->mini_aspect_runtime;
	}
	if (view->mini_aspect_rule != VIDOW_MINI_ASPECT_UNSET) {
		return view->mini_aspect_rule;
	}
	/* 全局键：配置面已经校验过取值，读不出来就当 auto（不静默退回别的值）。 */
	enum vidow_mini_aspect global = VIDOW_MINI_ASPECT_AUTO;
	vidow_mini_parse_aspect(view->server->config.mini_aspect, &global);
	return global;
}

bool vidow_mini_aspect_lock(const struct vidow_view *view, double *ratio) {
	/* 迷你 / 图标态永远按内容比例显示（contain），这条不受 mini_aspect 影响
	 * —— 那是「预览能不能被认出来」的呈现职责，不是用户偏好。 */
	if (view == NULL || view->mini != VIDOW_MINI_FREE) {
		return false;
	}
	switch (vidow_mini_aspect_effective(view)) {
	case VIDOW_MINI_ASPECT_FREE:
		return false;
	case VIDOW_MINI_ASPECT_LOCK: {
		/* 强制锁：没有 PAspect 也锁，比值取内容自己的比例。 */
		double r = 0.0;
		if (!mini_native_ratio(view, &r)) {
			return false;
		}
		if (ratio != NULL) {
			*ratio = r;
		}
		return true;
	}
	case VIDOW_MINI_ASPECT_AUTO:
	case VIDOW_MINI_ASPECT_UNSET:
		break;
	}
	/* auto：让自动信号决定。X11 的 PAspect 最权威（客户端自己声明的），其次
	 * min==max 的尺寸提示（固定尺寸窗口拖不动，锁比例是它的自然语义）。
	 * Wayland 协议没有比例提示 → 两条都不命中 → free。 */
	int num = 0, den = 0;
	if (vidow_view_aspect_hint(view, &num, &den) && num > 0 && den > 0) {
		if (ratio != NULL) {
			*ratio = (double)num / (double)den;
		}
		return true;
	}
	int fw = 0, fh = 0;
	if (vidow_view_fixed_size_hint(view, &fw, &fh) && fw > 0 && fh > 0) {
		if (ratio != NULL) {
			*ratio = (double)fw / (double)fh;
		}
		return true;
	}
	return false;
}

void vidow_mini_aspect_changed(struct vidow_view *view) {
	if (view == NULL) {
		return;
	}
	/* 只有自由小窗在乎：迷你 / 图标态的比例跟这个键无关。 */
	if (view->mini != VIDOW_MINI_FREE) {
		return;
	}
	double ratio = 0.0;
	bool lock = vidow_mini_aspect_lock(view, &ratio);
	int num = 0, den = 0;
	bool hinted = vidow_view_aspect_hint(view, &num, &den);
	wlr_log(WLR_INFO, "mini: %s 比例信号变了（PAspect %s，有效值 %s → %s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		hinted ? "有" : "没有",
		vidow_mini_aspect_name(vidow_mini_aspect_effective(view)),
		lock ? "锁" : "不锁");
	if (!lock || ratio <= 0.0 || view->float_geom.width <= 0 ||
			view->float_geom.height <= 0) {
		return;
	}
	/* 锁着但盒子还不是那个比例（客户端**映射之后**才设 PAspect 的常见情况：
	 * 进小窗那一刻还没有信号，盒子是配置的初始比例）→ 按宽度重排盒子，让它
	 * 当场变成锁定的形状。宽度驱动，和拖拽那条一致。 */
	double want = (double)view->float_geom.width / ratio;
	if (fabs(want - (double)view->float_geom.height) < 1.0) {
		return;
	}
	if (want < FLOAT_MIN_SIZE) {
		want = FLOAT_MIN_SIZE;
	}
	view->float_geom.height = (int)lround(want);
	view->mini_hf = view->float_geom.height /
		(double)vidow_work_area(view->server, view->output).height;
	vidow_float_set_size(view, view->float_geom.width, view->float_geom.height);
	vidow_arrange(view->server);
}

/* 配置里的百分比键（"28%"）-> 比例（0.28）。读不出来就用兜底值 ——
 * 写配置那一刻已经校验过（config.c 的 K_STRING 分支），这里不该再报错。 */
static double mini_frac(struct vidow_server *server, const char *key,
		double fallback) {
	const char *text = vidow_config_get(&server->config, key);
	double pct = 0.0;
	if (text != NULL && text[0] != '\0' &&
			vidow_config_parse_percent(text, &pct)) {
		return pct / 100.0;
	}
	return fallback;
}

/* 进各态的默认锚点：自由小窗居中，迷你 / 图标贴右上角（落点区的默认角）。
 * 只在「本来不在小窗态」时给一次，三态互切不动它（位置是用户拖出来的）。 */
static void mini_default_anchor(struct vidow_view *view,
		enum vidow_mini_state state) {
	switch (state) {
	case VIDOW_MINI_FREE:
		view->mini_ax = 0.5;
		view->mini_ay = 0.5;
		break;
	case VIDOW_MINI_MINI:
		view->mini_ax = 1.0;
		view->mini_ay = 0.0;
		break;
	case VIDOW_MINI_ICON:
		view->mini_ax = 1.0;
		view->mini_ay = 0.0;
		view->mini_edge = VIDOW_MINI_EDGE_RIGHT;
		break;
	case VIDOW_MINI_NONE:
		break;
	}
}

void vidow_mini_resolve_geom(struct vidow_view *view) {
	if (view == NULL || view->mini == VIDOW_MINI_NONE || view->popup) {
		return;
	}
	struct vidow_server *server = view->server;
	struct wlr_box work = vidow_work_area(server, view->output);
	if (work.width < 1 || work.height < 1) {
		/* 没有屏（嵌套会话的外层窗口关了 / 最后一块屏拔了）：没得算，留着
		 * 上一次的盒子。屏回来时 arrange 会再算一遍。 */
		return;
	}
	const struct vidow_config *cfg = &server->config;

	int w = 0, h = 0;
	switch (view->mini) {
	case VIDOW_MINI_FREE:
		if (!view->mini_size_known) {
			view->mini_wf = mini_frac(server, "mini_free_w", 0.40);
			view->mini_hf = mini_frac(server, "mini_free_h", 0.45);
		}
		w = (int)lround(view->mini_wf * (double)work.width);
		h = (int)lround(view->mini_hf * (double)work.height);
		break;
	case VIDOW_MINI_MINI:
		w = (int)lround(mini_frac(server, "mini_w", 0.28) *
			(double)work.width);
		h = (int)lround(mini_frac(server, "mini_h", 0.28) *
			(double)work.height);
		break;
	case VIDOW_MINI_ICON:
		w = h = cfg->mini_icon_size;
		break;
	case VIDOW_MINI_NONE:
		return;
	}
	if (w < FLOAT_MIN_SIZE) {
		w = FLOAT_MIN_SIZE;
	}
	if (h < FLOAT_MIN_SIZE) {
		h = FLOAT_MIN_SIZE;
	}
	if (w > work.width) {
		w = work.width;
	}
	if (h > work.height) {
		h = work.height;
	}

	/* 锚点 -> 绝对位置：0 = 贴工作区左 / 上边，1 = 贴右 / 下边。图标允许
	 * 超出 0..1（夹在 -0.5..1.5），好让它按 mini_icon_overflow 出屏一点。 */
	/* 锚点 -> 绝对位置：0 = 贴工作区左 / 上边，1 = 贴右 / 下边。
	 * FREE 允许越界（拖动里出屏半个盒子，盒子接近满屏时 w/(2*span) 能到 0.5，
	 * 所以兜底夹 -0.5..1.5）；MINI / ICON **夹在 0..1** —— 它们本来就该在工作区
	 * 里，图标那点出屏是下面 mini_icon_overflow 额外加的偏移。以前 MINI / ICON
	 * 也照 -0.5..1.5 兜底 = 允许半个工作区在外面：迷你盒整个能被拖出屏幕。 */
	double ax = view->mini_ax, ay = view->mini_ay;
	/* 迷你态和自由态**同一套夹取**（-0.5..1.5）：用户要的就是「两者一样」——
	 * 他原话「迷你态在工作区有左右死区，无法拖拽过去，自由态可以拖过去」。上一轮
	 * 我把迷你态单独夹在 0..1（当时是为了修视频里「盒子整个拖出屏、两秒看不见」），
	 * 结果正好造出他报的左右死区 —— 迷你态再也推不到工作区边上、更推不出去。
	 * 图标态走它自己的出屏偏移（mini_icon_overflow），同样不夹死。 */
	double lo = -0.5, hi = 1.5;
	if (ax < lo) {
		ax = lo;
	} else if (ax > hi) {
		ax = hi;
	}
	if (ay < -0.5) {
		ay = -0.5;
	} else if (ay > 1.5) {
		ay = 1.5;
	}
	int x = work.x + (int)lround(ax * (double)(work.width - w));
	int y = work.y + (int)lround(ay * (double)(work.height - h));

	if (view->mini == VIDOW_MINI_ICON) {
		double ov = cfg->mini_icon_overflow;
		if (ov < 0.0) {
			ov = 0.0;
		} else if (ov > 0.6) {
			ov = 0.6;
		}
		int dx = (int)lround(ov * (double)w);
		int dy = (int)lround(ov * (double)h);
		switch (view->mini_edge) {
		case VIDOW_MINI_EDGE_LEFT:
			x -= dx;
			break;
		case VIDOW_MINI_EDGE_RIGHT:
			x += dx;
			break;
		case VIDOW_MINI_EDGE_TOP:
			y -= dy;
			break;
		case VIDOW_MINI_EDGE_BOTTOM:
			y += dy;
			break;
		case VIDOW_MINI_EDGE_NONE:
			break;
		}
	} else {
		/* 边距**只给迷你态**：它是「贴边的小窗」，留一点缝好看。
		 * 自由小窗（FREE）和普通浮窗（NONE）都不吃 —— 它们能贴到工作区边上。
		 *
		 * 用户报的「迷你态和自由态一样的啊，现在还是左右有死区」就是这里：以前
		 * 只有 FREE 被特判成 0，MINI 和 NONE 都拿 mini_edge_margin（8px）—— 于是
		 * 「迷你态」和「普通浮窗/自由态」在左右各留 8px 进不去，一模一样。普通
		 * 浮窗更冤：它不是小窗家族的东西，凭什么吃小窗的边距。 */
		/* 内缩 = 0：**迷你态和自由态一模一样**（用户原话：「迷你态在工作区有
		 * 左右死区，无法拖拽过去，自由态可以拖过去」）。死区实测正好 8px = 旧
		 * 的 cfg->mini_edge_margin，两态都吃它 —— 这就是他说的「两者不一样 /
		 * 左右有死区」。mini_edge_margin 这个配置项留着（旧配置里有它，别报
		 * 未知键），但不再参与位置计算。 */
		int margin = 0;
		if (margin < 0) {
			margin = 0;
		}
		/* **迷你态和自由态一样**能出屏半个盒子（用户：「迷你态在工作区有左右
		 * 死区，无法拖拽过去，自由态可以拖过去」+「死区显然不止 8px」）。这里
		 * 以前只有 FREE 拿 w/2、h/2，MINI 是 0 —— 兜底夹回工作区就等于「工作区
		 * 左右那两片进不去」：盒子左边缘最多停在 work.x，离屏边还差整整半个
		 * 盒子。8px 那点只是它的尾巴（mini_edge_margin），大头是这儿。 */
		int out_x = w / 2;
		int out_y = h / 2;
		if (x < work.x + margin - out_x) {
			x = work.x + margin - out_x;
		}
		if (y < work.y + margin - out_y) {
			y = work.y + margin - out_y;
		}
		if (x + w > work.x + work.width - margin + out_x) {
			x = work.x + work.width - margin + out_x - w;
		}
		if (y + h > work.y + work.height - margin + out_y) {
			y = work.y + work.height - margin + out_y - h;
		}
	}

	view->float_geom.x = x;
	view->float_geom.y = y;
	view->float_geom.width = w;
	view->float_geom.height = h;
}

/* ------------------------------------------ 形态转换的过渡动画（S5，§1.4）
 *
 * 两条硬规矩，写在这儿当注释钉住：
 *
 *  1) **起点严格取「上一帧画在哪」**：`geom + anim_off`，和 view.c 的
 *     `vidow_place_view()` 里算 drawn_x/drawn_y 的那两行**完全同源**，而且
 *     必须在**改几何之前**取（下面 set_state 第一行就是它）。起点存成
 *     「起点 − 目标」的常量差值，update() 里**绝不重读 view->geom** ——
 *     转换中途夹着 arrange，geom 会被改写，重读 = 起点漂移。
 *     `daeba31` 的教训（计划 §1.4 引了原文）：那次用「起点落在输出框里就推到
 *     框的一侧」这种推断，正常滚动时也命中 → 每滚一列窗口都从屏幕外飞入。
 *     这里**不做任何推断**：起点是什么就是什么，哪怕它在屏外。
 *
 *  2) **位置曲线一律单调**（out_cubic），`out_back` 只用在**缩放**上 ——
 *     「中间帧位置单调」因此是构造性保证，测试不用给过冲开例外。
 *
 * 位置 / 缩放都走现成的 anim_off_x/anim_off_y 与 anim_scale（view_anim_place
 * 每帧读它们，fx.c 按窗口中心缩放），不新造第二条摆放管线。
 */
static const struct vidow_ease MINI_POS_EASE = {
	.kind = VIDOW_EASE_OUT_CUBIC,
};

static void mini_anim_apply(struct vidow_view *view, double t, double ts) {
	/* t = 位置进度（单调曲线），ts = 缩放进度（可能过冲）。 */
	view->anim_off_x = view->mini_from_dx * (1.0 - t);
	view->anim_off_y = view->mini_from_dy * (1.0 - t);
	double s = 1.0 + (view->mini_scale_from - 1.0) * (1.0 - ts);
	if (s < 0.01) {
		s = 0.01;
	}
	view->anim_scale = s;
	vidow_view_apply_anim(view);
	wlr_log(WLR_DEBUG, "mini anim: %s t=%.3f 画在 %d,%d（偏移 %.1f,%.1f）缩放 %.3f",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		t, view->geom.x + (int)lround(view->anim_off_x),
		view->geom.y + (int)lround(view->anim_off_y),
		view->anim_off_x, view->anim_off_y, view->anim_scale);
}

static void mini_anim_update(void *data, double t, double linear) {
	struct vidow_view *view = data;
	mini_anim_apply(view, t,
		vidow_ease_value(&view->mini_scale_ease, linear));
}

static void mini_anim_done(void *data, bool cancelled) {
	struct vidow_view *view = data;
	view->mini_anim = NULL;
	if (!cancelled) {
		/* 收尾：偏移归零、缩放归 1 —— 不归零的话窗口会永远画在差一点的地方。 */
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		view->anim_scale = 1.0;
	}
	vidow_view_apply_anim(view);
}

/* 形态转换的收口（set_state 的最后一步）：起点 `from` 是**改几何之前**取的
 * 「上一帧画在哪」，view->geom 这时已经是这一趟 arrange 摆出来的目标盒。
 *
 * `from_scale` 是同一时刻的「上一帧内容的缩放」（set_state 里在改之前问
 * vidow_view_mini_scale），**不是**从盒子面积推出来的那个 —— 见下面 scale_from
 * 的注释：推出来的那一个只在「内容正好填满盒子」时才等于屏上真正画的那个。 */
static void mini_transition_start(struct vidow_view *view,
		const struct wlr_box *from, double from_scale) {
	struct vidow_server *server = view->server;
	/* 上一趟还在飞就被打断了：先掐掉（cancelled 那条不清偏移 —— 新的这条
	 * 紧接着会把偏移重写成新起点，中途不清才不会闪）。 */
	vidow_anim_cancel(view->mini_anim);
	view->mini_anim = NULL;
	/* arrange 里 vidow_place_view 起的那条普通位移补间让位（它用的是
	 * window_move_ms 和全局曲线，也不动缩放）。 */
	vidow_anim_cancel(view->anim_move);
	view->anim_move = NULL;

	int ms = server->config.mini_transition_ms;
	/* 甩出：时长由速度推出来（越快越短），一次性的覆盖，读完就清 —— 不覆盖
	 * 配置键，也不留在下一次转换上。 */
	if (server->mini_transition_override_ms > 0) {
		ms = server->mini_transition_override_ms;
		server->mini_transition_override_ms = 0;
	}
	double dx = (double)(from->x - view->geom.x);
	double dy = (double)(from->y - view->geom.y);
	/* 起点缩放：view_anim_place 每帧画出来的是 **ms(新状态) × anim_scale**，
	 * 所以 anim_scale 的起点要取「上一帧的内容缩放 ÷ 新状态的 contain 系数」——
	 * 这样 t = 0 那一帧画出来的内容尺寸和上一帧**逐像素一样**（daeba31 那条
	 * 教训：补间起点必须是「上一帧画在哪」，不能是推出来的量）。
	 *
	 * 以前这里取 sqrt(盒面积比)：只在「内容正好填满盒子」时才等于屏上真正画的
	 * 那个（`sqrt` 是面积比的几何平均，撑不起 contain 的 min）。实测的偏差：
	 * 自由小窗（盒 504x315、内容 625x700 原生、contain 0.4257）还原成列
	 * （目标 625x700）时起点取到 0.602 → 退出动画第一帧的内容比上一帧大
	 * **41%**（用户报的「退出小窗的一瞬间显示原始大小」）；图标态那趟同理
	 * （0.097 vs 真正的 0.0865）。 */
	double to_scale = vidow_view_mini_scale_export(view);
	double scale_from = to_scale > 0.0 ? from_scale / to_scale : 1.0;
	double area_from = (double)from->width * (double)from->height;
	double area_to = (double)view->geom.width * (double)view->geom.height;
	if (ms <= 0 || (dx == 0.0 && dy == 0.0 &&
			fabs(scale_from - 1.0) < 0.01)) {
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		view->anim_scale = 1.0;
		vidow_view_apply_anim(view);
		return;
	}

	view->mini_from_dx = dx;
	view->mini_from_dy = dy;
	view->mini_scale_from = scale_from;
	/* 缩放曲线：**变大**（展开）给一点回弹（out_back，幅度取全局
	 * back_overshoot 反解出来的常数）；变小用 out_cubic 收住。位置永远是
	 * out_cubic —— 见本节顶上第 2 条。 */
	bool expanding = area_from < area_to;
	if (expanding) {
		double c1 = vidow_ease_out_back_c1(server->config.back_overshoot);
		view->mini_scale_ease = (struct vidow_ease){
			.kind = VIDOW_EASE_OUT_BACK, .back_c1 = c1,
		};
	} else {
		view->mini_scale_ease = (struct vidow_ease){
			.kind = VIDOW_EASE_OUT_CUBIC,
		};
	}

	/* 先把 t = 0 那一帧摆出来（第一帧就必须画在起点，不能等下一次 tick ——
	 * 判据 19 查的就是这个）。 */
	mini_anim_apply(view, 0.0, 0.0);
	view->mini_anim = vidow_anim_start(server, "mini-transition", ms,
		MINI_POS_EASE, mini_anim_update, mini_anim_done, view);
	if (view->mini_anim == NULL) {
		view->anim_off_x = 0.0;
		view->anim_off_y = 0.0;
		view->anim_scale = 1.0;
		vidow_view_apply_anim(view);
		return;
	}
	wlr_log(WLR_INFO, "mini: 过渡动画 %s → 起点 %d,%d（%.1f,%.1f 偏移）→ "
		"目标 %d,%d %dx%d，缩放 %.3f → 1，%dms",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		from->x, from->y, dx, dy,
		view->geom.x, view->geom.y, view->geom.width, view->geom.height,
		scale_from, ms);
}

void vidow_mini_set_state(struct vidow_view *view, enum vidow_mini_state state) {
	if (view == NULL || view->popup) {
		return;
	}
	struct vidow_server *server = view->server;
	enum vidow_mini_state old = view->mini;
	if (old == state) {
		return;
	}
	/* 过渡动画的起点：**改几何之前**的「上一帧画在哪」（§1.4 第 1 条）。
	 * 对小窗态来说「画在哪」是**内容**的原点，不是盒子的原点 —— 内容在盒子里
	 * 是 contain 居中摆的（S3b，见 view.c 的 mini_cx/mini_cy），盒子原点到
	 * 内容原点还差一个居中偏移。不把它算进来的话，退出小窗的第一帧会横着跳
	 * 一下（实测自由小窗那趟差 119px，比动画本身一步的位移还大）。 */
	int content_dx = 0, content_dy = 0;
	vidow_view_mini_content_offset(view, &content_dx, &content_dy);
	double from_scale = vidow_view_mini_scale_export(view);
	struct wlr_box from = {
		.x = view->geom.x + (int)lround(view->anim_off_x) + content_dx,
		.y = view->geom.y + (int)lround(view->anim_off_y) + content_dy,
		.width = view->geom.width,
		.height = view->geom.height,
	};

	/* S2：迷你 / 图标**不接键盘**，所以进这两个态之前先把焦点交出去（规矩照抄
	 * 最小化那条：位置不动，谁滑到空出来的位置就是谁）。必须在下面
	 * vidow_float_set_mode() 摘列**之前** —— focus_after_close() 要靠窗口还挂在
	 * 列里才找得到邻居。FREE 照常拿焦点，不用交。 */
	if (state == VIDOW_MINI_MINI || state == VIDOW_MINI_ICON) {
		vidow_view_yield_focus_for_mini(view);
	}
	/* 小窗（S4）：形态一变，悬停工具条就收掉（它的位置是照**上一个**形态的
	 * 盒子算的，留着会画在一个已经不在那儿的窗口旁边）。下一次指针动会重新
	 * 露出来。 */
	vidow_mini_toolbar_hide(server);
	/* 小窗（S5）：形态一变，悬停预览也收掉（预览画的是**上一个**形态旁边的
	 * 那块，留着会停在已经不在那儿的图标旁边）。 */
	vidow_mini_preview_hide(server);
	/* 键位动作用的指认（迷你 / 图标态不拿键盘焦点，server->focused 指不到）。 */
	if (state != VIDOW_MINI_NONE) {
		server->mini_last_id = view->id;
	}

	if (state == VIDOW_MINI_NONE) {
		view->mini = VIDOW_MINI_NONE;
		view->mini_edge = VIDOW_MINI_EDGE_NONE;
		/* S3b：键位翻转是「这一次小窗」的运行时选择，还原成列就忘掉 —— 下次
		 * 进小窗重新按 规则 > 全局键 判。 */
		view->mini_aspect_runtime = VIDOW_MINI_ASPECT_UNSET;
		/* 还原成列。小窗本来不属于任何工作区，先认领回自己那块屏显示着的
		 * 工作区（屏没了就留 NULL，view_ensure_column 会安置到一块无屏的
		 * 工作区上，屏回来时跟着认领回来）。浮窗记账不减 —— 窗口这就回列了，
		 * 列里的窗口本来就不算 ws->floats（见 vidow_workspace_track）。 */
		if (view->output != NULL) {
			view->ws = vidow_workspace_shown_for(server, view->output);
		}
		vidow_float_set_mode(view, VIDOW_FLOAT_NONE);
		/* S5：接回条带也走过渡动画（目标盒是 join_layout 算出来的条带位置，
		 * 上面那次 arrange 已经把它摆进 geom 了）。 */
		mini_transition_start(view, &from, from_scale);
		wlr_log(WLR_INFO, "mini: %s 还原成列（从 %s）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			vidow_mini_state_name(old));
		vidow_ipc_emit_window(server, "window-mini-changed", view);
		return;
	}

	/* 进小窗态 / 三态互切。小窗是浮动窗口上的一个形态：先把浮动这件事办齐
	 * （float_mode = FIXED，从条带摘走、种子 float_geom），再谈形态。 */
	bool was_float = view->float_mode != VIDOW_FLOAT_NONE;
	if (view->float_mode != VIDOW_FLOAT_FIXED) {
		vidow_view_unfullscreen(view, "进小窗");
		vidow_float_set_mode(view, VIDOW_FLOAT_FIXED);
	}
	if (old == VIDOW_MINI_NONE) {
		mini_default_anchor(view, state);
		/* 小窗是**屏级对象**：从工作区摘出来。这一步的账必须减 ——
		 * ws_keeps() = named || !ws_empty(ws)，而 ws_empty() 看 ws->floats，
		 * 不减的话一块只剩小窗的工作区永远收不掉（vidow_workspace_cleanup
		 * 一直跳过它），在 vidowmsg workspaces 和 overview 里永远多一行。
		 *
		 * **但只有本来就占着这笔账的那个才减**：ws->floats 的 +1 只在
		 * view_map()（src/view.c，`float_mode != NONE` 那条）与 ws_adopt_view()
		 * 里记（vidow_view_leave_column() 自己不动 floats —— 平铺窗口的账在
		 * columns 上）。平铺窗口进小窗时摘掉那条列已经够了，这里再减一下就是
		 * 减**别人**的账：同一块工作区里那个真浮窗的计数会被吃掉（实测：
		 * 浮窗 miniwin + 平铺 tilewin 进小窗，floats 1 → 0，miniwin 的账没了）。 */
		view->mini_home = NULL;
		if (view->output != NULL) {
			struct vidow_output *out =
				vidow_output_lookup(server, view->output);
			view->mini_home = out != NULL ? out->memory : NULL;
		}
		struct vidow_workspace *ws = view->ws;
		view->ws = NULL;
		if (ws != NULL && was_float) {
			vidow_workspace_track(ws, view, false);
		}
	}
	if (state == VIDOW_MINI_FREE) {
		/* FREE 的尺寸：记过就接着用（mini_remember_size 关掉 = 每次都用
		 * 配置的初始比例）。MINI / ICON 不记 —— 尺寸永远跟配置。 */
		if (!view->mini_size_known || !server->config.mini_remember_size) {
			view->mini_wf = mini_frac(server, "mini_free_w", 0.40);
			view->mini_hf = mini_frac(server, "mini_free_h", 0.45);
			view->mini_size_known = true;
		}
	}
	view->mini = state;

	vidow_mini_resolve_geom(view);
	if (view->float_geom.width > 0 && view->float_geom.height > 0) {
		/* 尺寸是客户端的事：configure 一次，并把 float_want_w/h 钉住
		 * （见 vidow_float_set_size 的说明）。 */
		vidow_float_set_size(view, view->float_geom.width,
			view->float_geom.height);
	}
	vidow_arrange(server);
	wlr_log(WLR_INFO, "mini: %s %s -> %s，%dx%d @ %d,%d（锚点 %.3f,%.3f，屏 %s）",
		vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
		vidow_mini_state_name(old), vidow_mini_state_name(state),
		view->float_geom.width, view->float_geom.height,
		view->float_geom.x, view->float_geom.y,
		view->mini_ax, view->mini_ay,
		view->output != NULL ? view->output->name : "-");
	if (state == VIDOW_MINI_FREE) {
		/* S3b：把「这一级算出来的宽高比策略」说清楚 —— 用户报「设了 PAspect 的
		 * 游戏还是锁着 / 没锁」时，这一行能直接看出是哪一级（键位 / 规则 /
		 * 全局键）说了算、自动信号有没有被读到。 */
		int anum = 0, aden = 0;
		double aratio = 0.0;
		bool alock = vidow_mini_aspect_lock(view, &aratio);
		bool ahint = vidow_view_aspect_hint(view, &anum, &aden);
		wlr_log(WLR_INFO, "mini: %s 宽高比 有效值=%s（键位 %s / 规则 %s / 全局 %s），"
			"PAspect=%s，min==max=%s → %s（比值 %.4f）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?",
			vidow_mini_aspect_name(vidow_mini_aspect_effective(view)),
			vidow_mini_aspect_name(view->mini_aspect_runtime),
			vidow_mini_aspect_name(view->mini_aspect_rule),
			server->config.mini_aspect,
			ahint ? "有" : "没有",
			vidow_view_fixed_size_hint(view, &anum, &aden) ? "有" : "没有",
			alock ? "锁" : "不锁", aratio);
	}
	/* S5：图标态的出帧策略（§9②）—— 退化情形说清楚：客户端一帧都还没交过
	 * （last_buffer 空）时没有快照可用，只能退回活节点（客户端继续出帧），
	 * 屏上照旧是动的图标。 */
	if (state == VIDOW_MINI_ICON && !view->force_render &&
			view->last_buffer == NULL) {
		wlr_log(WLR_INFO, "mini: %s 进图标态但手里没有最后一帧（客户端还没交过"
			"帧）→ 退回活节点、客户端继续出帧（force_render 关）",
			vidow_view_title(view) != NULL ? vidow_view_title(view) : "?");
	}
	/* S5：进小窗 / 三态互切 —— 上面那次 arrange 已经把目标盒摆进 geom 了，
	 * 这里起过渡动画（起点是进函数时取的「上一帧画在哪」）。 */
	mini_transition_start(view, &from, from_scale);
	/* 形态刚定下来：出帧策略（快照 / 活）与快照节点跟着走一趟。 */
	vidow_mini_frames_sync(view);
	vidow_ipc_emit_window(server, "window-mini-changed", view);
}

