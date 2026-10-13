#!/usr/bin/env python3
"""背景层的「非周期细节」壁纸：16 逻辑像素的随机灰块 + 三条彩色横带。

给「底衬的内容是不是按设备像素 1:1 画的」当靶子。为什么不用现成的 `tools/wall.py`
（渐变）：渐变太光滑，放大一倍看不出来。为什么不用 `tools/pattern.py`（周期图案）：
周期图案被放大 2 倍之后**看着还是同一个图案**（16px 的块变成 32px 的块，格数变了但
单看一处分不出「放大了」还是「换了个周期的图案」）—— 上一轮就是在那种靶子上量互相关，
峰值 0.07~0.19 全是噪声，得出了「模糊正常」的错误结论。随机块没有周期，配合下面三条
横带（糊过之后重心仍然在原处），「同坐标比大小」才有意义。

三条横带的位置是**逻辑**坐标（80 / 360 / 640），落在 16 的整数倍上，所以客户端按
倍率渲染时它们正好落在设备像素网格上（2 倍屏：160 / 720 / 1280），不引入半像素误差。

用法：
  python3 tools/cells.py             # 当壁纸跑（background 层）
  python3 tools/cells.py --png 路径  # 只把同一张纹理写成 PNG（当参考图）
"""
import random
import sys

CELL = 16          # 逻辑像素
W, H = 1280, 720   # 逻辑像素
SEED = 20261013
BANDS = ((80, (1.0, 0.0, 0.0)),      # 红
         (360, (0.0, 1.0, 0.0)),     # 绿
         (640, (0.0, 0.4, 1.0)))     # 蓝


def cells():
    rng = random.Random(SEED)
    grid = {}
    for y in range(0, H, CELL):
        for x in range(0, W, CELL):
            grid[(x, y)] = rng.uniform(0.08, 0.92)
    return grid


GRID = cells()


def draw(cr):
    """坐标一律**逻辑像素**：GTK 在 2 倍屏上给的 cairo 用户空间已经缩了 2 倍，
    所以逻辑坐标乘倍率就是设备像素，块边界正好落在设备网格上（16 -> 32），
    不引入半像素误差。"""
    for (x, y), v in GRID.items():
        cr.set_source_rgb(v, v, v)
        cr.rectangle(x, y, CELL, CELL)
        cr.fill()
    for yy, col in BANDS:
        cr.set_source_rgb(*col)
        cr.rectangle(0, yy, W, 6)
        cr.fill()


if len(sys.argv) > 2 and sys.argv[1] == '--png':
    import cairo
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    draw(cairo.Context(surf))
    surf.write_to_png(sys.argv[2])
    print('参考纹理写好', sys.argv[2])
    sys.exit(0)

import gi  # noqa: E402

gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
from gi.repository import Gtk, GtkLayerShell  # noqa: E402


class Cells(Gtk.Window):
    def __init__(self):
        super().__init__()
        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.BACKGROUND)
        GtkLayerShell.set_namespace(self, 'cells')
        for edge in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.BOTTOM,
                     GtkLayerShell.Edge.LEFT, GtkLayerShell.Edge.RIGHT):
            GtkLayerShell.set_anchor(self, edge, True)
        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        draw(cr)
        return False


Cells().show_all()
Gtk.main()
