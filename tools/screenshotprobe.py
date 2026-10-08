#!/usr/bin/env python3
"""发一次真的 Screenshot / PickColor 请求（走 org.freedesktop.portal.Screenshot）。

用法：
    ./tools/screenshotprobe.py [--interactive] [--pick-color] [--timeout 30]

    --interactive  让后端弹「框一块」的交互式选择器（默认：直接截整块屏）
    --pick-color   走 PickColor（点一下取色，回 (ddd)）
    --timeout      单个请求等多久（默认 30 秒；交互式要等用户）

输出（每步一行）：
    Screenshot: code=0
        uri=file:///home/…/截图-123.png
    PickColor: code=0
        color=(0.2, 0.4, 0.6)

退出码：0 = code=0 且拿到了 uri / color；1 = 失败或超时。
"""

import argparse
import os
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

DEST = "org.freedesktop.portal.Desktop"
OBJ = "/org/freedesktop/portal/desktop"
IFACE = "org.freedesktop.portal.Screenshot"
REQ_IFACE = "org.freedesktop.portal.Request"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--interactive", action="store_true")
    ap.add_argument("--pick-color", action="store_true")
    ap.add_argument("--timeout", type=float, default=30.0)
    args = ap.parse_args()

    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    sender = conn.get_unique_name()[1:].replace(".", "_")
    pid = os.getpid()
    seq = [0]
    loop = GLib.MainLoop()
    seen = {}

    def on_response(_c, _s, path, _i, _sig, params_, *rest):
        seen[path] = params_.unpack()
        loop.quit()

    conn.signal_subscribe(DEST, REQ_IFACE, "Response", None, None, 0,
                          on_response)
    # GDBus 的 AddMatch 是异步发的：先跟总线来回一次，免得 Response 比匹配规则先到。
    conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                   "org.freedesktop.DBus", "GetId", None, None,
                   Gio.DBusCallFlags.NONE, -1, None)

    method = "PickColor" if args.pick_color else "Screenshot"
    seq[0] += 1
    token = "vsp%d%s%d" % (pid, "c" if args.pick_color else "s", seq[0])
    handle = "%s/request/%s/%s" % (OBJ, sender, token)
    opts = {
        "handle_token": GLib.Variant("s", token),
        "interactive": GLib.Variant("b", args.interactive),
        "modal": GLib.Variant("b", True),
    }

    seen.clear()
    box = {}

    def on_timeout():
        box["timeout"] = True
        loop.quit()
        return False

    tid = GLib.timeout_add(int(args.timeout * 1000), on_timeout)
    # 前端的签名是 (s app_id, a{sv} options)（**没有 parent_window**），返回的是
    # 它自己的请求句柄（o）—— 就是按 handle_token 算出来的那条路径。
    ret = conn.call_sync(DEST, OBJ, IFACE, method,
                         GLib.Variant("(sa{sv})", ("", opts)), None,
                         Gio.DBusCallFlags.NONE, -1, None)
    actual = ret.unpack()[0] if ret is not None else None
    if actual is not None and actual != handle:
        handle = actual
    print("%s:" % method, flush=True)
    loop.run()
    if not box.get("fired"):
        GLib.source_remove(tid)
    if box.get("timeout"):
        print("    超时（%.0fs 没有 Response）—— 选择器没人点？" % args.timeout, flush=True)
        return 1
    code, results = seen.get(handle, (None, None))
    if code is None:
        # 句柄没算对：这条总线上只有我们在用 portal，取唯一一条。
        for _p, v in seen.items():
            code, results = v
    if code != 0:
        print("    Response code=%s（1=用户取消 2=其他错误）" % code, flush=True)
        return 1
    results = results or {}
    print("    code=0", flush=True)
    for k in ("uri", "color"):
        if k in results:
            print("    %s=%s" % (k, results[k]), flush=True)
    if args.pick_color:
        return 0 if "color" in results else 1
    return 0 if "uri" in results else 1


if __name__ == "__main__":
    sys.exit(main())
