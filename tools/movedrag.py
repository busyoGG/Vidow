#!/usr/bin/env python3
# 客户端发起的窗口拖拽探针（GTK3 / Wayland）。
#
# 干什么：造一个 320x240、**自己定尺寸**（min == max，于是 vidow 一上来就把它当
# 浮窗）的窗口，顶部 30 像素当成「客户端自己画的标题栏」—— 在那条带里按下就调
# gdk_window_begin_move_drag()，它在 Wayland 上就是 xdg_toplevel.move。这正是
# GTK / Chromium（微信这类 CEF 壳）自定义标题栏拖拽走的那条路，也是 vidow 的
# view_request_move 唯一的上游。
#
# 每条收到的指针事件打一行（probe: 时间 事件 ...），给 test-titlebar-drag.sh 当
# 判据用：
#   press / release = 客户端手里的按键状态。**松手那条路必须成对出现**：客户端
#   发起拖拽时「按下」是转给它的，合成器要是只吃掉松手，它就一直以为左键按着。
#
# 用法: WAYLAND_DISPLAY=wayland-1 python3 tools/movedrag.py
# 输出: 每行一条事件，直到被 kill
import time

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk  # noqa: E402

TITLE_H = 30
WIN_W = 320
WIN_H = 240

T0 = time.time()
# GTK 会把同一个按下事件发两遍（窗口自己 + 窗口部件的默认处理），而真实客户端
# 一次手势只发一次 move 请求 —— 去重，免得同一次拖拽被重新锚定两次（第二次锚定
# 会把「抓点」改成当时的指针位置，窗口就少跟一段）。
DRAGGING = [False]


def log(msg):
    print("probe: %8.3f %s" % (time.time() - T0, msg), flush=True)


def on_press(w, ev):
    log("press button=%d at %.0f,%.0f" % (ev.button, ev.x, ev.y))
    if ev.y < TITLE_H and not DRAGGING[0]:
        DRAGGING[0] = True
        w.get_window().begin_move_drag(ev.button, ev.x, ev.y, ev.time)
        log("begin_move_drag 已发")


def on_release(w, ev):
    DRAGGING[0] = False
    log("release button=%d at %.0f,%.0f" % (ev.button, ev.x, ev.y))


def on_motion(w, ev):
    log("motion at %.0f,%.0f state=%d" % (ev.x, ev.y, ev.state))


def main():
    win = Gtk.Window(title="movedrag")
    win.add_events(Gdk.EventMask.POINTER_MOTION_MASK |
                   Gdk.EventMask.BUTTON_PRESS_MASK |
                   Gdk.EventMask.BUTTON_RELEASE_MASK)
    win.set_default_size(WIN_W, WIN_H)
    # 固定尺寸 = 客户端自己定大小：vidow 会直接把它当浮窗（不用 IPC 切浮动）
    win.set_resizable(False)
    win.set_decorated(False)
    win.connect("button-press-event", on_press)
    win.connect("button-release-event", on_release)
    win.connect("motion-notify-event", on_motion)
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    log("窗口上屏")
    Gtk.main()


if __name__ == "__main__":
    main()
