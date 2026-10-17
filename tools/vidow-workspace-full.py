#!/usr/bin/env python3
"""vidow 的「当前工作区是否被占满」查询 —— noctalia native backend 的原型。

noctalia 的 dock 自动隐藏需要这个信息。它现在只能通过 Hyprland / niri 的专属
通道拿到（见 compositors/hyprland/、compositors/niri/），vidow 没有 → dock 永远
不隐藏。这个脚本走 vidow 自己的 IPC（$VIDOW_SOCKET）把同样的信息算出来。

用法：
    ./tools/vidow-workspace-full.py [输出名]
输出：
    0 = 没占满（dock 该显示），1 = 占满了（dock 该隐藏）
    输出名省略 = 所有输出里**任意一块**占满就返回 1
"""
import json, os, socket, sys

def query(cmd="state"):
    path = os.environ.get("VIDOW_SOCKET")
    if not path:
        sys.exit("VIDOW_SOCKET 没设 —— 不在 vidow 会话里")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(1.0)
    s.connect(path)
    s.sendall((cmd + "\n").encode())
    buf = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    return json.loads(buf)

def main():
    want = sys.argv[1] if len(sys.argv) > 1 else None
    st = query()
    for out in st.get("outputs", []):
        if want and out.get("name") != want:
            continue
        # 占满 = 这块屏的活动工作区里，有窗口所在的列宽 ≥ 1.0（vidow 没有单独的
        # maximize，niri 也是拿整列宽度当的），或者有全屏窗口。
        for ws in out.get("workspaces", []):
            if not ws.get("active"):
                continue
            for w in ws.get("windows", []):
                if w.get("fullscreen") or (w.get("column_width") or 0) >= 0.999:
                    print(1)
                    return
    print(0)

main()
