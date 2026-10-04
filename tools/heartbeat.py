#!/usr/bin/env python3
"""心跳窗口：每一帧打印一行 `frame <n> <毫秒>`。

用来量「客户端还在不在出帧」：Wayland 的帧回调（wl_surface.frame）是合成器
「我要用你的画面了」的许可，客户端（GTK/Qt/mpv）都拿它做节流。窗口滚到视口外、
或者所在工作区被藏起来时，场景里什么都没画到它，帧回调就没了 —— 客户端于是停帧。
这个窗口把「还在出帧」变成可断言的一行行输出。"""
import sys
import time

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib  # noqa: E402

W, H = 400, 300
start = time.monotonic()
frames = [0]
limit_ms = int(sys.argv[2]) * 1000 if len(sys.argv) > 2 else 0


class Beat(Gtk.Window):
    def __init__(self):
        super().__init__(title=sys.argv[1] if len(sys.argv) > 1 else 'heartbeat')
        self.set_default_size(W, H)
        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)
        self.hue = 0.0

    def on_draw(self, widget, cr):
        frames[0] += 1
        w = widget.get_allocated_width()
        h = widget.get_allocated_height()
        self.hue = (self.hue + 0.02) % 1.0
        cr.set_source_rgb(0.1 + 0.2 * self.hue, 0.35, 0.75)
        cr.rectangle(0, 0, w, h)
        cr.fill()
        print("frame %d %d" % (frames[0], int((time.monotonic() - start) * 1000)),
              flush=True)
        # 立刻再要一帧：这样出帧速度完全由合成器的帧回调决定
        widget.queue_draw()
        return False


win = Beat()
win.show_all()

def tick():
    if limit_ms and (time.monotonic() - start) * 1000 > limit_ms:
        Gtk.main_quit()
        return False
    return True

GLib.timeout_add(200, tick)
Gtk.main()
