#!/usr/bin/env python3
"""背景层：一条**从全透明到全不透明**的横向 alpha 斜坡。

用来量「透明度剔除」：模糊核碰到 alpha 低于阈值的像素时该怎么处理。
硬阈值会在阈值线上留下一道突变的边（模糊结果里出现一刀），
按采纳权重归一化则是平滑过渡、而且斜坡本身原样保留。

用法：python3 tools/ramp.py [上边缘的 alpha 渐变宽度]
"""
import sys

import gi

gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
import cairo  # noqa: E402
from gi.repository import Gtk, GtkLayerShell  # noqa: E402

SOFT = int(sys.argv[1]) if len(sys.argv) > 1 else 0


class Ramp(Gtk.Window):
    def __init__(self):
        super().__init__()
        visual = self.get_screen().get_rgba_visual()
        if visual is not None:
            self.set_visual(visual)
        self.set_app_paintable(True)
        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.BACKGROUND)
        GtkLayerShell.set_namespace(self, 'ramp')
        for edge in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.BOTTOM,
                     GtkLayerShell.Edge.LEFT, GtkLayerShell.Edge.RIGHT):
            GtkLayerShell.set_anchor(self, edge, True)

        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        w = widget.get_allocated_width()
        h = widget.get_allocated_height()
        grad = cairo.LinearGradient(0, 0, w, 0)
        grad.add_color_stop_rgba(0.0, 0.8, 0.8, 0.8, 0.0)
        grad.add_color_stop_rgba(1.0, 0.8, 0.8, 0.8, 1.0)
        cr.set_source(grad)
        cr.paint()
        return False


Ramp().show_all()
Gtk.main()
