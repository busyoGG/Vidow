/* 触控板手势：表 + 事件派发 + 连续拖视口。见 gesture.h 的说明。 */
#include "gesture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>

#include "ipc.h"
#include "server.h"

/* 正在进行的那一个手势（触控板一次只发一个手势，没有并发）。 */
struct gesture_state {
	bool active;
	enum vidow_gesture_kind kind;
	int fingers;
	/* 离散方向：累计位移（libinput 的归一化单位，按轴分开） */
	double acc_x, acc_y;
	/* 捏合：累计的 scale（相对手势开始） */
	double scale;
	/* 连续视口 */
	struct vidow_output *output;
	double base;  /* 手势开始时的视口 */
	double moved; /* 手指拖出来的像素（已乘 speed） */
	double speed; /* 这条手势的「单位 → 像素」系数 */
	double snap_from, snap_to;
	struct vidow_anim *snap; /* 松手吸附的那段动画 */
	struct vidow_server *server;
};
static struct gesture_state g;

static struct vidow_output *output_from_wlr(struct vidow_server *server,
		struct wlr_output *wlr) {
	if (wlr == NULL) {
		return NULL;
	}
	struct vidow_output *out;
	wl_list_for_each(out, &server->outputs, link) {
		if (out->wlr_output == wlr) {
			return out;
		}
	}
	return NULL;
}

/* 手势落在这块屏上：光标底下那块（没有就退回焦点屏）。 */
static struct vidow_output *output_under_cursor(struct vidow_server *server) {
	struct vidow_output *out = output_from_wlr(server,
		wlr_output_layout_output_at(server->output_layout,
			server->cursor->x, server->cursor->y));
	return out != NULL ? out : output_from_wlr(server, server->focused_output);
}

bool vidow_gesture_init(struct vidow_server *server) {
	wl_list_init(&server->gestures);
	return true;
}

void vidow_gesture_clear(struct vidow_server *server) {
	struct vidow_gesture *it, *tmp;
	wl_list_for_each_safe(it, tmp, &server->gestures, link) {
		wl_list_remove(&it->link);
		free(it);
	}
}

struct vidow_gesture *vidow_gesture_add(struct vidow_server *server,
		const struct vidow_gesture_opts *opts, const char *action,
		char *err, size_t errlen) {
	if (err != NULL && errlen > 0) {
		err[0] = '\0';
	}
	if (opts->fingers < 1 || opts->fingers > 5) {
		snprintf(err, errlen, "fingers 得是 1..5（触控板最多五根手指）");
		return NULL;
	}
	if (opts->kind != VIDOW_GESTURE_VIEW &&
			(action == NULL || action[0] == '\0')) {
		snprintf(err, errlen, "这条手势要一个 action（动作字符串，见 README）");
		return NULL;
	}
	struct vidow_gesture *it = calloc(1, sizeof(*it));
	if (it == NULL) {
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	it->fingers = opts->fingers;
	it->kind = opts->kind;
	it->axis = opts->axis;
	it->dir = opts->dir;
	it->speed = opts->speed > 0.0 ? opts->speed : VIDOW_GESTURE_SPEED;
	it->distance = opts->distance > 0.0 ? opts->distance
					    : VIDOW_GESTURE_DISTANCE;
	snprintf(it->action, sizeof(it->action), "%s",
		action != NULL ? action : "");
	switch (opts->kind) {
	case VIDOW_GESTURE_VIEW:
		snprintf(it->trigger, sizeof(it->trigger), "%d 指横扫（跟手拖视口）",
			opts->fingers);
		break;
	case VIDOW_GESTURE_SWIPE:
		snprintf(it->trigger, sizeof(it->trigger), "%d 指%s扫",
			opts->fingers,
			opts->axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? "横" : "竖");
		break;
	case VIDOW_GESTURE_PINCH:
		snprintf(it->trigger, sizeof(it->trigger), "%d 指捏合（%s）",
			opts->fingers, opts->dir > 0 ? "张开" : "收拢");
		break;
	default:
		snprintf(it->trigger, sizeof(it->trigger), "%d 指按住",
			opts->fingers);
		break;
	}
	wl_list_insert(server->gestures.prev, &it->link);
	wlr_log(WLR_INFO, "gesture: %s -> %s", it->trigger,
		it->action[0] != '\0' ? it->action : "(跟手拖视口)");
	return it;
}

static struct vidow_gesture *gesture_find(struct vidow_server *server,
		int fingers, enum vidow_gesture_kind kind,
		enum wl_pointer_axis axis, int dir) {
	struct vidow_gesture *it;
	wl_list_for_each(it, &server->gestures, link) {
		if (it->kind != kind || it->fingers != fingers) {
			continue;
		}
		if (kind == VIDOW_GESTURE_SWIPE && it->axis != axis) {
			continue;
		}
		if ((kind == VIDOW_GESTURE_SWIPE || kind == VIDOW_GESTURE_PINCH) &&
				it->dir != dir) {
			continue;
		}
		return it;
	}
	return NULL;
}

static void gesture_run(struct vidow_server *server,
		const struct vidow_gesture *it) {
	wlr_log(WLR_INFO, "手势：%s -> %s", it->trigger, it->action);
	vidow_action_run(server, it->action);
	const struct vidow_event_field fields[] = {
		VIDOW_EV_STR("gesture", it->trigger),
		VIDOW_EV_STR("action", it->action),
		VIDOW_EV_END,
	};
	vidow_ipc_emit_simple(server, "gesture", fields);
}

/* ------------------------------------------------- 连续：手指拖着条带走 */

static void view_drag_apply(struct vidow_server *server) {
	if (g.output == NULL) {
		return;
	}
	/* 视口 = 起手时的位置 + 手指拖出来的像素（clamp 在 arrange 的 Pass 2 里做：
	 * 条带的长度只有那儿知道）。 */
	g.output->gesture_viewport = g.base + g.moved;
	vidow_arrange(server);
}

static void snap_update(void *data, double t, double linear) {
	(void)linear;
	struct vidow_server *server = data;
	if (g.output == NULL) {
		return;
	}
	g.output->gesture_viewport = g.snap_from + (g.snap_to - g.snap_from) * t;
	vidow_arrange(server);
}

static void snap_done(void *data, bool cancelled) {
	(void)cancelled;
	struct vidow_server *server = data;
	g.snap = NULL;
	if (g.output != NULL) {
		/* 交还给「跟着焦点走」那套：下一帧 arrange 会用派生的视口。 */
		g.output->gesture_drag = false;
		g.output = NULL;
	}
	g.moved = 0.0;
	vidow_arrange(server);
}

/* 松手：拖得够远就吸附到相邻那条列（焦点换一条，视口跟着滑过去），不够就弹回原位。 */
static void view_snap(struct vidow_server *server, bool cancelled) {
	struct vidow_output *output = g.output;
	if (output == NULL) {
		return;
	}
	double from = output->gesture_viewport;
	if (!cancelled && fabs(g.moved) >= VIDOW_GESTURE_SNAP_PX) {
		/* 手指往左拖 = 看右边那条列（moved > 0）。 */
		vidow_focus_column(server, g.moved > 0 ? 1 : -1);
	}
	/* 目标 = 「不做手势时该在的位置」：先把 gesture_drag 摘掉 arrange 一次问出来，
	 * 再挂回去从手指拖到的位置滑过去。 */
	output->gesture_drag = false;
	vidow_arrange(server);
	double to = output->viewport_x;
	output->gesture_drag = true;
	output->gesture_viewport = from;
	g.snap_from = from;
	g.snap_to = to;
	g.snap = vidow_anim_start(server, "gesture-snap", 160,
		server->config.easing, snap_update, snap_done, server);
	if (g.snap == NULL) {
		snap_done(server, false);
	}
}

/* ------------------------------------------------------------- 事件入口 */

void vidow_gesture_swipe_begin(struct vidow_server *server, int fingers) {
	/* 会话锁（S3）：锁定期间手势不参与 —— 它操作的是被藏起来的窗口 / 视口
	 * （方案 §2 环节 3）。begin / update 挡在这里；end 照旧放行，好让锁定
	 * 之前就开着的手势能自己收尾。 */
	if (server->locked) {
		return;
	}
	/* 上一个手势还没收尾（设备抽风 / 注入没配对）：先收干净。 */
	if (g.active) {
		vidow_gesture_swipe_end(server, true);
	}
	g = (struct gesture_state){ .active = true, .kind = VIDOW_GESTURE_SWIPE,
		.fingers = fingers, .server = server };

	struct vidow_gesture *view = gesture_find(server, fingers,
		VIDOW_GESTURE_VIEW, WL_POINTER_AXIS_HORIZONTAL_SCROLL, 0);
	if (view == NULL) {
		return;
	}
	struct vidow_output *output = output_under_cursor(server);
	if (output == NULL) {
		return;
	}
	g.output = output;
	g.base = output->viewport_x;
	g.moved = 0.0;
	g.speed = view->speed;
	output->gesture_drag = true;
	output->gesture_viewport = g.base;
}

void vidow_gesture_swipe_update(struct vidow_server *server, int fingers,
		double dx, double dy) {
	/* 会话锁（S3）：锁定期间手势不参与 —— 它操作的是被藏起来的窗口 / 视口
	 * （方案 §2 环节 3）。begin / update 挡在这里；end 照旧放行，好让锁定
	 * 之前就开着的手势能自己收尾。 */
	if (server->locked) {
		return;
	}
	if (!g.active || g.kind != VIDOW_GESTURE_SWIPE || fingers != g.fingers) {
		return;
	}
	g.acc_x += dx;
	g.acc_y += dy;
	if (g.output == NULL) {
		return;
	}
	/* 手指往左划（dx < 0）= 把条带往左拖 = 看右边那条列：视口变大。 */
	g.moved = -g.acc_x * g.speed;
	view_drag_apply(server);
}

void vidow_gesture_swipe_end(struct vidow_server *server, bool cancelled) {
	if (!g.active || g.kind != VIDOW_GESTURE_SWIPE) {
		return;
	}
	bool dragging = g.output != NULL;
	/* 离散方向：整个手势里主方向对不对得上（横向 / 纵向分开比）。 */
	double ax = fabs(g.acc_x), ay = fabs(g.acc_y);
	if (!dragging && !cancelled) {
		enum wl_pointer_axis axis = ax >= ay
			? WL_POINTER_AXIS_HORIZONTAL_SCROLL
			: WL_POINTER_AXIS_VERTICAL_SCROLL;
		double amount = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? g.acc_x
									  : g.acc_y;
		int dir = amount > 0 ? 1 : -1;
		struct vidow_gesture *it =
			gesture_find(server, g.fingers, VIDOW_GESTURE_SWIPE, axis,
				dir);
		if (it != NULL && fabs(amount) >= it->distance) {
			gesture_run(server, it);
		}
	}
	if (dragging) {
		view_snap(server, cancelled);
	}
	g.active = false;
}

void vidow_gesture_pinch_begin(struct vidow_server *server, int fingers) {
	/* 会话锁（S3）：锁定期间手势不参与 —— 它操作的是被藏起来的窗口 / 视口
	 * （方案 §2 环节 3）。begin / update 挡在这里；end 照旧放行，好让锁定
	 * 之前就开着的手势能自己收尾。 */
	if (server->locked) {
		return;
	}
	if (g.active) {
		vidow_gesture_pinch_end(server, true);
	}
	g = (struct gesture_state){ .active = true, .kind = VIDOW_GESTURE_PINCH,
		.fingers = fingers, .scale = 1.0, .server = server };
}

void vidow_gesture_pinch_update(struct vidow_server *server, int fingers,
		double scale) {
	/* 会话锁（S3）：锁定期间手势不参与 —— 它操作的是被藏起来的窗口 / 视口
	 * （方案 §2 环节 3）。begin / update 挡在这里；end 照旧放行，好让锁定
	 * 之前就开着的手势能自己收尾。 */
	if (server->locked) {
		return;
	}
	(void)server;
	if (!g.active || g.kind != VIDOW_GESTURE_PINCH || fingers != g.fingers) {
		return;
	}
	g.scale = scale;
}

void vidow_gesture_pinch_end(struct vidow_server *server, bool cancelled) {
	if (!g.active || g.kind != VIDOW_GESTURE_PINCH) {
		return;
	}
	if (!cancelled && fabs(g.scale - 1.0) >= 0.1) {
		int dir = g.scale > 1.0 ? 1 : -1;
		struct vidow_gesture *it = gesture_find(server, g.fingers,
			VIDOW_GESTURE_PINCH, WL_POINTER_AXIS_VERTICAL_SCROLL, dir);
		if (it != NULL) {
			gesture_run(server, it);
		}
	}
	g.active = false;
}

void vidow_gesture_hold_begin(struct vidow_server *server, int fingers) {
	/* 会话锁（S3）：锁定期间手势不参与 —— 它操作的是被藏起来的窗口 / 视口
	 * （方案 §2 环节 3）。begin / update 挡在这里；end 照旧放行，好让锁定
	 * 之前就开着的手势能自己收尾。 */
	if (server->locked) {
		return;
	}
	if (g.active) {
		vidow_gesture_hold_end(server, true);
	}
	g = (struct gesture_state){ .active = true, .kind = VIDOW_GESTURE_HOLD,
		.fingers = fingers, .server = server };
	struct vidow_gesture *it = gesture_find(server, fingers,
		VIDOW_GESTURE_HOLD, WL_POINTER_AXIS_VERTICAL_SCROLL, 0);
	if (it != NULL) {
		gesture_run(server, it);
	}
}

void vidow_gesture_hold_end(struct vidow_server *server, bool cancelled) {
	(void)server;
	(void)cancelled;
	if (!g.active || g.kind != VIDOW_GESTURE_HOLD) {
		return;
	}
	g.active = false;
}

void vidow_gesture_finish(struct vidow_server *server) {
	if (g.snap != NULL) {
		vidow_anim_cancel(g.snap);
		g.snap = NULL;
	}
	if (g.output != NULL) {
		g.output->gesture_drag = false;
	}
	g = (struct gesture_state){ .server = server };
}
