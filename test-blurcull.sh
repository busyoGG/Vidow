#!/bin/bash
# blur_alpha_min = 「低于这个不透明度的底衬不糊」，硬阈值，只在最后一趟做一次。
#
# 靶子：tools/soft.py 一整屏均匀半透明的背景层。底衬里没别的东西，所以模糊金字塔
# （up[0]）dump 出来的 alpha 就等于那个客户端的 alpha —— 阈值管不管用一眼看得出。
#
# 判据：
#   1) 阈值关着（0）：0.45 的底衬原样糊（alpha ≈ 0.45）
#   2) 阈值 0.5 > 0.45：底衬低于阈值 ⇒ **完全不糊**（alpha ≈ 0）
#      —— 软过渡那版（smoothstep(min*0.5, min)）在这儿还剩 90%（实测 0.4039），
#      看着就是「这个键没用」；撒在每一趟抽样里那版则是 0.02 上下（「越糊越透明」）
#   3) 阈值 0.5 < 0.9：高于阈值的底衬**一点都不许衰减**（alpha ≈ 0.9）
#      —— 挡的是「顺手乘个系数」那种实现（那会把 0.9 也压下去）
#
# 用法：./test-blurcull.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
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
command -v grim >/dev/null || { echo "test-blurcull.sh: 需要 grim"; exit 1; }

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

# 逼一帧出来（截图那套有看门狗，会把渲染推起来），然后等这份新的金字塔 dump
trigger() {
	for _ in $(seq 1 8); do
		timeout 15 grim -o HEADLESS-1 "$RUNDIR/trigger.png" > /dev/null 2>&1
		ls /tmp/vidow-blur-*.raw > /dev/null 2>&1 && return 0
		sleep 0.4
	done
	return 1
}

grab() { # $1 = tag
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

start_soft() { # $1 = alpha
	setsid nohup python3 ./tools/soft.py "$1" > "$RUNDIR/soft-$1.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do grep -aq "layer map soft" "$LOG" && break; sleep 0.25; done
}

# 1) 阈值关着：0.45 的底衬原样
start_soft 0.45
grep -aq "layer map soft" "$LOG" || fail "半透明背景层没上屏"
sleep 1.2
grab off

# 2) 阈值 0.5：底衬 0.45 低于阈值 ⇒ 不糊
# 先把旧 dump 清掉再改配置：改完那次重建就是我们要量的那一版
rm -f /tmp/vidow-blur-*.raw /tmp/vidow-blur-*.txt
write_cfg 0.5
kill -HUP "$VIDOW_PID"
sleep 0.5
trigger && grab ign || fail "阈值 0.5 时没等到新的金字塔 dump"
sleep 0.5

# 3) 底衬换成 0.9（阈值还是 0.5）⇒ 高于阈值，一点都不许衰减
pkill -f "tools/soft.py" 2>/dev/null
sleep 0.6
rm -f /tmp/vidow-blur-*.raw /tmp/vidow-blur-*.txt
start_soft 0.9
sleep 0.6
trigger && grab keep || fail "底衬 0.9 时没等到新的金字塔 dump"

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
	try:
		with open('%s/blur-%s.txt' % (rundir, tag)) as f:
			w, h, fmt = f.read().split()
	except OSError:
		return None
	w, h = int(w), int(h)
	a = np.fromfile('%s/blur-%s.raw' % (rundir, tag), dtype=np.uint8)
	if a.size < w * h * 4:
		return None
	a = a[:w * h * 4].reshape(h, w, 4).astype(np.float32) / 255.0
	# 取中间那块，躲开边缘（边缘上本来就有 alpha 渐变）
	return a[h // 4:h * 3 // 4, w // 4:w * 3 // 4]

off, ign, keep = load('off'), load('ign'), load('keep')
if off is None or ign is None or keep is None:
	say(False, '模糊金字塔 dump 读不出来（off=%s ign=%s keep=%s）'
		% (off is not None, ign is not None, keep is not None))
	sys.exit(1)

a_off = float(off[..., 3].mean())
a_ign = float(ign[..., 3].mean())
a_keep = float(keep[..., 3].mean())
c_ign = float(ign[..., :3].mean())
c_keep = float(keep[..., :3].mean())
print('  alpha: 阈值关=%0.4f  0.45 底衬+阈值 0.5=%0.4f  0.9 底衬+阈值 0.5=%0.4f'
	% (a_off, a_ign, a_keep))
print('  颜色:  0.45 底衬+阈值 0.5 的 rgb 均值 %0.4f（预乘，该跟 alpha 一起归零）'
	% c_ign)
say(abs(a_off - 0.45) < 0.05, '阈值关着时 alpha 就是底衬的 0.45（实测 %.4f）' % a_off)
say(a_ign < 0.05, '低于阈值的底衬完全不糊（实测 %.4f，软过渡那版 0.4039）' % a_ign)
say(c_ign < 0.05, '颜色跟着一起归零，没留下「半透明黑」（rgb 均值 %.4f）' % c_ign)
say(a_keep > 0.85, '高于阈值的底衬一点都不衰减（实测 %.4f，0.9 底衬）' % a_keep)
say(abs(c_keep - 0.9 * 0.6) < 0.06,
	'颜色也没被顺手压过（rgb 均值 %.4f，期望 0.54）' % c_keep)
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
