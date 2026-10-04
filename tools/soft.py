#!/usr/bin/env python3
"""背景层一整屏**均匀半透明**的灰（默认 alpha 0.45）。

底衬里没有别的东西时，这块就是底衬本身：dump 出来的 alpha 直接等于这里的 alpha。
用来盯「透明度剔除」有没有在模糊链条里逐趟相乘 —— 那种情况下 alpha 会一路掉到
接近 0（实测 0.45 -> 0.0246），而不是老老实实留在 0.45。

用法：python3 tools/soft.py [alpha]
"""
import sys

import gi

gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
from gi.repository import Gtk, GtkLayerShell  # noqa: E402

ALPHA = float(sys.argv[1]) if len(sys.argv) > 1 else 0.45


class Soft(Gtk.Window):
    def __init__(self):
        super().__init__()
        visual = self.get_screen().get_rgba_visual()
        if visual is not None:
            self.set_visual(visual)
        self.set_app_paintable(True)
        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.BACKGROUND)
        GtkLayerShell.set_namespace(self, 'soft')
        for edge in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.BOTTOM,
                     GtkLayerShell.Edge.LEFT, GtkLayerShell.Edge.RIGHT):
            GtkLayerShell.set_anchor(self, edge, True)

        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        cr.set_source_rgba(0.6, 0.6, 0.6, ALPHA)
        cr.paint()
        return False


Soft().show_all()
Gtk.main()
