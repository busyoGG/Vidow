#!/bin/bash
# XWayland 的 HiDPI 告知：_XWAYLAND_GLOBAL_OUTPUT_SCALE + Xft.dpi
#
# 靶子 = 「X 的逻辑分辨率和面板对不上，微信登录窗只有 1/4 大」这一类问题。两件事：
#
#   1) _XWAYLAND_GLOBAL_OUTPUT_SCALE：打过 HiDPI 补丁的 Xwayland（xorg-xwayland-lily
#      / xorg 的 MR!733）**收到**这个属性之后，才会把 X 屏幕和 RandR 模式按
#      「逻辑 × 缩放 = 设备像素」报出来 —— 正是 vidow 内部「X 像素 = 逻辑 ×
#      xwayland_scale」的假设。不写它，X 屏幕只有逻辑尺寸：按屏幕尺寸算布局的
#      客户端（GTK）会把整屏当成一半大小，最大化的窗口只占 1/4 面积，固定尺寸的
#      登录窗也和面板对不上。（没打补丁的 Xwayland 里这个原子不存在，vidow 就
#      不写它，行为跟以前一样。）
#
#   2) Xft.dpi = 96 × 缩放：GDK_SCALE / QT_SCALE_FACTOR 覆盖不到的客户端
#      （Chromium/CEF = 微信登录窗、Electron、Java、Wine）只认它。不给，它们按
#      1 倍画，而 vidow 按 xwayland_scale 除 —— 窗口只剩该有的 1/4。
#
# 判据（全 headless，一块 2 倍屏 1280x720 → 逻辑 640x360）：
#   1) xprop 查得到 _XWAYLAND_GLOBAL_OUTPUT_SCALE = 2 与 Xft.dpi = 192
#   2) X 屏幕 = 布局逻辑尺寸 × 2（1280x720）—— 补丁生效的证据
#   3) 端到端点一下：X 客户端（xev）收到的 root 坐标 = 布局坐标 × 2
#   4) 缩放改成 1：两个属性、X 屏幕都跟着变（640x360）
#   5) 日志里没有 assert / abort
#
# 用法：./test-xwayland-hidpi.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-xhidpi-test
CFGDIR="$RUNDIR/home"
FAILED=0
VIDOW_PGID=""
XEV_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$XEV_PID" ] && kill "$XEV_PID" 2>/dev/null
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v xev >/dev/null || { echo "test-xwayland-hidpi.sh: 需要 xev"; exit 1; }
[ -x ./build/xpopup ] || { echo "test-xwayland-hidpi.sh: 先构建"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

# 一块 2 倍屏：物理 1280x720，逻辑 640x360
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
outputs = "HEADLESS-1:2"
scale = "auto"
xwayland = true
CFG

setsid ./build/vidow > "$RUNDIR/vidow.log" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$RUNDIR/vidow.log" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$RUNDIR/vidow.log"; then
	echo "vidow 没起来"; tail -5 "$RUNDIR/vidow.log"; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$RUNDIR/vidow.log" | tail -1 | cut -d= -f2)
export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$RUNDIR/vidow.log" | tail -1 | cut -d= -f2)

# X 侧环境：vidow 在 xwayland ready 时就该写好这两个属性
xenv() { # <标签>
	echo "--- $1"
	xprop -root 2>/dev/null | grep -E "RESOURCE_MANAGER|_XWAYLAND_GLOBAL_OUTPUT_SCALE"
	xdpyinfo 2>/dev/null | grep dimensions
	xrandr --query 2>/dev/null | sed -n '2p'
}
xenv "启动后" > "$RUNDIR/xenv-start.txt" 2>&1
cat "$RUNDIR/xenv-start.txt"

# 端到端：xev 的窗口（平铺）点一下，看它收到的 root 坐标
mark "1) 端到端：点窗口里的一个点，X 客户端收到的坐标 = 布局坐标 × 2"
setsid nohup xev -name xevhidi > "$RUNDIR/xev.log" 2>&1 &
XEV_PID=$!
for _ in $(seq 1 60); do grep -aq '"title": "xevhidi"' "$RUNDIR/vidow.log" && break; sleep 0.2; done
sleep 1.0
./build/vidowmsg windows > "$RUNDIR/windows.json" 2>/dev/null

read -r LX LY LW LH <<<"$(python3 - "$RUNDIR/windows.json" <<'PY'
import json, sys
ws = json.load(open(sys.argv[1]))
for w in ws:
    if w.get("title") == "xevhidi":
        g = w["geometry"]
        print(g["x"], g["y"], g["width"], g["height"])
        break
else:
    print(-1, -1, 0, 0)
PY
)"
echo "  xev 窗口在布局 $LX,$LY ${LW}x${LH}"

# 点在窗口里偏右下一点（避开边框）：布局坐标 -> vdrag 的 0..10000 归一化坐标
CX=$((LX + LW * 3 / 4))
CY=$((LY + LH * 3 / 4))
to_extent() { # <像素> <布局尺寸>
	python3 -c "import sys; print(int(round(float(sys.argv[1]) * 10000 / float(sys.argv[2]))))" "$1" "$2"
}
./build/vdrag "$(to_extent $CX 640)" "$(to_extent $CY 360)" \
	"$(to_extent $CX 640)" "$(to_extent $CY 360)" >/dev/null 2>&1
sleep 0.4
./build/vidowmsg action pointer-button 1 press >/dev/null 2>&1
sleep 0.2
./build/vidowmsg action pointer-button 1 release >/dev/null 2>&1
sleep 0.6
kill "$XEV_PID" 2>/dev/null
wait "$XEV_PID" 2>/dev/null
XEV_PID=""
grep -a -E "ButtonPress|root:" "$RUNDIR/xev.log" | head -4

mark "2) 把 X 的全局缩放改成 1 之后跟着变（屏的缩放还是 2，只是不当缩放桥了）"
./build/vidowmsg set-config xwayland_scale 1 > "$RUNDIR/scale1.json" 2>&1
sleep 0.8
xenv "改成 1 倍" > "$RUNDIR/xenv-1x.txt" 2>&1
cat "$RUNDIR/xenv-1x.txt"

mark "3) 断言"
python3 - "$RUNDIR" "$CX" "$CY" "$LX" "$LY" "$LW" "$LH" <<'PY'
import json, os, re, sys

rundir, cx, cy, lx, ly, lw, lh = (sys.argv[1], float(sys.argv[2]), float(sys.argv[3]),
                                  int(sys.argv[4]), int(sys.argv[5]),
                                  int(sys.argv[6]), int(sys.argv[7]))
failed = []

def check(ok, msg):
    print(("  PASS  " if ok else "  FAIL  ") + msg)
    if not ok:
        failed.append(msg)

def read(name):
    return open(os.path.join(rundir, name), encoding="utf-8", errors="replace").read()

log = read("vidow.log")
start = read("xenv-start.txt")
one = read("xenv-1x.txt")

# 1) vidow 自己写了两个属性
check("_XWAYLAND_GLOBAL_OUTPUT_SCALE(CARDINAL) = 2" in start,
      "启动后 root 上有 _XWAYLAND_GLOBAL_OUTPUT_SCALE = 2")
check(re.search(r'RESOURCE_MANAGER\(STRING\) = ".*Xft\.dpi:\\t192', start) is not None,
      "启动后 RESOURCE_MANAGER 里有 Xft.dpi = 192")
check("xwayland: _XWAYLAND_GLOBAL_OUTPUT_SCALE = 2" in log,
      "日志里说了它写了这个属性（补丁生效的话 X 屏幕会跟着变成设备像素）")
check("xwayland: Xft.dpi = 192" in log, "日志里说了 Xft.dpi = 192")

# 2) X 屏幕 = 逻辑 × 2
m = re.search(r"dimensions:\s+(\d+)x(\d+) pixels", start)
xw, xh = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
check((xw, xh) == (1280, 720),
      "X 屏幕 %dx%d = 布局 640x360 × 2（补丁生效；没生效的话是 640x360）" % (xw, xh))
check("xwayland: X 屏幕 1280x720" in log and "对不上" not in log.split("X 屏幕")[1][:200],
      "日志里 X 屏幕 1280x720 且没报「对不上」")

# 3) 端到端：xev 收到的 root 坐标 = 布局坐标 × 2
xev = read("xev.log")
mm = None
for mm in re.finditer(r"root:\((\d+),(\d+)\)", xev):
    pass
if mm is None:
    check(False, "xev 没报出 ButtonPress 的 root 坐标（点击没到客户端？）")
else:
    rx, ry = int(mm.group(1)), int(mm.group(2))
    ex, ey = int(cx * 2), int(cy * 2)
    check(abs(rx - ex) <= 2 and abs(ry - ey) <= 2,
          "点布局 %g,%g → xev 收到 root:%d,%d（预期 %d,%d = ×2）" % (cx, cy, rx, ry, ex, ey))

# 4) 改成 1 倍之后
check("_XWAYLAND_GLOBAL_OUTPUT_SCALE(CARDINAL) = 1" in one,
      "改成 1 倍后 root 属性变成 1")
check(re.search(r'RESOURCE_MANAGER\(STRING\) = ".*Xft\.dpi:\\t96', one) is not None,
      "改成 1 倍后 Xft.dpi = 96")
m1 = re.search(r"dimensions:\s+(\d+)x(\d+) pixels", one)
check(m1 is not None and (int(m1.group(1)), int(m1.group(2))) == (640, 360),
      "改成 1 倍后 X 屏幕 %s = 布局 640x360 × 1（补丁把屏幕缩回去了）"
      % (m1.group(0) if m1 else "?"))
check("xwayland: 全局缩放 2.00 -> 1.00" in log, "日志里有「全局缩放 2.00 -> 1.00」")

# 5) 没有崩溃
check("Assertion" not in log and "abort" not in log.lower(),
      "日志里没有 assert / abort")

print()
if failed:
    print("test-xwayland-hidpi.sh: %d 项没通过" % len(failed))
    sys.exit(1)
print("全部通过 / test-xwayland-hidpi.sh: OK")
PY
[ $? -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "test-xwayland-hidpi.sh: OK"
else
	echo "test-xwayland-hidpi.sh: 有失败项"
fi
exit $FAILED
