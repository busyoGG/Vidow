#!/usr/bin/env python3
"""半透明的 top 层 layer surface（状态栏的样子），给 layer 背后的模糊当靶子。

只画一层 50% 黑的底、别的什么都不画 —— 于是这块区域画出来 = 背后糊不糊一眼可见：
没模糊时透出来的是壁纸的硬边，模糊之后是糊开的一片。

用法：WAYLAND_DISPLAY=wayland-0 python3 tools/bar.py
环境：VIDOW_TEST_BAR_ALPHA=1 画一块**整块不透明**的状态栏（给「看不见的模糊不算」
      那条判据当靶子，见 test-layerblur.sh 最后一节）。
"""
import os

import gi

gi.require_version('Gtk', '3.0')
gi.require_version('GtkLayerShell', '0.1')
from gi.repository import Gtk, GtkLayerShell  # noqa: E402


class Bar(Gtk.Window):
    def __init__(self):
        super().__init__()
        # GTK 默认窗口是不透明的（alpha 会被合成到窗口底色上）：要真的半透明，
        # 必须拿 RGBA visual + set_app_paintable，否则画出来是一块不透明的灰。
        # 反过来，alpha = 1 时不拿 RGBA visual：缓冲就是 XRGB（不透明格式），
        # 合成器一眼能看出「这块底下垫模糊一个像素都透不出来」（见 fx.c 的
        # layer_blur_visible）—— 真客户端里不透明的状态栏也该这么报。
        if float(os.environ.get('VIDOW_TEST_BAR_ALPHA', '0.5')) < 1.0:
            visual = self.get_screen().get_rgba_visual()
            if visual is not None:
                self.set_visual(visual)
            self.set_app_paintable(True)

        GtkLayerShell.init_for_window(self)
        GtkLayerShell.set_layer(self, GtkLayerShell.Layer.TOP)
        GtkLayerShell.set_namespace(self, 'testbar')
        for edge in (GtkLayerShell.Edge.TOP, GtkLayerShell.Edge.LEFT,
                     GtkLayerShell.Edge.RIGHT):
            GtkLayerShell.set_anchor(self, edge, True)
        GtkLayerShell.set_exclusive_zone(self, 40)
        self.set_size_request(-1, 40)

        area = Gtk.DrawingArea()
        area.connect('draw', self.on_draw)
        self.add(area)
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        alpha = float(os.environ.get('VIDOW_TEST_BAR_ALPHA', '0.5'))
        cr.set_source_rgba(0.1, 0.1, 0.15, alpha)
        cr.paint()
        return False


Bar().show_all()
Gtk.main()
