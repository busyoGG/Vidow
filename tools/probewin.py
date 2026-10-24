#!/usr/bin/env python3
"""造一个 X11 窗口，内容分四块不同颜色 —— 用来肉眼/截图看死区和坐标换算。

用法：shapedwin.py <x> <y> <w> <h> [--shape-left-pct N] [--popup]
不带 --popup：普通顶层窗口，四象限四色（左上红/右上绿/左下蓝/右下黄）。
带 --popup ：先建一个父窗口，再弹一个 override-redirect 的小窗（位置由参数定）。
"""
import sys, time
from Xlib import X, display
from Xlib.protocol import event

def main():
    args = [a for a in sys.argv[1:]]
    popup = "--popup" in args
    args = [a for a in args if a != "--popup"]
    shape_pct = None
    if "--shape-left-pct" in args:
        i = args.index("--shape-left-pct")
        shape_pct = float(args[i + 1])
        args = args[:i] + args[i + 2:]
    x, y, w, h = (int(a) for a in args[:4])

    d = display.Display()
    screen = d.screen()
    root = screen.root

    win = root.create_window(
        x, y, w, h, 0,
        screen.root_depth,
        X.InputOutput,
        X.CopyFromParent,
        background_pixel=screen.white_pixel,
        event_mask=X.ExposureMask | X.StructureNotifyMask | X.ButtonPressMask,
    )
    win.set_wm_name("probe")
    win.set_wm_class("probe", "Probe")
    win.map()
    d.sync()

    gc = win.create_gc()

    def paint():
        cw, ch = win.get_geometry().width, win.get_geometry().height
        half_w, half_h = cw // 2, ch // 2
        cmap = screen.default_colormap
        def col(r, g, b):
            return cmap.alloc_color(r * 257, g * 257, b * 257).pixel
        quads = [
            (0, 0, half_w, half_h, col(255, 0, 0)),        # 左上 红
            (half_w, 0, cw - half_w, half_h, col(0, 255, 0)),   # 右上 绿
            (0, half_h, half_w, ch - half_h, col(0, 0, 255)),   # 左下 蓝
            (half_w, half_h, cw - half_w, ch - half_h, col(255, 255, 0)),  # 右下 黄
        ]
        for qx, qy, qw, qh, pixel in quads:
            gc.change(foreground=pixel)
            # fill_rectangle 是**窗口**的方法（GC 只带绘图状态），这是
            # python-xlib 的 API 形状，不是笔误。
            win.fill_rectangle(gc, qx, qy, qw, qh)
        d.flush()

    paint()

    popup_win = None
    if popup:
        # 菜单式 override-redirect：贴着父窗口右下角往外探
        px, py = x + w - 40, y + h - 40
        pw, ph = 200, 150
        popup_win = root.create_window(
            px, py, pw, ph, 0,
            screen.root_depth,
            X.InputOutput,
            X.CopyFromParent,
            background_pixel=screen.white_pixel,
            override_redirect=True,
            event_mask=X.ExposureMask,
        )
        popup_win.set_wm_name("probe-popup")
        popup_win.map()
        d.sync()
        pgc = popup_win.create_gc()
        cmap = screen.default_colormap
        pgc.change(foreground=cmap.alloc_color(0, 255, 255).pixel)
        popup_win.fill_rectangle(pgc, 0, 0, pw, ph)
        pgc.change(foreground=cmap.alloc_color(0, 0, 0).pixel)
        popup_win.fill_rectangle(pgc, 10, 10, pw - 20, 6)  # 黑杠，认朝向
        d.flush()

    # 报告自己的真实 X 几何
    g = win.get_geometry()
    print("win id=0x%x geom=%d,%d %dx%d abs=%d,%d" % (
        win.id, g.x, g.y, g.width, g.height,
        g.x, g.y), flush=True)
    if popup_win is not None:
        pg = popup_win.get_geometry()
        print("popup id=0x%x geom=%d,%d %dx%d" % (
            popup_win.id, pg.x, pg.y, pg.width, pg.height), flush=True)

    while True:
        e = d.next_event()
        if e.type == X.Expose:
            paint()
            if popup_win is not None:
                d.flush()

if __name__ == "__main__":
    main()
