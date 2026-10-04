#!/usr/bin/env python3
"""右下角一块 bottom 层的 50% 红面板。

bottom 层在底衬抓取的那几棵树里（background / bottom / windows），所以它自己
会被抓进底衬 —— 而那份底衬正是它背后模糊的来源，于是模糊里会浮出一个「自己的
影子」。这个客户端就是用来盯这件事的：红得扎眼，一眼能在底衬 dump 里认出来。

用法：python3 tools/bpanel.py [宽] [高]
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
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.BOTTOM)
        GtkLayerShell.set_namespace(self, 'bpanel')
        for edge in (GtkLayerShell.Edge.RIGHT, GtkLayerShell.Edge.BOTTOM):
            GtkLayerShell.set_anchor(self, edge, True)
        self.set_size_request(W, H)

        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        cr.set_source_rgba(1.0, 0.0, 0.0, 0.5)
        cr.paint()
        return False


Panel().show_all()
Gtk.main()
