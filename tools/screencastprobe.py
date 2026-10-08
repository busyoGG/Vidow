#!/usr/bin/env python3
"""发一次真的 ScreenCast 请求，把每一步的 Response 打出来。

链路（org.freedesktop.portal.ScreenCast）：
    CreateSession  → SelectSources（后端弹「选哪个屏/窗口」）→ Start（拿到 PipeWire node id）

用法：
    ./tools/screencastprobe.py [--types 1] [--multiple] [--timeout 25]

    --types   1=屏幕 2=窗口 3=都行（默认 1）
    --timeout 单个请求等多久（默认 25 秒；后端要弹对话框，慢）

退出码：0 = 拿到 streams；1 = 某一步失败 / 超时（失败原因打出来）。
订阅是「先按 handle_token 算好 Request 路径再订阅」，portal 允许这么做 —— 它保证
返回的句柄就是我们算出来的那个。
"""

import argparse
import os
import sys
import time

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

DEST = "org.freedesktop.portal.Desktop"
OBJ = "/org/freedesktop/portal/desktop"
SC_IFACE = "org.freedesktop.portal.ScreenCast"
REQ_IFACE = "org.freedesktop.portal.Request"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--types", type=int, default=1)
    ap.add_argument("--multiple", action="store_true")
    ap.add_argument("--cursor-mode", type=int, default=1)
    ap.add_argument("--timeout", type=float, default=25.0)
    ap.add_argument("--hold", type=float, default=0.0,
                    help="拿到 streams 后别退出，保持会话 N 秒（给消费者留时间连上来）")
    args = ap.parse_args()

    conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    sender = conn.get_unique_name()[1:].replace(".", "_")
    pid = os.getpid()
    seq = [0]
    loop = GLib.MainLoop()

    def req_path(token):
        return f"{OBJ}/request/{sender}/{token}"

    def sess_path(token):
        return f"{OBJ}/session/{sender}/{token}"

    seen = {}  # path -> (code, results)

    def on_any_response(_c, _s, path, _i, _sig, params_, *rest):
        seen[path] = params_.unpack()
        loop.quit()

    # 通配订阅：不猜句柄路径（portal 的路径是 request/<sender>/<token>，自己算容易错），
    # 这条总线上只有我们在用 portal，收到哪条就看哪条。
    conn.signal_subscribe(DEST, REQ_IFACE, "Response", None, None, 0,
                          on_any_response)
    # GDBus 的 AddMatch 是异步发的：不先跟总线来回一次，CreateSession 这种
    # 「立刻回」的请求，Response 可能在我们收到匹配规则之前就过去了。
    conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
                   "org.freedesktop.DBus", "GetId", None, None,
                   Gio.DBusCallFlags.NONE, -1, None)

    def call(method, params, handle, timeout):
        """发请求并等它那条 Response。"""
        seen.clear()
        box = {}

        def on_timeout():
            box["timeout"] = True
            box["fired"] = True
            loop.quit()
            return False

        tid = GLib.timeout_add(int(timeout * 1000), on_timeout)
        ret = conn.call_sync(DEST, OBJ, SC_IFACE, method, params, None,
                             Gio.DBusCallFlags.NONE, -1, None)
        actual = ret.unpack()[0] if ret is not None else "(无)"
        if actual != handle:
            print(f"    （句柄不是我们算的那个：{actual}）", flush=True)
        loop.run()
        if not box.get("fired"):
            GLib.source_remove(tid)

        if box.get("timeout"):
            print(f"  {method}: 超时（{timeout:.0f}s 没有 Response）—— 对话框没人点？", flush=True)
            return None
        code, results = seen.get(actual, seen.get(handle, (None, None)))
        if code != 0:
            print(f"  {method}: Response code={code}（1=用户取消 2=其他错误）", flush=True)
            return None
        print(f"  {method}: code=0", flush=True)
        return results or {}

    def next_token(tag):
        seq[0] += 1
        return f"vsc{pid}{tag}{seq[0]}"

    # 1. CreateSession
    token = next_token("a")
    sess_token = f"vscs{pid}"
    handle = req_path(token)
    params = GLib.Variant("(a{sv})", ({
        "handle_token": GLib.Variant("s", token),
        "session_handle_token": GLib.Variant("s", sess_token),
    },))
    print("CreateSession:", flush=True)
    res = call("CreateSession", params, handle, args.timeout)
    if res is None:
        return 1
    session = res.get("session_handle") or sess_path(sess_token)
    print(f"    session={session}", flush=True)

    # 2. SelectSources（后端在这一步弹「选哪个屏 / 窗口」）
    token = next_token("b")
    handle = req_path(token)
    opts = {
        "handle_token": GLib.Variant("s", token),
        "types": GLib.Variant("u", args.types),
        "multiple": GLib.Variant("b", args.multiple),
        "cursor_mode": GLib.Variant("u", args.cursor_mode),
    }
    print("SelectSources:", flush=True)
    res = call("SelectSources", GLib.Variant("(oa{sv})", (session, opts)), handle,
               args.timeout)
    if res is None:
        return 1

    # 3. Start：拿 streams（每个是一条 PipeWire 流，key 是 node id）
    token = next_token("c")
    handle = req_path(token)
    opts = {"handle_token": GLib.Variant("s", token)}
    print("Start:", flush=True)
    res = call("Start", GLib.Variant("(osa{sv})", (session, "", opts)), handle,
               args.timeout)
    if res is None:
        return 1

    streams = res.get("streams") or []
    print(f"    streams={len(streams)}", flush=True)
    for node_id, props in streams:
        print(f"      node={node_id} props={props}", flush=True)
    if not streams:
        print("    （没有流 —— 后端说成功但没给 node id）", flush=True)
        return 1
    print("OK", flush=True)
    if args.hold > 0:
        print(f"保持会话 {args.hold:.0f}s（node={streams[0][0]}）", flush=True)
        time.sleep(args.hold)
        # 关掉会话：客户端退出了前端不一定马上收摊（它那边会话还挂着），虚拟输出这种
        # 「跟着流存在」的东西就一直在。显式 Close 一下，链路两头都干净。
        try:
            conn.call_sync(DEST, session, "org.freedesktop.portal.Session", "Close",
                           None, None, Gio.DBusCallFlags.NONE, -1, None)
            print("会话已关闭", flush=True)
        except Exception as exc:  # noqa: BLE001
            print(f"关会话失败（不影响前面拿到的流）：{exc}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
