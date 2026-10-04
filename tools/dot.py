#!/usr/bin/env python3
"""背景层的「一个亮点 + 纯平底」壁纸，给模糊的**对位**当靶子。

模糊是线性且各向同性的：一个圆斑糊开之后仍然是同一中心对称的斑，所以量出来的
亮度重心（centroid）就等于圆斑的真实中心 —— 对位偏了多少像素一眼看得出，跟
糊得多厉害无关。用法：python3 tools/dot.py [x] [y]
"""
import sys

import gi

gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
import cairo  # noqa: E402
from gi.repository import Gtk, GtkLayerShell  # noqa: E402

CX = float(sys.argv[1]) if len(sys.argv) > 1 else 640.0
CY = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
# 第二个斑可选：放在没被任何 layer 盖住的地方，用来校准「量出来的坐标本身对不对」
CX2 = float(sys.argv[3]) if len(sys.argv) > 3 else None
CY2 = float(sys.argv[4]) if len(sys.argv) > 4 else None


class Dot(Gtk.Window):
    def __init__(self):
        super().__init__()
        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.BACKGROUND)
        GtkLayerShell.set_namespace(self, 'dot')
        for edge in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.BOTTOM,
                     GtkLayerShell.Edge.LEFT, GtkLayerShell.Edge.RIGHT):
            GtkLayerShell.set_anchor(self, edge, True)

        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        cr.set_source_rgb(0.12, 0.12, 0.12)
        cr.paint()
        cr.set_source_rgb(1.0, 1.0, 1.0)
        cr.arc(CX, CY, 16, 0, 6.283185307179586)
        cr.fill()
        if CX2 is not None:
            cr.arc(CX2, CY2, 16, 0, 6.283185307179586)
            cr.fill()
        return False


Dot().show_all()
Gtk.main()
