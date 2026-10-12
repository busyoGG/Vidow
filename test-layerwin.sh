#!/bin/bash
# top / overlay 层的模糊**必须含窗口**（niri 的语义：layer 抓的是它盖住的内容）。
#
# 靶子：tools/dot.py 当壁纸（纯平 0.12 灰 + 一个亮点），tools/bluewin.py 一个铺满
# 工作区的纯蓝窗口，tools/panel.py 一块 400x200 的 top 层 50% 面板压在窗口左上角（缩放布局里一个窗
# 口只占左边一条，所以面板也得贴左边才压得住）。
# 面板本身是 50% 深色：它背后要是窗口的模糊，那块截屏就明显偏蓝；要是壁纸的模糊，
# 就是灰的（老代码那种，B-R ≈ 0.02）。
#
# 判据：
#   1) 窗口在屏上（截屏里那块是蓝的）—— 不然这条测试等于没测
#   2) 面板那块（截屏）明显偏蓝（B-R 大）
#   3) 面板那块底衬抓取（vidow-lbd-*，抓的就是它盖住的那块区域）里大片是蓝的
#   4) 同一块区域在**共享**底衬里仍是壁纸的灰（窗口在那份里是被藏起来的）——
#      证明区域抓取 = 共享底衬（壁纸）那块 + 窗口，而不是整块换了一张
#
# 用法：./test-layerwin.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layerwin-test
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
	{ echo "test-layerwin.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }
command -v grim >/dev/null || { echo "test-layerwin.sh: 需要 grim"; exit 1; }

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
window_opacity = 1.0
CFG

rm -f /tmp/vidow-bd-*.raw /tmp/vidow-bd-*.txt /tmp/vidow-blur-*.raw /tmp/vidow-blur-*.txt
rm -f /tmp/vidow-lbd-*.raw /tmp/vidow-lbd-*.txt /tmp/vidow-lblur-*.raw /tmp/vidow-lblur-*.txt
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "test-layerwin.sh: vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

setsid nohup python3 ./tools/dot.py 640 20 > "$RUNDIR/dot.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map dot" "$LOG" && break; sleep 0.25; done
sleep 0.5

setsid nohup python3 ./tools/bluewin.py > "$RUNDIR/bluewin.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "view map" "$LOG" && break; sleep 0.25; done
sleep 0.8

setsid nohup python3 ./tools/panel.py $PW $PH tl > "$RUNDIR/panel.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map testpanel" "$LOG" && break; sleep 0.25; done
sleep 1.5

grep -aq "layer map testpanel" "$LOG" || fail "top 层面板没上屏"
grim -o HEADLESS-1 "$RUNDIR/shot.png" 2>/dev/null || fail "grim 失败"

# 面板那块区域的底衬抓取（最后一次就是压在窗口上那一版）
last=$(ls /tmp/vidow-lbd-*.raw 2>/dev/null | sed 's/.*-\([0-9]*\)\.raw/\1/' | sort -n | tail -1)
if [ -n "${last:-}" ]; then
	cp "/tmp/vidow-lbd-$last.raw" "$RUNDIR/lbd.raw"
	cp "/tmp/vidow-lbd-$last.txt" "$RUNDIR/lbd.txt"
else
	fail "没拿到 layer 区域底衬的 dump（VIDOW_BLUR_DUMP 没生效？）"
fi
last=$(ls /tmp/vidow-bd-*.raw 2>/dev/null | sed 's/.*-\([0-9]*\)\.raw/\1/' | sort -n | tail -1)
if [ -n "${last:-}" ]; then
	cp "/tmp/vidow-bd-$last.raw" "$RUNDIR/bd.raw"
	cp "/tmp/vidow-bd-$last.txt" "$RUNDIR/bd.txt"
else
	fail "没拿到共享底衬的 dump"
fi

python3 - "$RUNDIR" "$PW" "$PH" <<'PY'
import os
import sys
import numpy as np
from PIL import Image

rundir, pw, ph = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
ok = True

def say(good, msg):
	global ok
	print(('  PASS  ' if good else '  FAIL  ') + msg)
	ok = ok and good

def load_raw(path, txt):
	with open(txt) as f:
		w, h, _fmt = f.read().split()
	w, h = int(w), int(h)
	a = np.fromfile(path, dtype=np.uint8)[:w * h * 4]
	# 内存顺序 B,G,R,A
	return a.reshape(h, w, 4).astype(np.float32) / 255.0, w, h

# 截屏：PIL 给的是 RGB
shot = np.asarray(Image.open(rundir + '/shot.png').convert('RGB')).astype(np.float32) / 255.0
sh, sw = shot.shape[0], shot.shape[1]

# 面板：左上角 400x200，往里收 20px 躲开边缘
px0, py0 = 20, 20
px1, py1 = pw - 20, ph - 20
panel = shot[py0:py1, px0:px1]
say(float((panel[..., 2] - panel[..., 0]).mean()) > 0.25,
	'截屏里面板那块偏蓝（B-R 均值 %.3f）= 透出来的是窗口的模糊'
	% float((panel[..., 2] - panel[..., 0]).mean()))

# 窗口本体在屏上：整个屏去掉面板那块之后，蓝得还是一片一片的
mask = np.ones((sh, sw), dtype=bool)
mask[py0:py1, px0:px1] = False
blue_frac = float((((shot[..., 2] - shot[..., 0]) > 0.3) & mask).mean())
say(blue_frac > 0.2, '纯蓝窗口在屏上、没被面板盖住的那部分还在（蓝像素 %.1f%%）'
	% (blue_frac * 100))

# 面板那块区域的底衬抓取：整块就是面板盖住的内容
if not os.path.exists(rundir + '/lbd.raw'):
	say(False, '没有 layer 区域底衬的 dump（老版本没有这条路）')
else:
	lbd, lw, lh = load_raw(rundir + '/lbd.raw', rundir + '/lbd.txt')
	inner = lbd[20:lh - 20, 20:lw - 20]
	bfrac = float(((inner[..., 0] - inner[..., 2]) > 0.3).mean())
	say(bfrac > 0.8, '区域底衬里大片是窗口的蓝（蓝像素 %.1f%%）' % (bfrac * 100))

# 同一块区域在共享底衬里（窗口被藏起来）该是壁纸的灰
bd, bw, bh = load_raw(rundir + '/bd.raw', rundir + '/bd.txt')
if (bw, bh) == (sw, sh):
	sub = bd[py0:py1, px0:px1, :3]
	diff = float(np.abs(sub[..., 0] - sub[..., 2]).mean())
	say(diff < 0.05, '共享底衬里同一块是壁纸的灰（B-R 均值 %.3f）' % diff)
else:
	say(False, '共享底衬尺寸 %dx%d 跟屏 %dx%d 对不上' % (bw, bh, sw, sh))
sys.exit(0 if ok else 1)
PY
rc=$?
[ $rc -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-layerwin.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
