#!/usr/bin/env python3
"""不停重画的测试窗口（模拟视频 / 浏览器）：每 ~16ms 重画一帧，内容每帧都不同。

用来验证「屏幕另一头的客户端一直在交帧，不该让别处的模糊底衬每帧重抓」
（见 test-blurlive.sh / README 的 145）。

用法: python3 tools/spin.py <标题> [r g b] [mode [bx by]]   （颜色 0..1）
      mode = full（默认，每帧整块重画，亮条来回跑）
           = small（每帧只重画窗口里 80x40 那一小块，亮条在小块里跑）——
             模拟「窗口里只有一条进度条 / 光标在动」：客户端报的损伤就是这一小块，
             压着它的那片模糊不该整块重抓（见 test-blursmall.sh）。
           = step（同上只动那一小块，亮条什么时候挪由**测试控制**：环境变量
             SPIN_STEP_FILE 指的那个文件一出现就挪一次，之后一直不动）—— 内容
             「变一下就停住」，两次运行的最终画面才有得逐像素比对（见
             test-blurpartial.sh）。没设那个变量就退回「第 120 帧挪一次」。
"""
import os
import sys
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib

TITLE = sys.argv[1] if len(sys.argv) > 1 else 'spin'
R, G, B = (float(v) for v in (sys.argv[2:5] if len(sys.argv) >= 5 else ('1', '0', '0')))
W, H = 400, 300
MODE = sys.argv[5] if len(sys.argv) > 5 else 'full'   # full / small / step
SMALL = MODE in ('small', 'step')
STEP_AT = 120  # step：没给 SPIN_STEP_FILE 时，第 120 帧之后亮条挪到右边，之后不动
STEP_FILE = os.environ.get('SPIN_STEP_FILE')
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
        if MODE == 'small':
            # 步长和范围取互质（7 / 61）：周期 ≈ 1 秒且**不整除**常见的截图间隔，
            # 免得「相隔 1.2 秒的两张图里亮条正好跑回原位」（踩过：截图一模一样，
            # test-blursmall 那条「真的只动了一小块」判不出来）
            x = BX + (self.n * 7) % 61
            cr.rectangle(x, BY, 20, BH)
        elif MODE == 'step':
            stepped = (self.n >= STEP_AT) if STEP_FILE is None else os.path.exists(STEP_FILE)
            cr.rectangle(BX + (40 if stepped else 0), BY, 20, BH)
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
