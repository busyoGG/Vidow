#!/bin/bash
# 模糊半径**跟着降采样级数扩展**：级数每加一级，半径（原始像素）翻倍。
#
# 靶子：tools/wall.py 当壁纸（渐变 + 硬边方块），直接从模糊金字塔 dump（半分辨
# 率）里量梯度。硬边的最大梯度 ∝ 1/σ，所以「级数越大、梯度越小」就是「半径随
# 级数扩展」的可测量版本。老代码是 dual filter 的 add 版本，每级只涨 1.5 倍而
# 不是 2 倍，级数一多就几乎看不出变化。
#
# 判据（2 / 4 / 6 级各跑一轮）：
#   1) 最大梯度随级数单调下降
#   2) 2 级 vs 6 级差 2 倍以上（半径扩展确实是几何级的）
#   3) 4 级 vs 6 级还有明显差别（不是早早饱和）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurlevel-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
	# vidow 的调试转储写死在 /tmp（vidow-blur-N.raw 那些）：它们是我们自己丢的，
	# 收尾一起带走，别在 /tmp 里堆着。
	rm -f /tmp/vidow-blur-*.raw /tmp/vidow-blur-*.txt \
		/tmp/vidow-bd-*.raw /tmp/vidow-bd-*.txt \
		/tmp/vidow-lblur-*.raw /tmp/vidow-lblur-*.txt \
		/tmp/vidow-lbd-*.raw /tmp/vidow-lbd-*.txt
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-blurlevel.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=info VIDOW_BLUR_DUMP=4
export DISPLAY= WAYLAND_DISPLAY=

BIN=${VIDOW_BIN:-./build/vidow}

run_once() {
	local levels="$1"
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
vidow.output{ name = "HEADLESS-1", scale = 1 }
blur_enabled = true
blur_levels = $levels
blur_offset = 1.5
blur_alpha_min = 0.0
window_opacity = 0.5
CFG
	rm -f /tmp/vidow-blur-*.raw /tmp/vidow-blur-*.txt
	setsid "$BIN" > "$LOG" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	grep -aq "vidow ready" "$LOG" || { echo "test-blurlevel.sh: vidow 没起来（levels=$levels）"; exit 1; }
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

	setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall-$levels.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do grep -aq "layer map wall" "$LOG" && break; sleep 0.25; done
	sleep 1.2

	local last=$(ls /tmp/vidow-blur-*.raw 2>/dev/null |
		sed 's/.*-\([0-9]*\)\.raw/\1/' | sort -n | tail -1)
	if [ -n "${last:-}" ]; then
		cp "/tmp/vidow-blur-$last.raw" "$RUNDIR/blur-$levels.raw"
		cp "/tmp/vidow-blur-$last.txt" "$RUNDIR/blur-$levels.txt"
	else
		fail "levels=$levels：没拿到模糊金字塔快照"
	fi

	kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	CLIENT_PIDS=""
	VIDOW_PID=""
	sleep 0.3
}

for L in 2 4 6; do run_once "$L"; done

python3 - "$RUNDIR" <<'PY'
import sys
import numpy as np

rundir = sys.argv[1]
ok = True

def say(good, msg):
	global ok
	print(('  PASS  ' if good else '  FAIL  ') + msg)
	ok = ok and good

def grads(levels):
	with open('%s/blur-%d.txt' % (rundir, levels)) as f:
		w, h, _fmt = f.read().split()
	w, h = int(w), int(h)
	a = np.fromfile('%s/blur-%d.raw' % (rundir, levels), dtype=np.uint8)
	a = a[:w * h * 4].reshape(h, w, 4).astype(np.float32) / 255.0
	if float(a[..., 3].mean()) < 0.5:
		return None
	# 内存顺序 B,G,R
	g = 0.114 * a[..., 0] + 0.587 * a[..., 1] + 0.299 * a[..., 2]
	mx = np.abs(np.diff(g, axis=1)).max()
	mean = float(np.abs(np.diff(g, axis=1)).mean())
	return mx, mean

vals = {}
for L in (2, 4, 6):
	v = grads(L)
	if v is None:
		say(False, 'levels=%d：金字塔是空的（alpha < 0.5）' % L)
		sys.exit(1)
	vals[L] = v
	print('  levels=%d  最大梯度=%.4f  平均梯度=%.4f' % (L, v[0], v[1]))

g2, g4, g6 = vals[2][0], vals[4][0], vals[6][0]
say(g2 > g4 > g6, '最大梯度随级数单调下降（%.4f > %.4f > %.4f）' % (g2, g4, g6))
say(g2 > 2.0 * g6, '2 级 vs 6 级差了 2 倍以上（%.1f 倍）' % (g2 / g6))
say(g4 > 1.5 * g6, '4 级 vs 6 级还有明显差别（%.1f 倍）' % (g4 / g6))
sys.exit(0 if ok else 1)
PY
rc=$?
[ $rc -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-blurlevel.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
