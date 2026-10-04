#!/usr/bin/env python3
"""开三个真实 GTK 窗口，验证浮动窗口的两种模式自动识别：

  1. float-parent  普通窗口            -> 平铺
  2. float-dialog  set_transient_for  -> 跟随父窗口浮动 (FOLLOW)
  3. float-fixed   set_resizable(False) -> 固定位置浮动 (FIXED)

用法: GDK_BACKEND=wayland WAYLAND_DISPLAY=wayland-0 python3 test-float.py
"""
import gi

gi.require_version("Gtk", "3.0")
from gi.repository import GLib, Gtk  # noqa: E402


def make(title, w, h, resizable=True):
    win = Gtk.Window(title=title)
    win.set_default_size(w, h)
    win.set_resizable(resizable)
    box = Gtk.Box()
    box.add(Gtk.Label(label=title))
    win.add(box)
    win.show_all()
    return win


parent = make("float-parent", 500, 400)
parent.connect("destroy", Gtk.main_quit)


def open_dialog():
    dialog = make("float-dialog", 320, 220)
    dialog.set_transient_for(parent)  # -> xdg_toplevel.set_parent
    return False


def open_fixed():
    make("float-fixed", 260, 180, resizable=False)
    return False


GLib.timeout_add_seconds(3, open_dialog)
GLib.timeout_add_seconds(6, open_fixed)
Gtk.main()
