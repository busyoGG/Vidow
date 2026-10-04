#!/usr/bin/env python3
"""layer-shell 背景（壁纸）。

给模糊测试提供有对比度的内容：渐变底 + 硬边方块，模糊之后边缘会糊开，
可以量化。用法：WAYLAND_DISPLAY=wayland-0 python3 tools/wall.py
"""
import gi

gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
import cairo  # noqa: E402
from gi.repository import Gtk, GtkLayerShell  # noqa: E402


class Wall(Gtk.Window):
    def __init__(self):
        super().__init__()
        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.BACKGROUND)
        GtkLayerShell.set_namespace(self, 'wall')
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
        grad = cairo.LinearGradient(0, 0, w, h)
        grad.add_color_stop_rgb(0.0, 0.04, 0.06, 0.14)
        grad.add_color_stop_rgb(0.5, 0.14, 0.24, 0.46)
        grad.add_color_stop_rgb(1.0, 0.58, 0.20, 0.34)
        cr.set_source(grad)
        cr.paint()

        # Hard-edged blocks: blur turns these into soft gradients.
        cr.set_source_rgb(1.0, 0.85, 0.20)
        for i in range(0, w, 120):
            for j in range(0, h, 120):
                if (i // 120 + j // 120) % 2 == 0:
                    cr.rectangle(i + 12, j + 12, 64, 64)
        cr.fill()
        return False


Wall().show_all()
Gtk.main()
