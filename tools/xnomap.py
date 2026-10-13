#!/usr/bin/env python3
# 「建了列但从不映射」的 X11 探针：给 test-emptycol-nomap.sh 当靶子。
#
# 它造的是用户 dump 里那个窗口 50（标题 `Input`）的**最小形态**：两个 X11 窗口，
# 其中 A **从生下来就没 map 过**（`map()` 都不调），B 正常上屏。两个窗口都跑同一套
# 属性动作：
#   1) 声明自己是对话框（`_NET_WM_WINDOW_TYPE_DIALOG` → vidow 判成浮动 FIXED）；
#   2) 再把这条属性删掉（→ vidow 重判成平铺 NONE）。
# 第 2 步走的是 vidow 的「浮动 → 平铺」那条路（`vidow_float_set_mode` →
# `vidow_view_join_layout`），也就是当初给那个 `Input` 窗口建列的那一步：
#   * A 没上屏 —— 以前这一步照样给它建一条列，屏上什么都不画、那格却照占，
#     就是用户报的「凭空多出一列」；
#   * B 上屏了 —— 它该照常拿到列（对照组：修法不能把上屏的窗口一起拦掉）。
# 最后 A 自己 map 一次：验证「延迟到首次 map」—— 上屏之后列该补回来。
#
# 用法: DISPLAY=:0 python3 tools/xnomap.py
# 输出: 每步一行，直到被 kill。
import sys
import time

from Xlib import X, Xatom, display

WM_NAME = "emptycol-nomap"
WM_MAPPED = "emptycol-mapped"
NET_WM_WINDOW_TYPE = "_NET_WM_WINDOW_TYPE"
NET_WM_WINDOW_TYPE_DIALOG = "_NET_WM_WINDOW_TYPE_DIALOG"

DIALOG_HOLD = 1.5   # 声明对话框之后等多久才删（让 vidow 把重判跑完）
MAP_DELAY = 4.0     # A 什么时候才 map（测试在那之前断言「它没占列」）


def log(msg):
    print("xnomap: %s" % msg, flush=True)


def new_window(d, name):
    screen = d.screen()
    win = screen.root.create_window(
        0, 0, 400, 300, 0, screen.root_depth,
        X.InputOutput, X.CopyFromParent,
        background_pixel=screen.white_pixel,
        event_mask=X.ExposureMask | X.StructureNotifyMask)
    win.change_property(d.intern_atom("_NET_WM_NAME"),
                        d.intern_atom("UTF8_STRING"), 8, name.encode("utf-8"))
    win.set_wm_class("emptycol", "Emptycol")
    return win


def set_dialog(d, win, atoms):
    win.change_property(atoms[NET_WM_WINDOW_TYPE], Xatom.ATOM, 32,
                        [atoms[NET_WM_WINDOW_TYPE_DIALOG]])
    d.sync()


def clear_dialog(d, win, atoms):
    win.delete_property(atoms[NET_WM_WINDOW_TYPE])
    d.sync()


def main():
    d = display.Display()
    atoms = {
        name: d.intern_atom(name)
        for name in (NET_WM_WINDOW_TYPE, NET_WM_WINDOW_TYPE_DIALOG)
    }

    # A：**故意不 map** —— 它一辈子不上屏（用户的窗口 50 就是这样）。
    a = new_window(d, WM_NAME)
    d.sync()
    log("建窗 A=%s（没有 map）" % WM_NAME)

    # B：同一个进程里的对照组，正常上屏。
    b = new_window(d, WM_MAPPED)
    b.map()
    d.sync()
    log("建窗 B=%s 并 map（对照组）" % WM_MAPPED)

    time.sleep(0.5)
    set_dialog(d, a, atoms)
    set_dialog(d, b, atoms)
    log("两个窗口都声明成对话框（浮动）")

    time.sleep(DIALOG_HOLD)
    clear_dialog(d, a, atoms)
    clear_dialog(d, b, atoms)
    log("两个窗口都删掉类型（vidow 该重判成平铺）")

    time.sleep(MAP_DELAY - DIALOG_HOLD)
    a.map()
    d.sync()
    log("A 现在才 map（延迟到首次 map 该把列补回来）")

    try:
        while True:
            d.next_event()
    except Exception as exc:  # 合成器退出 / 连接断
        log("退出：%s" % exc)
        sys.exit(0)


if __name__ == "__main__":
    main()
