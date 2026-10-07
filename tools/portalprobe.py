#!/usr/bin/env python3
# portal 探针：GTK3 客户端，用 Gtk.FileChooserNative 走 xdg-desktop-portal 的文件
# 选择器（**必须**由调用方设 GTK_USE_PORTAL=1，否则 GTK 自己画对话框、根本不碰
# portal）。给 portaltest.sh 用：它拿这个探针看「对话框到底出没出来、挂在谁身上、
# 谁拿到了焦点」。
#
# 用法: GTK_USE_PORTAL=1 WAYLAND_DISPLAY=wayland-3 python3 tools/portalprobe.py [存活秒数]
# 输出: 每步一行（窗口上屏 / 弹对话框 / 对话框返回了什么）
import os
import sys

import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk  # noqa: E402

ALIVE = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0


def log(msg):
    print("portalprobe: %s" % msg, flush=True)


def main():
    log("GTK_USE_PORTAL=%s WAYLAND_DISPLAY=%s"
        % (os.environ.get("GTK_USE_PORTAL", "（没设）"),
           os.environ.get("WAYLAND_DISPLAY", "（没设）")))

    win = Gtk.Window(title="portalprobe")
    win.set_default_size(360, 240)
    win.connect("destroy", Gtk.main_quit)
    win.show_all()
    log("窗口上屏")

    dlg = Gtk.FileChooserNative.new("portal 探针：挑一个文件或者点取消", win,
                                    Gtk.FileChooserAction.OPEN, "确定", "取消")
    dlg.set_modal(True)

    def on_response(d, resp, _):
        log("对话框返回 %s（%s）"
            % (resp, "选了东西" if resp == Gtk.ResponseType.ACCEPT else "取消 / 关掉了"))
        d.destroy()
        Gtk.main_quit()
        return False

    dlg.connect("response", on_response)

    # 1 秒后再弹：先让窗口 map 完、焦点稳住（跟真实 App 一样）
    GLib.timeout_add(1000, lambda: (log("弹文件选择器（走 portal）"),
                                    dlg.show(), False)[2])
    # 兜底：到点自己收，免得测试挂在对话框上。
    # 注意别调 dlg.hide()：原生（走 portal）对话框的 hide() 里有一句
    # `assert(data != NULL)`，请求还没建起来（portal 起不来 / 后端崩了）时直接
    # abort（GTK 3 的 gtkfilechoosernativeportal.c:532，实测踩过）。直接退主循环，
    # 进程一退对话框跟着消失。
    GLib.timeout_add(int(ALIVE * 1000),
                     lambda: (log("到点，自己收（没等到回应）"), Gtk.main_quit(),
                              False)[2])
    Gtk.main()
    log("退出")
    # 直接 _exit：请求没建起来（portal / 后端没起来）时，GTK 收尾会走
    # gtk_file_chooser_native_portal_hide() 里那句 assert(data != NULL) 直接
    # abort（实测踩过）。探针没什么要收的，绕开析构。
    sys.stdout.flush()
    os._exit(0)


if __name__ == "__main__":
    main()
