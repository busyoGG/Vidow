#!/usr/bin/env python3
"""不停重画的测试窗口（模拟视频 / 浏览器）：每 ~16ms 重画一帧，内容每帧都不同。

用来验证「屏幕另一头的客户端一直在交帧，不该让别处的模糊底衬每帧重抓」
（见 test-blurlive.sh / README 的 145）。

用法: python3 tools/spin.py <标题> [r g b] [small [bx by]]   （颜色 0..1）
      small：每帧只重画窗口里的一小块（80x40 的条，默认在右下角，bx by 可挪）。
             这模拟的是「窗口里只有一条进度条 / 光标在动」：客户端报的损伤就是
             这一小块，压着它的那片模糊不该整块重抓（见 test-blursmall.sh）。
"""
import sys
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

TITLE = sys.argv[1] if len(sys.argv) > 1 else 'spin'
R, G, B = (float(v) for v in (sys.argv[2:5] if len(sys.argv) >= 5 else ('1', '0', '0')))
W, H = 400, 300
SMALL = len(sys.argv) > 5 and sys.argv[5] == 'small'
BW, BH = 80, 40
BX = int(sys.argv[6]) if len(sys.argv) > 6 else W - BW - 40
BY = int(sys.argv[7]) if len(sys.argv) > 7 else H - BH - 20


class Spin(Gtk.Window):
    def __init__(self):
        super().__init__(title=TITLE)
        self.set_default_size(W, H)
        self.set_resizable(False)
        # 跟 sheet.py 一样：自己画整块（不设 app_paintable + RGBA visual 的话，
        # GTK 会把自己的主题背景和 CSD 标题栏画在我们的内容上面）。
        self.set_app_paintable(True)
        vis = self.get_screen().get_rgba_visual()
        if vis is not None:
            self.set_visual(vis)
        self.n = 0
        self.connect('draw', self.on_draw)
        self.connect('destroy', Gtk.main_quit)
        GLib.timeout_add(16, self.tick)

    def tick(self):
        self.n += 1
        if SMALL:
            # 只让这一小块失效：客户端交出来的损伤区域就是它
            self.queue_draw_area(BX, BY, BW, BH)
        else:
            self.queue_draw()
        return True

    def on_draw(self, widget, cr):
        cr.set_operator(2)  # CAIRO_OPERATOR_OVER
        cr.set_source_rgb(R, G, B)
        # 填满当前的裁剪区：整块失效时就是整块，small 模式下就是那一小块
        cr.paint()
        cr.set_source_rgb(1, 1, 1)
        if SMALL:
            x = BX + (self.n * 5) % max(BW - 20, 1)
            cr.rectangle(x, BY, 20, BH)
        else:
            # 一条来回跑的亮条：每帧都不一样（截图比对能看出它在动）
            x = (self.n * 7) % max(W - 40, 1)
            cr.rectangle(x, 0, 40, H)
        cr.fill()
        return False


def main():
    win = Spin()
    win.show_all()
    Gtk.main()


if __name__ == '__main__':
    main()
