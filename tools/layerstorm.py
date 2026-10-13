#!/usr/bin/env python3
"""每块屏上摆一套 layer 面（学 noctalia：壁纸 + 状态栏 + 停靠栏 + 四个角），
每个输出一份 —— test-layerstorm.sh 的靶子。

面一起上屏时每一块都有自己的入场动画，vidow 会连着几百帧重排布局；这个脚本的
用途就是把「同时有 N 块面在动」这个局面摆出来。

用法：python3 tools/layerstorm.py [每屏块数] [存活秒数]
  每屏块数  默认 7（1 壁纸 + 1 状态栏 + 1 停靠栏 + 4 个角），第 7 块之后是
            叠在角上的小块，只为把面数堆上去
  存活秒数  默认 0 = 一直跑；到点自己退出
上屏之后往 stdout 打一行 `layerstorm: <n> 面已上屏`（测试用它当同步点）。"""
import sys

import gi

gi.require_version('Gtk', '3.0')
gi.require_version('Gdk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
from gi.repository import Gdk, GLib, Gtk, GtkLayerShell  # noqa: E402

PER = int(sys.argv[1]) if len(sys.argv) > 1 else 7
LIFETIME = float(sys.argv[2]) if len(sys.argv) > 2 else 0.0

L = GtkLayerShell.Layer
E = GtkLayerShell.Edge
CORNERS = ((E.LEFT, E.TOP), (E.RIGHT, E.TOP), (E.LEFT, E.BOTTOM), (E.RIGHT, E.BOTTOM))

wins = []


def build(monitor, out_index, i):
    win = Gtk.Window()
    GtkLayerShell.init_for_window(win)
    GtkLayerShell.set_monitor(win, monitor)
    GtkLayerShell.set_namespace(win, 'storm%d-%d' % (out_index, i))
    if i == 0:                       # 壁纸：铺满，不吃独占区
        GtkLayerShell.set_layer(win, L.BACKGROUND)
        for edge in (E.LEFT, E.RIGHT, E.TOP, E.BOTTOM):
            GtkLayerShell.set_anchor(win, edge, True)
    elif i == 1:                     # 状态栏：贴顶，独占 30
        GtkLayerShell.set_layer(win, L.TOP)
        for edge in (E.LEFT, E.RIGHT, E.TOP):
            GtkLayerShell.set_anchor(win, edge, True)
        GtkLayerShell.set_exclusive_zone(win, 30)
        win.set_size_request(-1, 30)
    elif i == 2:                     # 停靠栏：贴底，独占 60
        GtkLayerShell.set_layer(win, L.TOP)
        for edge in (E.LEFT, E.RIGHT, E.BOTTOM):
            GtkLayerShell.set_anchor(win, edge, True)
        GtkLayerShell.set_exclusive_zone(win, 60)
        win.set_size_request(-1, 60)
    else:                            # 四个角（再多就叠着摆）
        GtkLayerShell.set_layer(win, L.OVERLAY)
        for edge in CORNERS[(i - 3) % 4]:
            GtkLayerShell.set_anchor(win, edge, True)
        win.set_size_request(40, 40)
    win.set_app_paintable(True)
    win.connect('destroy', Gtk.main_quit)
    win.show_all()
    wins.append(win)


def main():
    display = Gdk.Display.get_default()
    for out_index in range(display.get_n_monitors()):
        monitor = display.get_monitor(out_index)
        for i in range(PER):
            build(monitor, out_index, i)

    def ready():
        print('layerstorm: %d 面已上屏' % len(wins), flush=True)
        return False

    GLib.timeout_add(300, ready)
    if LIFETIME > 0:
        GLib.timeout_add(int(LIFETIME * 1000), Gtk.main_quit)
    Gtk.main()


main()
