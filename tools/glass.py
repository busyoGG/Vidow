#!/usr/bin/env python3
"""半透明定尺寸浮动窗口，用来验证窗口透明度 + kawase 背景模糊。"""
import os
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk  # noqa: E402


class Glass(Gtk.Window):
    def __init__(self):
        super().__init__(title='glass')
        self.set_default_size(420, 320)
        self.set_resizable(False)
        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        # VIDOW_TEST_EMPTY=1 时完全不画内容：只剩背景模糊 + 描边 + 阴影，
        # 用来单独验证 kawase 与透明度剔除。
        if os.environ.get('VIDOW_TEST_EMPTY') == '1':
            return False
        # 只画一个细网格，其余留空，方便看出背后的模糊
        w = widget.get_allocated_width()
        h = widget.get_allocated_height()
        cr.set_source_rgb(1, 1, 1)
        cr.set_line_width(1)
        for x in range(0, w, 40):
            cr.move_to(x + 0.5, 0); cr.line_to(x + 0.5, h)
        for y in range(0, h, 40):
            cr.move_to(0, y + 0.5); cr.line_to(w, y + 0.5)
        cr.stroke()
        return False


win = Glass()
win.show_all()
Gtk.main()
