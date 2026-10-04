#!/usr/bin/env python3
"""xdg-foreign 测试客户端：真实客户端行为，不是手写协议。

开两个窗口，把第一个通过 xdg-foreign-v2 导出成 handle，再用这个 handle 把
第二个窗口挂上去（GTK4 的 Gdk.Toplevel.export_handle / set_transient_for_exported，
就是 GTK 应用自己走的那条路）。

打印 `EXPORTED=<handle>` 之后一直挂着，方便外部（比如 portal 的 parent_window
参数）拿这个 handle 去用。

用法: WAYLAND_DISPLAY=wayland-0 ./tools/foreign-app.py
"""

import sys

import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gdk, GLib, Gtk  # noqa: E402

state = {"handle": None, "imported": None}


def on_exported(toplevel, result, child):
    try:
        handle = toplevel.export_handle_finish(result)
    except GLib.Error as e:
        print(f"EXPORT-FAILED {e.message}", flush=True)
        return
    state["handle"] = handle
    print(f"EXPORTED={handle}", flush=True)

    # 第二个窗口用这个 handle 把自己挂到第一个窗口上。
    child.set_transient_for_exported(handle)
    print("IMPORTED=ok", flush=True)


def on_activate(app):
    parent = Gtk.ApplicationWindow(application=app, title="foreign-parent")
    parent.set_default_size(320, 200)
    parent.present()

    child = Gtk.ApplicationWindow(application=app, title="foreign-child")
    child.set_default_size(240, 160)
    child.present()

    toplevel = parent.get_surface()
    if not isinstance(toplevel, Gdk.Toplevel):
        print("NO-TOPLEVEL", flush=True)
        return
    toplevel.export_handle(on_exported, child)


app = Gtk.Application(application_id="dev.vidow.ForeignApp")
app.connect("activate", on_activate)
sys.exit(app.run(sys.argv))
