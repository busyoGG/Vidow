/*
 * 独立于 wlroots 的 X11 观察层 —— 声明。
 *
 * 为什么需要它，见 src/x11.c 文件头。
 */

#ifndef VIDOW_X11_H
#define VIDOW_X11_H

#include <stdbool.h>
#include <xcb/xcb.h>

struct vidow_server;

/* 启动 / 停止只读观察。XWayland ready 之后调 start，退出时调 stop。 */
void x11_observe_start(struct vidow_server *server);
void x11_observe_stop(void);

/* 这个 X 窗口在服务器眼里是不是可见的。
 *
 * 和 wlr_xwayland_surface.mapped 的区别：后者只反映「我们收到过
 * MapRequest」，一个已经映射过的窗口被重新 map 时 X 只发 MapNotify，
 * 那个字段就永远停在 0。这里问的是 X 服务器本人。 */
bool x11_observe_is_viewable(xcb_window_t window);

/* 打一行事件计数（诊断用）。 */
void x11_observe_report(void);

#endif /* VIDOW_X11_H */
