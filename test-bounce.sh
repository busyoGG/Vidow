#!/bin/bash
# 窗口动画的「q 弹」：入场要**过冲**（冲到比最终尺寸还大一点再收住），关闭要
# **回弹**（缩过头再涨回来一点）。
#
# 实现：`out_elastic` 曲线（anim.c 里的阻尼余弦 1 - e^(-kt)·cos(wt)）+ 入场缩放
# open_scale_from / 关闭缩放 close_scale_to。缩放作用在场景节点上，围绕窗口中心。
#
# 为什么必须量出来：曲线本身好写，难的是「过冲真的落到屏幕上了」——
#   * 曲线写错（比如漏了 t=0 处为 0 的相位）就是「一上来就满尺寸」；
#   * 缩放只改离屏缓冲尺寸、场景节点没跟着缩，屏幕上什么都不会变；
#   * 透明度/着色器进度跟着过冲越界（负 alpha、u_progress > 1）会闪。
# 这几条都不看屏幕发现不了，所以这里按日志里的 `anim open start` / `anim close
# start` 对时，在曲线上几个特征时刻连拍，量蓝色窗口的包围盒。
#
# 靶子：tools/dot.py 纯灰壁纸（整个屏幕没有蓝色像素，检测不会误判）、
# tools/bluewin.py 纯蓝窗口。配置里把缩放拉到 0.5（默认 0.82，量起来余量小）、
# 动画拉到 4s（快门有几十毫秒延迟，曲线得足够长才量得准），关掉边框/圆角/阴影/
# 模糊/淡入淡出 —— 只留几何，包围盒就是窗口自己。
#
# 判据（基准 = 动画结束、缩放归 1 的那一帧的窗口宽 fw）：
#   1) 窗口上屏了
#   2) 入场刚开始那帧明显是小的（< 0.85 fw，曲线从 0.5 弹出来）
#   3) 入场中段那帧**比最终还大**（> 1.02 fw）—— 过冲，回弹的本体
#   4) 关闭中段那帧缩到 0.55 fw 以下，关闭后段那帧又涨回来一点（回弹）
#
# 用法：./test-bounce.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-bounce-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
CFG="$CFGDIR/.config/vidow/config.lua"
FRAMES="$RUNDIR/frames"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""
WIN_PID=""

fail() { echo "  FAIL  $1"; FAILED=1; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-bounce.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }
command -v grim >/dev/null || { echo "test-bounce.sh: 需要 grim"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run" "$FRAMES"
chmod 700 "$RUNDIR/run"

cat > "$CFG" <<'CFGEOF'
vidow.output{ name = "HEADLESS-1", scale = 1 }
scale = "1"
gaps = 10
window_open_ms = 4000
window_close_ms = 4000
open_easing = "out_elastic"
close_easing = "out_elastic"
open_scale_from = 0.5
close_scale_to = 0.5
-- 只测几何：淡入淡出、滑动都关掉，不然包围盒会被透明度/位移搅浑
open_alpha_from = 1.0
close_alpha_to = 1.0
open_slide_px = 0
close_slide_px = 0
decor_enabled = true
corner_radius = 0
border_width = 0
shadow_radius = 0
shadow_color = "#00000000"
blur_enabled = false
CFGEOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

BIN=${VIDOW_BIN:-./build/vidow}

setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "test-bounce.sh: vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

grep -a "^config: open=" "$LOG" | tail -1

setsid nohup python3 ./tools/dot.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map dot" "$LOG" && break; sleep 0.25; done
sleep 0.6

# 按日志里的动画起点对时：客户端 map 多久、快门多久都不影响判据落在曲线的哪一段。
wait_until() {
	python3 - "$1" "$2" <<'PY'
import sys, time
goal = float(sys.argv[1]) - (time.monotonic() - float(sys.argv[2]))
time.sleep(goal if goal > 0 else 0)
PY
}
# 等某条日志出现，返回它出现的时刻（monotonic 秒）
wait_log() {
	for _ in $(seq 1 400); do
		if grep -aq "$1" "$LOG"; then python3 -c 'import time; print(time.monotonic())'; return 0; fi
		sleep 0.02
	done
	return 1
}

setsid nohup python3 ./tools/bluewin.py > "$RUNDIR/win.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
WIN_PID=$!

T0=$(wait_log "anim open start") || { fail "没等到入场动画开始（窗口没上屏？）"; tail -3 "$RUNDIR/win.log"; }
if [ -n "${T0:-}" ]; then
	# 曲线上的特征时刻（见 anim.c 的 out_elastic）：0.03 处刚起步、0.40 处接近
	# 过冲顶点（t≈0.45 到顶）、结束后是基准帧。
	wait_until 0.12 "$T0";  grim -o HEADLESS-1 "$FRAMES/open-tiny.png" 2>/dev/null
	wait_until 1.60 "$T0";  grim -o HEADLESS-1 "$FRAMES/open-over.png" 2>/dev/null
	wait_until 4.60 "$T0";  grim -o HEADLESS-1 "$FRAMES/open-done.png" 2>/dev/null
fi

# 关闭：客户端退出 → 快照关闭动画（window_close_ms = 4000）
if [ -n "${WIN_PID:-}" ]; then
	kill "$WIN_PID" 2>/dev/null
	T1=$(wait_log "anim close start") || fail "没等到关闭动画开始（快照没抓到？）"
	if [ -n "${T1:-}" ]; then
		# 0.45 处缩到最小（回弹的最低点）、0.80 处已经涨回来一点
		wait_until 1.80 "$T1"; grim -o HEADLESS-1 "$FRAMES/close-low.png" 2>/dev/null
		wait_until 3.20 "$T1"; grim -o HEADLESS-1 "$FRAMES/close-back.png" 2>/dev/null
	fi
fi

python3 - "$RUNDIR" <<'PY'
import os, sys
import numpy as np
from PIL import Image

frames = sys.argv[1] + '/frames/'
ok = True

def say(good, msg):
	global ok
	print(('  PASS  ' if good else '  FAIL  ') + msg)
	ok = ok and good

def blue_box(name):
	"""纯蓝窗口的包围盒（壁纸是纯灰，整屏没有蓝像素，不会误判）。"""
	path = frames + name
	if not os.path.exists(path):
		return None
	a = np.asarray(Image.open(path).convert('RGB')).astype(np.int16)
	m = (a[:, :, 2] - a[:, :, 0] > 80) & (a[:, :, 2] - a[:, :, 1] > 80) & (a[:, :, 2] > 100)
	if m.sum() < 500:
		return None
	cols = np.where(m.any(axis=0))[0]
	rows = np.where(m.any(axis=1))[0]
	return (int(cols.min()), int(rows.min()), int(cols.max()), int(rows.max()))

def width(box):
	return box[2] - box[0] + 1

def height(box):
	return box[3] - box[1] + 1

done = blue_box('open-done.png')
if done is None:
	say(False, '动画结束后没找到窗口（窗口没上屏？）')
	sys.exit(1)
fw, fh = width(done), height(done)
say(fw > 200 and fh > 200, '窗口上屏了：动画结束后 %dx%d' % (fw, fh))

tiny = blue_box('open-tiny.png')
if tiny is None:
	say(False, '入场刚开始那帧没抓到窗口')
else:
	r = 100.0 * width(tiny) / fw
	say(width(tiny) < 0.85 * fw,
		'入场是从小的弹出来的：起步那帧宽 %d = 基准的 %.0f%%（< 85%%）' % (width(tiny), r))

over = blue_box('open-over.png')
if over is None:
	say(False, '入场中段那帧没抓到窗口')
else:
	r = 100.0 * width(over) / fw
	say(width(over) > 1.02 * fw and height(over) > 1.02 * fh,
		'入场中段**比最终还大**（过冲）：%dx%d = 基准的 %.1f%% / %.1f%%'
		% (width(over), height(over), r, 100.0 * height(over) / fh))

low = blue_box('close-low.png')
back = blue_box('close-back.png')
if low is None or back is None:
	say(False, '关闭动画里没抓到窗口（%s）' % ('、'.join(n for n, b in
		(('低点', low), ('回弹点', back)) if b is None)))
else:
	say(width(low) < 0.55 * fw,
		'关闭缩过头：低点那帧宽 %d = 基准的 %.0f%%（< 55%%）'
		% (width(low), 100.0 * width(low) / fw))
	say(width(back) > width(low) + 5,
		'关闭**回弹**：后段那帧宽 %d > 低点 %d（+%d px，涨回来了）'
		% (width(back), width(low), width(back) - width(low)))

sys.exit(0 if ok else 1)
PY
rc=$?
[ $rc -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-bounce.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
