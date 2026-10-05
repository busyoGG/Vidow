#!/usr/bin/env python3
"""一块 top 层「面板」：向外留 40px 全透明的边距，只中间画一个圆角半透明块。

用来盯「模糊会不会漏进元素自己透明的部分」：vidow 的模糊是垫在 surface 节点下面的
**一整块矩形**，透明边距里也会透出模糊（本该是原样显示的背景）；niri 的 ignore-alpha
拿元素自己的 alpha 纹理做 `step`，会把那儿从模糊里掐掉 —— vidow 还没有这条路。

用法：python3 tools/padpanel.py [宽] [高]
"""
import sys
import gi
gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
from gi.repository import Gtk, GtkLayerShell  # noqa: E402

PAD = 40


class Shape(Gtk.Window):
    def __init__(self):
        super().__init__()
        v = self.get_screen().get_rgba_visual()
        if v is not None:
            self.set_visual(v)
        self.set_app_paintable(True)
        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.TOP)
        GtkLayerShell.set_namespace(self, 'shape')
        for e in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.LEFT):
            GtkLayerShell.set_anchor(self, e, True)
        w = int(sys.argv[1]) if len(sys.argv) > 1 else 500
        h = int(sys.argv[2]) if len(sys.argv) > 2 else 300
        self.set_size_request(w, h)
        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        w = widget.get_allocated_width()
        h = widget.get_allocated_height()
        # 向外留 PAD 像素完全不画（alpha 0），中间一个圆角半透明块
        x0, y0, x1, y1 = PAD, PAD, w - PAD, h - PAD
        r = 24.0
        cr.new_sub_path()
        cr.arc(x0 + r, y0 + r, r, 3.14159265, 1.5 * 3.14159265)
        cr.arc(x1 - r, y0 + r, r, 1.5 * 3.14159265, 2 * 3.14159265)
        cr.arc(x1 - r, y1 - r, r, 0.0, 0.5 * 3.14159265)
        cr.arc(x0 + r, y1 - r, r, 0.5 * 3.14159265, 3.14159265)
        cr.close_path()
        cr.set_source_rgba(0.1, 0.1, 0.15, 0.5)
        cr.fill()
        return False


Shape().show_all()
Gtk.main()
