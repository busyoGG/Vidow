#!/bin/bash
# layer surface 背后的模糊（状态栏 / 面板那层）。
#
# 靶子：tools/wall.py 画壁纸（渐变 + 硬边方块），tools/bar.py 画一块 50% 透明的
# top 层状态栏。状态栏里画出来的东西 = 0.5 * 黑 + 0.5 * 背后的模糊 —— 背后的方块
# 糊没糊、糊到什么程度，直接量状态栏那 40px 里的梯度就知道。
#
# 判据（不截屏判分，只看像素梯度）：
#   1) 开了模糊：状态栏里的方块边缘比壁纸本身软得多（最大梯度不到壁纸的 1/4）
#   2) blur_levels 真的有效：4 级比 2 级更糊（最大梯度、平均梯度都更低）
#   3) 状态栏还在画（那段不是纯黑，也不是壁纸原样）
#   4) 状态栏的 exclusive zone 仍然生效：窗口排在栏下面，不被压住
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layerblur-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi" 2>/dev/null || { echo "test-layerblur.sh: 需要 python3-gobject"; exit 1; }
python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-layerblur.sh: 需要 gtk-layer-shell"; exit 1; }
command -v grim >/dev/null || { echo "test-layerblur.sh: 需要 grim"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY=

# 跑一轮：<levels> <输出 png>
run_once() {
	local levels="$1" out="$2"
	cat > "$CFGDIR/.config/vidow/config.lua" <<EOF
vidow.output{ name = "HEADLESS-1", scale = 1 }
blur_enabled = true
blur_levels = $levels
window_opacity = 0.5
EOF
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	if ! grep -aq "vidow ready" "$LOG"; then
		echo "test-layerblur.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)

	setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do grep -aq "layer map wall" "$LOG" && break; sleep 0.25; done
	sleep 1
	setsid nohup python3 ./tools/bar.py > "$RUNDIR/bar.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do grep -aq "layer map testbar" "$LOG" && break; sleep 0.25; done
	sleep 1
	setsid nohup ./tools/pattern.py w > "$RUNDIR/p.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 2
	grep -aq "layer map testbar" "$LOG" || fail "levels=$levels：状态栏没上屏"
	grim -o HEADLESS-1 "$out" 2>/dev/null || fail "levels=$levels：grim 失败"
	cp "$LOG" "$RUNDIR/log-$levels.txt"
	./build/vidowmsg state > "$RUNDIR/state-$levels.json" 2>/dev/null
	kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.5
	kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	CLIENT_PIDS=""
	VIDOW_PID=""
	sleep 0.3
}

mark "跑两轮：blur_levels=2 和 =4，各截一张图"
run_once 2 "$RUNDIR/shot-2.png"
run_once 4 "$RUNDIR/shot-4.png"

python3 - "$RUNDIR" <<'PY'
import json, sys
import numpy as np
from PIL import Image

rundir = sys.argv[1]

def load(p):
    return np.asarray(Image.open(p).convert('RGB')).astype(np.float32)

def grad(a):
    g = a.mean(axis=2)
    return float(np.abs(np.diff(g, axis=1)).max()), float(np.abs(np.diff(g, axis=1)).mean())

shots = {n: load(f"{rundir}/shot-{n}.png") for n in (2, 4)}
# 状态栏：y 4..36（高 40，避开上下各 4px 边界），x 60..1220（方块区）
# 参照：栏下面那一段壁纸（没被任何东西盖住）
bars = {n: shots[n][4:36, 60:1220] for n in shots}
wall = shots[4][60:92, 60:1220]
wm, wmean = grad(wall)
bm = {n: grad(bars[n]) for n in bars}

print(f"  壁纸（未模糊）      最大梯度 {wm:7.2f}  平均梯度 {wmean:6.3f}")
for n in (2, 4):
    print(f"  levels={n} 状态栏内   最大梯度 {bm[n][0]:7.2f}  平均梯度 {bm[n][1]:6.3f}  平均色 {bars[n].mean(axis=(0,1)).round(1)}")

ok = True
# 1) 状态栏里确实糊了：方块边缘比壁纸本身软得多
if bm[2][0] < wm / 4.0:
    print(f"  PASS  状态栏背后是糊的（最大梯度 {bm[2][0]:.2f} < 壁纸 {wm:.2f} 的 1/4）")
else:
    print(f"  FAIL  状态栏背后没糊（最大梯度 {bm[2][0]:.2f}，壁纸 {wm:.2f}）"); ok = False
# 2) levels 4 比 2 更糊。判据看**最大梯度**：4 级应该只有 2 级的一半左右，分离度高
#    （实测 5.33 -> 2.67，跟二进制无关）。平均梯度只当方向参考 —— 它还会被窗口自带
#    的那圈半透明边框影响（透明度剔除之后窗口自己的软化边距不再糊，而那一圈正好
#    落在状态栏的底衬抓取里），拿 0.9 这种紧比值当判据会很脆。
if bm[4][0] < bm[2][0] * 0.75 and bm[4][1] <= bm[2][1]:
    print(f"  PASS  blur_levels=4 比 =2 更糊（最大 {bm[4][0]:.2f} < {bm[2][0]:.2f}，平均 {bm[4][1]:.3f} < {bm[2][1]:.3f}）")
else:
    print(f"  FAIL  levels 改了没区别（最大 {bm[2][0]:.2f} vs {bm[4][0]:.2f}，平均 {bm[2][1]:.3f} vs {bm[4][1]:.3f}）"); ok = False
# 3) 状态栏那段不是纯黑（漏画 / 没建节点会是全 0 方差）
if bars[2].std() > 2.0 and bars[2].mean() > 15:
    print(f"  PASS  状态栏还在画（标准差 {bars[2].std():.1f}，平均色 {bars[2].mean():.1f}）")
else:
    print(f"  FAIL  状态栏那段不对（标准差 {bars[2].std():.1f}，平均色 {bars[2].mean():.1f}）"); ok = False
# 4) exclusive zone 生效：窗口排在状态栏下面
try:
    # vidowmsg state（不带 --raw）打印的就是 result 那一层
    st = json.load(open(f"{rundir}/state-2.json"))
    wins = [w for w in st["windows"] if w["title"] == "w"]
    y = wins[0]["geometry"]["y"] if wins else -1
    if y >= 40:
        print(f"  PASS  窗口排在状态栏下面（y={y} ≥ 40）")
    else:
        print(f"  FAIL  窗口被状态栏压住了（y={y}）"); ok = False
except Exception as e:
    print(f"  FAIL  读 state 失败：{e}"); ok = False

sys.exit(0 if ok else 1)
PY
[ $? = 0 ] || FAILED=1

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-layerblur.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
