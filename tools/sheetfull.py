#!/usr/bin/env python3
"""全屏薄纱：整块屏大小、半透明、带白色网格。给帧序列当「放大镜」——拖起来的
窗口压过它时，能看出底衬有没有跟着格子一起被拉弯。

用法: python3 tools/sheetfull.py <标题> <r> <g> <b> <alpha> <scale>"""
import sys
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk

TITLE = sys.argv[1]
R, G, B, A = (float(v) for v in sys.argv[2:6])
SCALE = float(sys.argv[6])


class W(Gtk.Window):
    def __init__(self):
        super().__init__(title=TITLE)
        self.set_decorated(False)
        sw = Gdk.Display.get_default().get_default_screen()
        w, h = int(1920 / SCALE), int(1080 / SCALE)
        try:
            mon = Gdk.Display.get_default().get_monitor(1)
            g = mon.get_geometry()
            w, h = g.width, g.height
        except Exception:
            pass
        self.resize(w, h)
        self.move(0, 0)
        self.set_app_paintable(True)
        vis = self.get_screen().get_rgba_visual()
        if vis is not None:
            self.set_visual(vis)
        self.connect('draw', self.on_draw)
        self.connect('destroy', Gtk.main_quit)
        self.size = (w, h)

    def on_draw(self, widget, cr):
        W_, H_ = self.size
        cr.set_operator(2)
        cr.set_source_rgba(0, 0, 0, 0)
        cr.paint()
        cr.set_source_rgba(R, G, B, A)
        cr.paint()
        cr.set_source_rgba(1, 1, 1, 1)
        cr.set_line_width(1)
        for x in range(20, W_, 40):
            cr.move_to(x + 0.5, 0); cr.line_to(x + 0.5, H_)
        for y in range(20, H_, 40):
            cr.move_to(0, y + 0.5); cr.line_to(W_, y + 0.5)
        cr.stroke()
        return False


w = W()
w.show_all()
Gtk.main()
