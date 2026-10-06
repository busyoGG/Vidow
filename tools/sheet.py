#!/usr/bin/env python3
"""全窗纯色薄纱 + 白网格的定尺寸浮动窗口（半透明客户端）。

给测试当靶子：透明窗口背后的模糊底衬里**不许**混进它自己上一帧的形变图
（混进去屏幕上就是错位的竖线 / 重影，见 jelly.c / fx.c 的抓底衬）。

用法: python3 tools/sheet.py <标题> <r> <g> <b> <alpha>   （颜色/alpha 都是 0..1）"""
import sys
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk

TITLE = sys.argv[1]
R, G, B, A = (float(v) for v in sys.argv[2:6])
W, H = 400, 300


class Sheet(Gtk.Window):
    def __init__(self):
        super().__init__(title=TITLE)
        self.set_default_size(W, H)
        self.set_resizable(False)
        self.set_app_paintable(True)
        vis = self.get_screen().get_rgba_visual()
        if vis is not None:
            self.set_visual(vis)
        self.connect('draw', self.on_draw)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        cr.set_operator(2)  # CAIRO_OPERATOR_OVER
        cr.set_source_rgba(0, 0, 0, 0)
        cr.paint()
        cr.set_source_rgba(R, G, B, A)
        cr.paint()
        cr.set_source_rgba(1, 1, 1, 1)
        cr.set_line_width(1)
        for x in range(20, W, 40):
            cr.move_to(x + 0.5, 0); cr.line_to(x + 0.5, H)
        for y in range(20, H, 40):
            cr.move_to(0, y + 0.5); cr.line_to(W, y + 0.5)
        cr.stroke()
        return False


w = Sheet()
w.show_all()
Gtk.main()
