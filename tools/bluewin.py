#!/usr/bin/env python3
"""一整个纯蓝的普通窗口。

用来盯「layer 的模糊里有没有窗口」：蓝得扎眼，壁纸是灰的（tools/dot.py），于是
面板底下那块底衬里蓝占多少，一眼能量出来。配 tools/panel.py（top 层 50% 面板）
一起用：面板压在窗口上，它背后该是**窗口的模糊**（偏蓝），不是壁纸的模糊（灰）。

用法：python3 tools/bluewin.py
"""
import gi

gi.require_version('Gtk', '3.0')
from gi.repository import Gtk  # noqa: E402


class Blue(Gtk.Window):
    def __init__(self):
        super().__init__(title='bluewin')
        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        cr.set_source_rgb(0.0, 0.0, 1.0)
        cr.paint()
        return False


Blue().show_all()
Gtk.main()
