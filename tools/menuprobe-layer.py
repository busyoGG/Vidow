#!/usr/bin/env python3
# layer surface 版的二级菜单探针：一块状态栏（layer shell，top 层、贴顶、满宽）+
# 一级菜单（第一项带子菜单）+ 二级子菜单。给 test-popup-layer.sh 当靶子 —— 那个
# 测试量的是「挂在 layer surface 上的弹出物再弹二级时，子菜单相对父菜单摆在哪儿」。
#
# 跟 menuprobe2.py 的差别只在「一级弹出物的父面是 layer 表面，不是窗口」：窗口那条
# 路 7768fe7 修过了，layer 这条路当时没修。
#
# 用法: WAYLAND_DISPLAY=wayland-1 python3 tools/menuprobe-layer.py
# 输出: 每步一行（状态栏上屏 / 一级菜单上屏 / 二级菜单上屏），直到被 kill
import gi

gi.require_version("Gtk", "3.0")
gi.require_version("GtkLayerShell", "0.1")
from gi.repository import Gtk, Gdk, GtkLayerShell  # noqa: E402

BAR_H = 32        # 状态栏高度：菜单从栏下面弹出来
MENU_ITEMS = 8    # 一级菜单：够大，像素/几何断言才看得出多了一块东西
SUB_ITEMS = 2     # 二级菜单：比一级明显小，测试靠尺寸把两个弹出物分开


def log(msg):
    print("menuprobe-layer: %s" % msg, flush=True)


class Bar(Gtk.Window):
    def __init__(self):
        super().__init__()
        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_namespace(self, "menuprobe-layer")
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.TOP)
        GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.TOP, True)
        GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.LEFT, True)
        GtkLayerShell.set_anchor(self, GtkLayerShell.Edge.RIGHT, True)
        GtkLayerShell.set_exclusive_zone(self, BAR_H)
        self.set_size_request(0, BAR_H)

        area = Gtk.DrawingArea()
        area.connect("draw", self.on_draw)
        self.add(area)
        self.connect("destroy", Gtk.main_quit)

    def on_draw(self, widget, cr):
        cr.set_source_rgb(0.15, 0.15, 0.2)
        cr.paint()
        return False


def main():
    bar = Bar()

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

    def on_press(w, ev):
        log("弹一级菜单（xdg_popup，父面是状态栏这块 layer 表面）")
        menu.show_all()
        menu.popup_at_pointer(ev)
        return True

    bar.connect("button-press-event", on_press)
    bar.connect("map-event", lambda *_: log("状态栏上屏"))
    menu.connect("map-event", lambda *_: log("一级菜单上屏"))
    sub.connect("map-event", lambda *_: log("二级菜单上屏"))
    bar.show_all()
    log("进入主循环")
    Gtk.main()


if __name__ == "__main__":
    main()
