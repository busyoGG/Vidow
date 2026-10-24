#!/usr/bin/env python3
"""扫描某个窗口的命中区域，报告「死区」——命中测试返回空、但窗口明明画在那里的点。

用法：deadzone.py <窗口标题> [--step N]

它先用 vidowmsg windows 拿到窗口的几何，再对窗口内均匀撒点，逐点问
vidowmsg pick-window，统计哪些点没命中**这个窗口**（命中别的窗口也算死区，
因为那说明区域被别的窗口盖住了 —— 只有命中 None 才是真正的死区）。
"""
import json
import os
import subprocess
import sys

VIDOWMSG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                        "build", "vidowmsg")
VIDOWMSG = os.path.normpath(VIDOWMSG)


def msg(*args):
    r = subprocess.run([VIDOWMSG] + [str(a) for a in args],
                       capture_output=True, text=True)
    return r.stdout


def windows():
    out = msg("windows")
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return []


def pick(x, y):
    out = msg("pick-window", int(x), int(y))
    try:
        return json.loads(out)
    except json.JSONDecodeError:
        return None


def main():
    if len(sys.argv) < 2:
        print("用法: deadzone.py <窗口标题> [--step N]")
        return 2
    title = sys.argv[1]
    step = 10
    if "--step" in sys.argv:
        step = int(sys.argv[sys.argv.index("--step") + 1])

    target = None
    for w in windows():
        if w.get("title") == title:
            target = w
            break
    if target is None:
        print("FAIL 没找到窗口 %r；现有窗口：%s" %
              (title, [w.get("title") for w in windows()]))
        return 1

    g = target["float_geometry"] if target.get("floating") else target["geometry"]
    x0, y0, gw, gh = g["x"], g["y"], g["width"], g["height"]
    print("目标 %r 几何 %d,%d %dx%d（输出 %s）" %
          (title, x0, y0, gw, gh, target.get("output")))

    dead = []
    other = []
    total = 0
    # 内缩 2px 避开边框
    xs = list(range(x0 + 2, x0 + gw - 2, step))
    ys = list(range(y0 + 2, y0 + gh - 2, step))
    for y in ys:
        row = []
        for x in xs:
            total += 1
            d = pick(x, y)
            w = d.get("window") if isinstance(d, dict) else None
            t = w.get("title") if isinstance(w, dict) else None
            if t == title:
                row.append(".")
            elif t is None:
                row.append("X")
                dead.append((x, y))
            else:
                row.append("o")
                other.append((x, y, t))
        print("  y=%-6d %s" % (y, "".join(row)))

    print()
    print("撒点 %d 个：命中 %d，死区 %d，被别的窗口挡住 %d" %
          (total, total - len(dead) - len(other), len(dead), len(other)))
    if dead:
        xs_d = [p[0] for p in dead]
        ys_d = [p[1] for p in dead]
        print("死区范围 x %d..%d（窗口 %d..%d），y %d..%d（窗口 %d..%d）" %
              (min(xs_d), max(xs_d), x0, x0 + gw - 1,
               min(ys_d), max(ys_d), y0, y0 + gh - 1))
        print("死区右边界占窗口宽 %.1f%%" %
              (100.0 * (min(xs_d) - x0) / gw))
        print("FAIL 有死区")
        return 1
    print("PASS 没有死区")
    return 0


if __name__ == "__main__":
    sys.exit(main())
