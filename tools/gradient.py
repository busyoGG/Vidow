#!/usr/bin/env python3
"""
在指定 Wayland 输出上画一条全屏灰阶渐变（0 -> 255）
用来测试 8bit vs 10bit 输出下的色阶过渡与量化直方图
"""

import sys
import os
import time

try:
    import gi
    gi.require_version('Gtk', '3.0')
    from gi.repository import Gtk, Gdk, GLib
except ImportError:
    print("需要 python3-gobject / Gtk 3", file=sys.stderr)
    sys.exit(1)

class GradientWindow(Gtk.Window):
    def __init__(self, output_name=None):
        super().__init__()
        self.output_name = output_name
        self.set_title("gradient-test")
        self.set_app_paintable(True)
        self.connect("draw", self.on_draw)
        self.connect("destroy", Gtk.main_quit)

    def on_draw(self, widget, cr):
        width = self.get_allocated_width()
        height = self.get_allocated_height()
        import cairo
        pat = cairo.LinearGradient(0, 0, width, 0)
        pat.add_color_stop_rgb(0, 0.0, 0.0, 0.0)
        pat.add_color_stop_rgb(1, 1.0, 1.0, 1.0)
        cr.rectangle(0, 0, width, height)
        cr.set_source(pat)
        cr.fill()
        return False

def main():
    win = GradientWindow()
    win.fullscreen()
    win.show_all()
    if len(sys.argv) > 1 and sys.argv[1] == "--hold":
        seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 2.0
        GLib.timeout_add(int(seconds * 1000), Gtk.main_quit)
    Gtk.main()

if __name__ == "__main__":
    main()
