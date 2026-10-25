#ifndef VIDOW_XWM_H
#define VIDOW_XWM_H

#include <stdbool.h>

struct vidow_view;

/* X11 表面的「该不该在屏幕上」判定。
 *
 * 为什么单独抽一层：以前 vidow 对 X11 窗口能不能上屏的判断散在三个地方 ——
 * x11_map_request 看一次 view->mapped，x11_associate 查一次 X 侧的
 * IsViewable，view_commit 里再 replay 一次。三处各有各的前置条件，条件之间
 * 还会互相打架：Steam 重开主窗口时三者全都说「没问题」，窗口在 `vidowmsg`
 * 里 mapped / visible / placed 也全对，屏幕上却什么都没有。
 *
 * Hyprland 只有一个判据（src/xwayland/XSurface.cpp:97 的 commit 监听和
 * :158 的 considerMap）：**表面有 texture 就该在屏幕上，没有就不该**。
 * 不看 X 的 map 状态、不看我们自己的标志位、不看事件顺序。texture 是
 * 客户端真的交过一帧的证据，也是我们能画出来的唯一前提。
 *
 * 这里就照这个模型重写：所有 X11 的上屏判定都走 xwm_surface_sync()，
 * 别处不再自己判。 */
enum xwm_sync_result {
	XWM_SYNC_NOOP = 0, /* 状态没变，什么都没做 */
	XWM_SYNC_MAPPED,   /* 本次把窗口放上了屏 */
	XWM_SYNC_UNMAPPED, /* 本次把窗口收了下来 */
};

/* 按「有没有 texture」重新对齐一次窗口的显示状态，需要切就切。
 *
 * 幂等：状态已经对得上时什么都不做，可以随便重复调用（commit 每帧都会走）。
 * 非 X11 的 view 直接什么都不做。 */
enum xwm_sync_result xwm_surface_sync(struct vidow_view *view);

/* 只要判定结果，不动状态。诊断日志用。 */
bool xwm_surface_should_be_mapped(const struct vidow_view *view);

/* 这个 view 的 X11 状态机是否已经可以判定（surface 建好了）。
 * 表面还没 associate 的时候判不了，调用方应当等 associate 之后再来。 */
bool xwm_surface_ready(const struct vidow_view *view);

#endif /* VIDOW_XWM_H */
