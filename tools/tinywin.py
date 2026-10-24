#!/usr/bin/env python3
"""一个 1x1 的辅助 toplevel（模拟 QQ / Electron 拉起的 wl-clipboard）。

它只要一个能 set_selection 的键盘 serial，不是给人看的窗口。合成器该认出它、
**不**把它塞进布局（否则「粘一次，窗口动一下」）。

用法：
    python3 tools/tinywin.py            一直开着（Ctrl-C 结束）
    python3 tools/tinywin.py --ms 800   开 800ms 后自己关（脚本化测试用）
"""
import sys

import gi

gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib  # noqa: E402


class Tiny(Gtk.Window):
    def __init__(self):
        super().__init__(title='tinywin-helper')
        self.set_default_size(1, 1)
        self.set_resizable(False)
        self.set_decorated(False)
        # 钉死 1x1 的几何请求：客户端自己报「我就要这么大」，
        # 合成器那边 surface->current 才会稳定是 1x1。
        self.set_size_request(1, 1)
        area = Gtk.DrawingArea()
        area.set_size_request(1, 1)
        area.connect('draw', self.on_draw)
        self.add(area)

    def on_draw(self, widget, cr):
        cr.set_source_rgb(0.0, 0.0, 1.0)
        cr.paint()
        return False


def main():
    ms = 0
    args = sys.argv[1:]
    for i, a in enumerate(args):
        if a == '--ms' and i + 1 < len(args):
            ms = int(args[i + 1])

    win = Tiny()
    win.connect('destroy', Gtk.main_quit)
    win.show_all()
    if ms > 0:
        GLib.timeout_add(ms, Gtk.main_quit)
    Gtk.main()


if __name__ == '__main__':
    main()
