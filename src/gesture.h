/* 触控板手势：三指横扫、四指竖扫、捏合、按住。
 *
 *   vidow.gesture{ fingers = 3, swipe = "view" }                    -- 连续：跟手拖视口
 *   vidow.gesture{ fingers = 4, direction = "up",   action = "workspace up" }
 *   vidow.gesture{ fingers = 4, direction = "down", action = "workspace down" }
 *   vidow.gesture{ fingers = 2, pinch = "out",      action = "cycle-column-width" }
 *   vidow.gesture{ fingers = 2, pinch = "in",       action = "cycle-column-width" }
 *   vidow.gesture{ fingers = 3, hold = true,        action = "toggle-overview" }
 *
 * 和键位同一个规矩：**合成器一个手势都不内置**，默认配置里给几条，删掉就没反应。
 * 动作字符串也是同一套（vidow_action_run）。
 *
 * 三种语义：
 *   - `swipe = "view"`（连续）：手指拖着条带走，视口跟着动；松手按拖了多远决定
 *     「吸附到相邻那条列」还是「弹回原位」（见 gesture.c 的 snap）。
 *   - `direction = …`（离散）：整个手势结束后，看累计位移的主方向对不对得上，
 *     对得上就触发那条动作（阈值 distance）。
 *   - `pinch` / `hold`：捏合看 scale，按住是 libinput 的 hold（手指按住不动）。
 *
 * 单位：libinput 的手势位移是**归一化到 1000dpi 的**（libinput 文档：Relative
 * motion deltas are normalized to represent those of a device with 1000dpi
 * resolution），所以 1 单位 ≈ 0.025mm；手指划 1cm ≈ 394 单位。`speed` 是
 * 「1 单位 → 多少逻辑像素」，默认 0.3（手指 1cm ≈ 屏幕 118px）。 */
#ifndef VIDOW_GESTURE_H
#define VIDOW_GESTURE_H

#include <stdbool.h>
#include <stddef.h>

#include <wayland-client-protocol.h> /* enum wl_pointer_axis */
#include <wayland-util.h>

struct vidow_server;

enum vidow_gesture_kind {
	VIDOW_GESTURE_VIEW = 0, /* 连续拖视口（水平） */
	VIDOW_GESTURE_SWIPE,    /* 离散：松手按主方向触发 */
	VIDOW_GESTURE_PINCH,    /* 离散：捏合方向触发 */
	VIDOW_GESTURE_HOLD,     /* 按住不动 */
};

/* 手指位移 → 逻辑像素的默认系数（见文件头）。 */
#define VIDOW_GESTURE_SPEED 0.3
/* 离散方向手势：累计位移（归一化单位）超过这么多才算数（≈ 0.8cm）。 */
#define VIDOW_GESTURE_DISTANCE 300.0
/* 连续手势松手：拖过这么多逻辑像素就吸附到相邻那条列。 */
#define VIDOW_GESTURE_SNAP_PX 100.0

struct vidow_gesture {
	struct wl_list link; /* vidow_server.gestures */
	char trigger[64];    /* 原样记着（日志 / state 里看） */
	char action[128];    /* VIEW 那种没有动作（空串） */
	int fingers;
	enum vidow_gesture_kind kind;
	enum wl_pointer_axis axis; /* SWIPE：纵向 / 横向 */
	int dir;                   /* SWIPE / PINCH：-1 / 1 */
	double speed;              /* VIEW */
	double distance;           /* SWIPE */
};

/* 一条手势的解析结果（script.c 从 Lua 表里按类型取出来）。 */
struct vidow_gesture_opts {
	int fingers;
	enum vidow_gesture_kind kind;
	enum wl_pointer_axis axis;
	int dir;
	double speed;    /* <= 0 = 用默认 */
	double distance; /* <= 0 = 用默认 */
};

bool vidow_gesture_init(struct vidow_server *server);

/* 加一条手势。失败时把原因写进 err，返回 NULL。 */
struct vidow_gesture *vidow_gesture_add(struct vidow_server *server,
		const struct vidow_gesture_opts *opts, const char *action,
		char *err, size_t errlen);

/* 清空全部手势（config.lua 重载时用）。 */
void vidow_gesture_clear(struct vidow_server *server);

/* 事件入口：input.c 的 libinput 手势监听调，IPC 注入（vidowmsg gesture）走同一条路。 */
void vidow_gesture_swipe_begin(struct vidow_server *server, int fingers);
void vidow_gesture_swipe_update(struct vidow_server *server, int fingers,
		double dx, double dy);
void vidow_gesture_swipe_end(struct vidow_server *server, bool cancelled);
void vidow_gesture_pinch_begin(struct vidow_server *server, int fingers);
void vidow_gesture_pinch_update(struct vidow_server *server, int fingers,
		double scale);
void vidow_gesture_pinch_end(struct vidow_server *server, bool cancelled);
void vidow_gesture_hold_begin(struct vidow_server *server, int fingers);
void vidow_gesture_hold_end(struct vidow_server *server, bool cancelled);

/* 关掉合成器时把「手指正拖着条带」这个状态清干净（不然 arrange 一直绕过
 * 「把焦点列滚回可见区」那条）。 */
void vidow_gesture_finish(struct vidow_server *server);

#endif
