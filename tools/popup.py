#!/usr/bin/env python3
# 最小 Wayland xdg_popup 客户端：一个窗口 + 一个菜单。
#
# 用途：wlr_xdg_popup_unconstrain_from_box() 只能在 popup **自己第一次 commit
# 之后**调 —— 它内部会 wlr_xdg_surface_schedule_configure()，而那里面有
# assert(surface->initialized)。在 new_popup 里直接调就是 SIGABRT
# （noctalia 状态栏的提示一弹就把合成器打死）。这个客户端专门用来撞这条路：
# 跑起来 1 秒后弹一个 Gtk.Menu（GTK3 在 Wayland 上用 xdg_popup 实现菜单），
# 合成器要是还在 new_popup 里 unconstrain，这里就能稳定复现 abort。
#
# 用法: WAYLAND_DISPLAY=wayland-1 python3 tools/popup.py [存活秒数]
# 输出: 每步一行（窗口上屏 / 菜单弹出），退出码 0
import sys
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk, GLib  # noqa: E402

ALIVE = float(sys.argv[1]) if len(sys.argv) > 1 else 4.0


def log(msg):
    print(f"popup: {msg}", flush=True)


def popup_menu(win, menu, event=None):
    log("弹菜单（xdg_popup）")
    menu.show_all()
    # GTK 3.22 起 popup_at_* 要一个「触发事件」，传 None 会只留一句
    # 「no trigger event for menu popup」然后把菜单吞掉（菜单根本不出）。
    # 没有真事件（定时器那条路）就自己合成一个 BUTTON_PRESS。
    if event is None:
        event = Gdk.Event.new(Gdk.EventType.BUTTON_PRESS)
        event.window = win.get_window()
        event.x = 10.0
        event.y = 10.0
        event.button = 1
    menu.popup_at_pointer(event)


def main():
    win = Gtk.Window(title="popup-test")
    win.set_default_size(300, 200)
    box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
    win.add(box)

    menu = Gtk.Menu()
    item = Gtk.MenuItem(label="一个菜单项")
    item.connect("activate", lambda *_: log("点了菜单项"))
    menu.append(item)
    menu.append(Gtk.SeparatorMenuItem())
    menu.append(Gtk.MenuItem(label="另一个菜单项"))

    button = Gtk.Button(label="点我弹菜单")
    button.connect("clicked", lambda *_: popup_menu(win, menu))
    # 真事件那条路：窗口里按下就弹（trigger event 现成的）
    win.connect("button-press-event",
                lambda w, ev: (popup_menu(w, menu, ev), True)[1])
    box.pack_start(button, False, False, 0)

    win.connect("map-event", lambda *_: log("窗口上屏"))
    win.connect("destroy", lambda *_: Gtk.main_quit())
    win.show_all()

    GLib.timeout_add(1000, lambda: (popup_menu(win, menu), False)[1])
    GLib.timeout_add(int(ALIVE * 1000), lambda: (log("结束"), Gtk.main_quit(), False)[2])
    log("进入主循环")
    Gtk.main()
    log("退出")


if __name__ == "__main__":
    main()
