#!/usr/bin/env python3
# 最小 xdg_popup 探针：一个窗口，按一下弹一个菜单（GTK3 在 Wayland 上用 xdg_popup
# 实现菜单）。给 test-popup2out.sh 当靶子 —— 那个测试量的是「弹出物被摆到哪儿」。
#
# 用法: WAYLAND_DISPLAY=wayland-1 python3 tools/menuprobe.py
# 输出: 每步一行（窗口上屏 / 弹菜单），直到被 kill
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk, Gdk  # noqa: E402

MENU_ITEMS = 6  # 菜单要够大：像素断言得看得出「屏幕上多了一块东西」


def log(msg):
    print("menuprobe: %s" % msg, flush=True)


def popup_menu(win, menu, event):
    log("弹菜单（xdg_popup）")
    menu.show_all()
    menu.popup_at_pointer(event)


def main():
    win = Gtk.Window(title="menuprobe")
    win.set_default_size(400, 300)
    win.set_decorated(False)

    menu = Gtk.Menu()
    for i in range(MENU_ITEMS):
        item = Gtk.MenuItem(label="菜单项 %d" % (i + 1))
        item.connect("activate", lambda *_: log("点了菜单项"))
        menu.append(item)

    win.connect("button-press-event",
                lambda w, ev: (popup_menu(w, menu, ev), True)[1])
    win.connect("map-event", lambda *_: log("窗口上屏"))
    win.connect("destroy", lambda *_: Gtk.main_quit())
    win.show_all()
    log("进入主循环")
    Gtk.main()


if __name__ == "__main__":
    main()
