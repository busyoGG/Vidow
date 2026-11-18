#!/usr/bin/env python3
"""半透明窗口（真 RGBA）：验证私有底衬 + kawase 背景模糊。

为什么不用 tools/glass.py：那个只画了半透明颜色的网格，但窗口本身没开
    自绘 + RGBA visual，合成器拿到的还是不透明缓冲 —— 模糊链根本不建。
"""
import os
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk  # noqa: E402


class Silk(Gtk.Window):
    def __init__(self, title, alpha=0.55):
        super().__init__(title=title)
        self.alpha = alpha
        self.set_default_size(520, 400)
        self.set_app_paintable(True)                 # 关键：自己画背景
        screen = self.get_screen()
        vis = screen.get_rgba_visual()
        if vis is not None:
            self.set_visual(vis)                     # 关键：告诉合成器有 alpha
        self.connect('draw', self.on_draw)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        w = widget.get_allocated_width()
        h = widget.get_allocated_height()
        cr.set_operator(1)                           # cairo.OPERATOR_SOURCE
        cr.set_source_rgba(0.25, 0.45, 0.95, self.alpha)
        cr.rectangle(0, 0, w, h)
        cr.fill()
        return False


win = Silk(os.environ.get('SILK_TITLE', 'silk'))
win.show_all()
Gtk.main()
