#!/bin/bash
# 透明度剔除**只在最后一趟做一次**，不能撒在每一趟的抽样里。
#
# 靶子：tools/soft.py 一整屏均匀 alpha=0.45 的背景层（底衬里没别的东西，
# 所以 dump 出来的 alpha 就等于 0.45）。把 blur_alpha_min 设成 0.5（阈值在
# 0.45 上面），剔除就该只是「稍微压一点」，而不是把整块糊没 —— 撒在每一趟里
# 的话 alpha 会逐趟相乘，0.45 一路掉到 0.02 左右，颜色也一起没了（「越糊越透
# 明」的真身）。
#
# 判据：
#   1) 阈值关着（0）时，模糊金字塔的 alpha ≈ 0.45
#   2) 阈值 0.5 时仍然是同一量级（> 0.35）—— 老代码这儿是 ~0.02
#   3) 两次差得不多（剔除没有把整块抹掉）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurcull-test
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
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-blurcull.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=info VIDOW_BLUR_DUMP=8
export DISPLAY= WAYLAND_DISPLAY=

BIN=${VIDOW_BIN:-./build/vidow}

write_cfg() {
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
outputs = "HEADLESS-1:1"
blur_enabled = true
blur_levels = 4
blur_offset = 1.5
blur_alpha_min = $1
window_opacity = 0.5
CFG
}

# 拿最新一份模糊金字塔 dump，存到 $2
grab() {
	local last=$(ls /tmp/vidow-blur-*.raw 2>/dev/null |
		sed 's/.*-\([0-9]*\)\.raw/\1/' | sort -n | tail -1)
	if [ -z "${last:-}" ]; then
		fail "$1：没拿到模糊金字塔快照"
		return 1
	fi
	cp "/tmp/vidow-blur-$last.raw" "$RUNDIR/blur-$1.raw"
	cp "/tmp/vidow-blur-$last.txt" "$RUNDIR/blur-$1.txt"
}

write_cfg 0.0
rm -f /tmp/vidow-blur-*.raw /tmp/vidow-blur-*.txt
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "test-blurcull.sh: vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

setsid nohup python3 ./tools/soft.py 0.45 > "$RUNDIR/soft.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map soft" "$LOG" && break; sleep 0.25; done
grep -aq "layer map soft" "$LOG" || fail "半透明背景层没上屏"
sleep 1.2
grab off

# 换阈值：SIGHUP 重载配置，金字塔会按新参数重建
write_cfg 0.5
kill -HUP "$VIDOW_PID"
sleep 0.6
# 触发一次重绘（重建发生在 blur_update 里），再等它落定
pkill -HUP -f "tools/soft.py" 2>/dev/null
sleep 1.4
grab on

python3 - "$RUNDIR" <<'PY'
import sys
import numpy as np

rundir = sys.argv[1]
ok = True

def say(good, msg):
	global ok
	print(('  PASS  ' if good else '  FAIL  ') + msg)
	ok = ok and good

def load(tag):
	with open('%s/blur-%s.txt' % (rundir, tag)) as f:
		w, h, fmt = f.read().split()
	w, h = int(w), int(h)
	a = np.fromfile('%s/blur-%s.raw' % (rundir, tag), dtype=np.uint8)
	if a.size < w * h * 4:
		return None, fmt
	a = a[:w * h * 4].reshape(h, w, 4).astype(np.float32) / 255.0
	# 取中间那块，躲开边缘（边缘上本来就有 alpha 渐变）
	return a[h // 4:h * 3 // 4, w // 4:w * 3 // 4], fmt

off, _ = load('off')
on, _ = load('on')
if off is None or on is None:
	say(False, '模糊金字塔 dump 读不出来')
	sys.exit(1)

a_off = float(off[..., 3].mean())
a_on = float(on[..., 3].mean())
print('  alpha: 阈值关=%0.4f  阈值 0.5=%0.4f（底色 rgb %.3f/%.3f）' %
	(a_off, a_on, float(off[..., 2].mean()), float(on[..., 2].mean())))
say(abs(a_off - 0.45) < 0.05, '阈值关着时 alpha 就是 0.45（实测 %.4f）' % a_off)
say(a_on > 0.35, '阈值 0.5 时没被抹掉（实测 %.4f，老代码 ~0.02）' % a_on)
say(a_on > 0.7 * a_off, '剔除只是压一点，没抹掉整块（%.4f vs %.4f）' % (a_on, a_off))
sys.exit(0 if ok else 1)
PY
rc=$?
[ $rc -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-blurcull.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
