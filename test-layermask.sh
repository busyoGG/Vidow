#!/bin/bash
# 透明度剔除：模糊**不能漏进元素自己透明的部分**（niri 的 ignore-alpha 那件事）。
#
# 靶子：tools/dot.py 当壁纸（0.12 灰 + 两个亮点），tools/padpanel.py 一块 400x300 的
# top 层面板压在左上角 —— 它自己向外留 40px **完全不画**的边距：
#   亮点 A 在 (320,20)：落在面板的透明边距里
#   亮点 B 在 (320,150)：落在面板中间那个圆角半透明块底下
# 面板的模糊是垫在它下面的一整块矩形，所以没剔除时 A 也会被糊开（一圈透明模糊）。
#
# 判据：
#   1) 面板上屏 + 拿到截屏
#   2) blur_alpha_min = 0.15：A 还是原样的亮点（峰值 ≥ 0.85）= 透明边距不糊
#   3) blur_alpha_min = 0.0（热改配置）：A 被糊开（峰值 ≤ 0.5）—— 这条是差异对照，
#      证明 2) 是剔除的功劳而不是「模糊本来就没画上去」
#   4) 0.15 时 B 仍然是糊的（峰值 ≤ 0.35）= 元素有内容的地方（块 alpha 0.5 > 0.15）
#      照糊，没被一刀切掉
#
# 用法：./test-layermask.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layermask-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
CFG="$CFGDIR/.config/vidow/config.lua"
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
	{ echo "test-layermask.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }
command -v grim >/dev/null || { echo "test-layermask.sh: 需要 grim"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY=

BIN=${VIDOW_BIN:-./build/vidow}
PW=400 PH=300

cat > "$CFG" <<'CFGEOF'
outputs = "HEADLESS-1:1"
blur_enabled = true
blur_levels = 4
blur_offset = 2
blur_alpha_min = 0.15
window_opacity = 1.0
CFGEOF

setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "test-layermask.sh: vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

setsid nohup python3 ./tools/dot.py 320 20 320 150 > "$RUNDIR/dot.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map dot" "$LOG" && break; sleep 0.25; done
sleep 0.6

setsid nohup python3 ./tools/padpanel.py $PW $PH > "$RUNDIR/panel.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map shape" "$LOG" && break; sleep 0.25; done
sleep 1.5

grep -aq "layer map shape" "$LOG" || fail "top 层面板上屏失败"
grim -o HEADLESS-1 "$RUNDIR/cull.png" 2>/dev/null || fail "grim 失败（cull）"

# 热改配置：阈值归零 = 不剔。dumps 之外没有别的缓存键挡路（layer 那套的缓存键里
# 带着 alpha_min），HUP 之后会重算重画。
sed -i 's/^blur_alpha_min = .*/blur_alpha_min = 0.0/' "$CFG"
kill -HUP "$(pgrep -x vidow | head -1)" 2>/dev/null
sleep 1.5
grim -o HEADLESS-1 "$RUNDIR/nocull.png" 2>/dev/null || fail "grim 失败（nocull）"

python3 - "$RUNDIR" <<'PY'
import sys
import numpy as np
from PIL import Image

rundir = sys.argv[1]
ok = True

def say(good, msg):
	global ok
	print(('  PASS  ' if good else '  FAIL  ') + msg)
	ok = ok and good

def luma(img):
	a = np.asarray(img.convert('RGB')).astype(np.float32) / 255.0
	return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]

def peak(l, cx, cy, r=20):
	return float(l[cy - r:cy + r, cx - r:cx + r].max())

try:
	cull = luma(Image.open(rundir + '/cull.png'))
	nocull = luma(Image.open(rundir + '/nocull.png'))
except Exception as e:
	say(False, '截屏读不出来：%s' % e)
	sys.exit(1)

# 亮点 A 落在面板 40px 的透明边距里（面板在左上角，边距是 [0,400)x[0,300) 的外圈）
a_cull = peak(cull, 320, 20)
a_nocull = peak(nocull, 320, 20)
say(a_cull > 0.85, '剔除开（0.15）：透明边距里的亮点还是原样的（峰值 %.3f）' % a_cull)
say(a_nocull < 0.5, '剔除关（0.0）：同一个亮点被糊开了（峰值 %.3f）—— 上面那条是剔除的功劳'
	% a_nocull)

# 亮点 B 在中间的圆角半透明块底下（块 alpha 0.5 > 0.15）：该糊还得糊
b = peak(cull, 320, 150)
say(b < 0.35, '元素有内容的地方（块 alpha 0.5）照糊：块底下的亮点峰值 %.3f' % b)

sys.exit(0 if ok else 1)
PY
rc=$?
[ $rc -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-layermask.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
