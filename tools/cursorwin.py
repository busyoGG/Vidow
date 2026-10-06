#!/usr/bin/env python3
"""一个把指针设成 crosshair 的普通窗口：给「客户端自己设的光标图」当靶子。

`wl_pointer.set_cursor` 设进来的是一张 surface，和主题里的箭头是两回事 —— 摇晃放大
（见 src/cursorfx.c）要记住它、放大结束还得把它原样还回去（不能变成主题箭头）。用法：
python3 tools/cursorwin.py [标题]
"""
import sys

import gi

gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk  # noqa: E402


class CursorWin(Gtk.Window):
    def __init__(self):
        super().__init__(title=sys.argv[1] if len(sys.argv) > 1 else 'cursorwin')
        self.set_default_size(400, 300)
        self.connect('realize', self.on_realize)
        self.connect('destroy', Gtk.main_quit)

    def on_realize(self, widget):
        cursor = Gdk.Cursor.new_from_name(Gdk.Display.get_default(), 'crosshair')
        widget.get_window().set_cursor(cursor)


CursorWin().show_all()
Gtk.main()
