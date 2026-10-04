#!/usr/bin/env python3
"""右下角一块 50% 透明的 top 层面板（不是贴着 (0,0) 的，专治对位问题）。

用法：python3 tools/panel.py [宽] [高]
"""
import sys

import gi

gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
from gi.repository import Gtk, GtkLayerShell  # noqa: E402

W = int(sys.argv[1]) if len(sys.argv) > 1 else 400
H = int(sys.argv[2]) if len(sys.argv) > 2 else 200


class Panel(Gtk.Window):
    def __init__(self):
        super().__init__()
        visual = self.get_screen().get_rgba_visual()
        if visual is not None:
            self.set_visual(visual)
        self.set_app_paintable(True)

        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.TOP)
        GtkLayerShell.set_namespace(self, 'testpanel')
        for edge in (GtkLayerShell.Edge.RIGHT, GtkLayerShell.Edge.BOTTOM):
            GtkLayerShell.set_anchor(self, edge, True)
        self.set_size_request(W, H)

        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        cr.set_source_rgba(0.1, 0.1, 0.15, 0.5)
        cr.paint()
        return False


Panel().show_all()
Gtk.main()
