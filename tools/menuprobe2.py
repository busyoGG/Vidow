#!/usr/bin/env python3
# 二级菜单探针：一个窗口，按一下弹一个菜单，菜单的第一项**带子菜单**（GTK3 在
# Wayland 上用嵌套的 xdg_popup 实现它）。给 test-popup-nested.sh 当靶子 —— 那个
# 测试量的是「嵌套弹出物相对父弹出物摆在哪儿」。
#
# 跟 menuprobe.py（单层）的差别只在「有子菜单」这一件事上：一级弹出物挂在顶层
# 窗口上，二级弹出物的父面是**一级弹出物**，两者走的坐标空间不一样。
#
# 用法: WAYLAND_DISPLAY=wayland-1 python3 tools/menuprobe2.py
# 输出: 每步一行（窗口上屏 / 弹一级菜单 / 二级菜单上屏），直到被 kill
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import Gtk  # noqa: E402

MENU_ITEMS = 8    # 一级菜单：够大，像素/几何断言才看得出多了一块东西
SUB_ITEMS = 2     # 二级菜单：比一级明显小，测试靠尺寸把两个弹出物分开


def log(msg):
    print("menuprobe2: %s" % msg, flush=True)


def popup_menu(win, menu, event):
    log("弹一级菜单（xdg_popup）")
    menu.show_all()
    menu.popup_at_pointer(event)


def main():
    win = Gtk.Window(title="menuprobe2")
    win.set_default_size(400, 300)
    win.set_decorated(False)

    sub = Gtk.Menu()
    for i in range(SUB_ITEMS):
        sub.append(Gtk.MenuItem(label="二%d" % (i + 1)))

    menu = Gtk.Menu()
    # 子菜单挂在第一项上：菜单是按指针位置弹的，指针正好落在这项上，不用再
    # 自己算「第几项在哪儿」去挪指针（GTK 会自己起延时把子菜单弹出来）。
    first = Gtk.MenuItem(label="一级菜单项 1（带子菜单）")
    first.set_submenu(sub)
    menu.append(first)
    for i in range(1, MENU_ITEMS):
        menu.append(Gtk.MenuItem(label="一级菜单项 %d" % (i + 1)))

    win.connect("button-press-event",
                lambda w, ev: (popup_menu(w, menu, ev), True)[1])
    win.connect("map-event", lambda *_: log("窗口上屏"))
    sub.connect("map-event", lambda *_: log("二级菜单上屏"))
    win.connect("destroy", lambda *_: Gtk.main_quit())
    win.show_all()
    log("进入主循环")
    Gtk.main()


if __name__ == "__main__":
    main()
