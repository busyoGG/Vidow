#!/usr/bin/env python3
"""layer-shell 背景（壁纸）。

给模糊测试提供有对比度的内容：渐变底 + 硬边方块，模糊之后边缘会糊开，
可以量化。用法：WAYLAND_DISPLAY=wayland-0 python3 tools/wall.py

也可以画一块纯色、换个 namespace —— 测试「全景用另一张壁纸」时要同时起两块
（规则按 namespace 匹配，见 rule.c 的 overview）：

    python3 tools/wall.py                       # 渐变 + 方块，namespace wall
    python3 tools/wall.py "#ff00ff"             # 纯洋红，namespace wall
    python3 tools/wall.py "#ff00ff" wall2       # 纯洋红，namespace wall2
    python3 tools/wall.py --ns wall2 --fine     # 渐变+细密方块，namespace wall2
                                                # （不传颜色 = 画内容，不是纯色）

**每块输出要各起一个** —— layer-shell 背景只盖自己那块屏，只起一个的话另一
块是纯黑，看不出「跨屏前那半边有没有模糊」。
"""
import sys
import os

import gi

gi.require_version('Gtk', '3.0')
# Gdk 必须显式钉 3.0：不钉的话 gi 会先加载 4.0，再要 3.0 就报
# 「Requiring namespace 'Gdk' version '3.0', but '4.0' is already loaded」。
gi.require_version('Gdk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
import cairo  # noqa: E402
from gi.repository import Gdk, Gtk, GtkLayerShell  # noqa: E402

# 位置参数：颜色、namespace。选项（--fine）先摘出去，不然会被当成颜色。
_args = []
FINE = '--fine' in sys.argv
_ns_opt = None
OUTPUT = None
_i = 1
while _i < len(sys.argv):
    a = sys.argv[_i]
    if a == '--output' and _i + 1 < len(sys.argv):
        OUTPUT = sys.argv[_i + 1]
        _i += 2
        continue
    if a == '--ns' and _i + 1 < len(sys.argv):
        _ns_opt = sys.argv[_i + 1]
        _i += 2
        continue
    if not a.startswith('--'):
        _args.append(a)
    _i += 1
color = _args[0] if len(_args) > 0 else None
namespace = _ns_opt if _ns_opt is not None else (
    _args[1] if len(_args) > 1 else 'wall')
rgb = None
if color is not None:
    hexs = color.lstrip('#')
    rgb = tuple(int(hexs[i:i + 2], 16) / 255.0 for i in (0, 2, 4))


class Wall(Gtk.Window):
    def __init__(self):
        super().__init__()
        GtkLayerShell.init_for_window(self)
        # 层可切（VIDOW_WALL_LAYER=background/top/overlay）：
        # test-blurlevel 要量「层模糊的金字塔」，而 vidow 明确不给 background 层
        # 画模糊（壁纸本身就是模糊的源头，见 fx.c:3654 的「这是 background 层」），
        # 所以那套用例得把它放到会模糊的层上才量得到东西。
        layer = {
            "background": GtkLayerShell.Layer.BACKGROUND,
            "bottom": GtkLayerShell.Layer.BOTTOM,
            "top": GtkLayerShell.Layer.TOP,
            "overlay": GtkLayerShell.Layer.OVERLAY,
        }.get(os.environ.get("VIDOW_WALL_LAYER", "background"),
              GtkLayerShell.Layer.BACKGROUND)
        GtkLayerShell.set_layer(self, layer)
        GtkLayerShell.set_namespace(self, namespace)
        # 指定输出（--output HEADLESS-1）：多屏时两个实例默认都会挑同一块
        # （GTK 的默认显示器），另一块就永远是黑的 —— 模糊到底盖没盖住
        # 跨屏那半边就看不出来了。
        if OUTPUT is not None:
            Gdk.Display.get_default()
            mon = None
            n = Gdk.Display.get_default().get_n_monitors()
            for i in range(n):
                m = Gdk.Display.get_default().get_monitor(i)
                if m.get_model() == OUTPUT or str(i) == OUTPUT:
                    mon = m
                    break
            if mon is not None:
                GtkLayerShell.set_monitor(self, mon)
        for edge in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.BOTTOM,
                     GtkLayerShell.Edge.LEFT, GtkLayerShell.Edge.RIGHT):
            GtkLayerShell.set_anchor(self, edge, True)

        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        w = widget.get_allocated_width()
        h = widget.get_allocated_height()
        if rgb is not None:
            cr.set_source_rgb(*rgb)
            cr.paint()
            return False

        grad = cairo.LinearGradient(0, 0, w, h)
        grad.add_color_stop_rgb(0.0, 0.04, 0.06, 0.14)
        grad.add_color_stop_rgb(0.5, 0.14, 0.24, 0.46)
        grad.add_color_stop_rgb(1.0, 0.58, 0.20, 0.34)
        cr.set_source(grad)
        cr.paint()

        # Hard-edged blocks: blur turns these into soft gradients.
        # 「细密」模式（--fine）：格子 40px、方块 20px —— 尺寸和模糊半径
        # （3 级约 29 设备像素）同一量级，糊完之后方块几乎化开，录屏里一眼
        # 就能看出底衬有没有模糊。默认那档（120/64）只软化边缘一圈，缩到
        # 视频尺寸后肉眼几乎看不出来。
        fine = FINE
        step = 40 if fine else 120
        size = 20 if fine else 64
        pad = 4 if fine else 12
        cr.set_source_rgb(1.0, 0.85, 0.20)
        for i in range(0, w, step):
            for j in range(0, h, step):
                if (i // step + j // step) % 2 == 0:
                    cr.rectangle(i + pad, j + pad, size, size)
        cr.fill()
        return False


Wall().show_all()
Gtk.main()
