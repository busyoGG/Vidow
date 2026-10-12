#!/bin/bash
# 会被模糊的 layer surface **自己**不能进自己的底衬。
#
# 靶子：tools/dot.py 当壁纸（纯平 0.12 灰），tools/bpanel.py 一块 bottom 层 50%
# 红面板。bottom 层就在底衬抓取的那几棵树里（background / bottom / windows），
# 于是面板自己被抓进底衬 —— 而那份底衬正是它背后模糊的来源，模糊里就会浮出一个
# 「自己的影子」（半透明的地方透出自己被糊过一遍的样子）。
#
# 判据：
#   1) 面板确实在屏上（截屏里那块是红的）—— 不然这条测试等于没测
#   2) 底衬 dump 里那块区域**没有**红像素（面板被抓的时候被藏起来了）
#   3) 底衬别处不空（壁纸在）—— 不然 2 是白给的
#
# 用法：./test-layerghost.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layerghost-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

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
	{ echo "test-layerghost.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }
command -v grim >/dev/null || { echo "test-layerghost.sh: 需要 grim"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=info VIDOW_BLUR_DUMP=8
export DISPLAY= WAYLAND_DISPLAY=

BIN=${VIDOW_BIN:-./build/vidow}
PW=400 PH=200

cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1 }
blur_enabled = true
blur_levels = 4
blur_offset = 1.5
blur_alpha_min = 0.15
window_opacity = 0.5
CFG

rm -f /tmp/vidow-bd-*.raw /tmp/vidow-bd-*.txt /tmp/vidow-blur-*.raw /tmp/vidow-blur-*.txt
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "test-layerghost.sh: vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

setsid nohup python3 ./tools/dot.py 640 20 > "$RUNDIR/dot.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map dot" "$LOG" && break; sleep 0.25; done
sleep 0.5
setsid nohup python3 ./tools/bpanel.py $PW $PH > "$RUNDIR/bpanel.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map bpanel" "$LOG" && break; sleep 0.25; done
sleep 1.5
grep -aq "layer map bpanel" "$LOG" || fail "bottom 层面板没上屏"
grim -o HEADLESS-1 "$RUNDIR/shot.png" 2>/dev/null || fail "grim 失败"

last=$(ls /tmp/vidow-bd-*.raw 2>/dev/null | sed 's/.*-\([0-9]*\)\.raw/\1/' | sort -n | tail -1)
if [ -n "${last:-}" ]; then
	cp "/tmp/vidow-bd-$last.raw" "$RUNDIR/bd.raw"
	cp "/tmp/vidow-bd-$last.txt" "$RUNDIR/bd.txt"
else
	fail "没拿到底衬快照（VIDOW_BLUR_DUMP 没生效？）"
fi

python3 - "$RUNDIR" "$PW" "$PH" <<'PY'
import sys
import numpy as np
from PIL import Image

rundir, pw, ph = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
ok = True

def say(good, msg):
	global ok
	print(('  PASS  ' if good else '  FAIL  ') + msg)
	ok = ok and good

with open(rundir + '/bd.txt') as f:
	w, h, _fmt = f.read().split()
w, h = int(w), int(h)
# 内存顺序 B,G,R,A（dump 出来是 AR24）
bd = np.fromfile(rundir + '/bd.raw', dtype=np.uint8)[:w * h * 4]
bd = bd.reshape(h, w, 4).astype(np.float32) / 255.0
shot = np.asarray(Image.open(rundir + '/shot.png').convert('RGB')).astype(np.float32)[:, :, ::-1] / 255.0

# 面板区域（右下角，往里收 20px 躲开边缘）
x0, y0 = w - pw + 20, h - ph + 20
x1, y1 = w - 20, h - 20
scr = shot[y0:y1, x0:x1]
bdr = bd[y0:y1, x0:x1]

def redish(a):
	return (a[..., 2] > 0.25) & (a[..., 2] - a[..., 1] > 0.15) & (a[..., 2] - a[..., 0] > 0.15)

scr_red = float(redish(scr).mean())
bd_red = float(redish(bdr).mean())
say(scr_red > 0.9, 'bottom 层面板在屏上（红像素 %.1f%%）' % (scr_red * 100))
say(bd_red < 0.001, '底衬里没有面板自己（红像素 %.2f%%）' % (bd_red * 100))
# 别处（面板上方那半屏）壁纸得在：0.12 灰 + 那个亮点
up = bd[:h // 2, :, :3]
say(float(up.mean()) > 0.05 and float(up.mean()) < 0.9,
	'底衬别处是壁纸（均值 %.3f）' % float(up.mean()))
sys.exit(0 if ok else 1)
PY
rc=$?
[ $rc -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-layerghost.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
