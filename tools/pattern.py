#!/usr/bin/env python3
"""四象限测试窗口：左上红、右上绿、左下蓝、右下白。
用来验证自定义 shader 管线的方向与颜色。"""
import sys
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk  # noqa: E402

W, H = 400, 300


class Pattern(Gtk.Window):
    def __init__(self):
        super().__init__(title=sys.argv[1] if len(sys.argv) > 1 else 'pattern')
        self.set_default_size(W, H)
        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        w = widget.get_allocated_width()
        h = widget.get_allocated_height()
        cr.set_source_rgb(1, 0, 0); cr.rectangle(0, 0, w / 2, h / 2); cr.fill()
        cr.set_source_rgb(0, 1, 0); cr.rectangle(w / 2, 0, w / 2, h / 2); cr.fill()
        cr.set_source_rgb(0, 0, 1); cr.rectangle(0, h / 2, w / 2, h / 2); cr.fill()
        cr.set_source_rgb(1, 1, 1); cr.rectangle(w / 2, h / 2, w / 2, h / 2); cr.fill()
        return False


win = Pattern()
win.show_all()
Gtk.main()
