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
#   5) 整块**不透明**的状态栏不垫模糊：底衬一次都不抓、kawase 一趟都不跑，而半透明的
#      照旧每帧重抓（见 fx.c 的 layer_blur_visible —— 模糊被 content.a 乘成 0）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layerblur-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

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

mark "整块不透明的状态栏：不垫模糊（看不见的模糊不算）"

# 靶子：状态栏 + 一个每帧换色的窗口压在它那块区域里（top 层在窗口上面，所以窗口的
# 每一帧都落在状态栏的底衬里）。半透明的状态栏每帧都得重抓那一小块底衬 + 跑一遍
# kawase；整块不透明的那趟一个像素都透不出来（装饰着色器里模糊被 content.a 乘成 0），
# 该一次都不跑 —— 判据在 fx.c 的 layer_blur_visible。
#
# 实测（headless 1280x720@60，blur_levels=3，5s 稳定窗口）：
#   半透明（alpha 0.5）：layer region 重抓 305 次，kawase 链条 310 趟
#   不透明（alpha 1.0）：layer region 重抓 0 次，kawase 链条 0 趟
run_bar_alpha() { # <alpha> <标签>
	local alpha="$1" tag="$2"
	cat > "$CFGDIR/.config/vidow/config.lua" <<EOF
vidow.output{ name = "HEADLESS-1", scale = 1 }
blur_enabled = true
blur_levels = 3
window_open_ms = 0
window_move_ms = 0
EOF
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	grep -aq "vidow ready" "$LOG" || { echo "test-layerblur.sh: vidow 没起来"; exit 1; }
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)

	setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do grep -aq "layer map wall" "$LOG" && break; sleep 0.25; done
	sleep 0.8
	VIDOW_TEST_BAR_ALPHA="$alpha" setsid nohup python3 ./tools/bar.py \
		> "$RUNDIR/bar-$tag.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do grep -aq "layer map testbar" "$LOG" && break; sleep 0.25; done
	sleep 1.5
	setsid nohup python3 ./tools/heartbeat.py wbeat > "$RUNDIR/beat-$tag.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 2.5
	./build/vidowmsg toggle-floating --title wbeat >/dev/null 2>&1
	sleep 0.4
	./build/vidowmsg set-window-size --title wbeat 500 400 >/dev/null 2>&1
	sleep 0.4
	./build/vidowmsg set-window-position --title wbeat 200 0 >/dev/null 2>&1
	sleep 1.5

	local OFF=$(wc -c < "$LOG")
	sleep 5
	tail -c +$((OFF + 1)) "$LOG" | grep -ac "blur: layer region" > "$RUNDIR/region-$tag.txt"
	tail -c +$((OFF + 1)) "$LOG" | grep -a "渲染统计" > "$RUNDIR/stats-$tag.txt"
	grep -ac ^frame "$RUNDIR/beat-$tag.log" > "$RUNDIR/beats-$tag.txt"
	grim -o HEADLESS-1 "$RUNDIR/bar-$tag.png" 2>/dev/null
	kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.5
	kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	CLIENT_PIDS=""
	VIDOW_PID=""
	sleep 0.3
}

# 这一节要数「抓了几次 layer region」（DEBUG 日志）和 kawase 链条数（渲染统计）
export VIDOW_LOG=debug VIDOW_RENDER_STATS=1
run_bar_alpha 0.5 trans
run_bar_alpha 1.0 opaque

python3 - "$RUNDIR" <<'PY'
import sys
import numpy as np
from PIL import Image

rundir = sys.argv[1]
ok = True


def num(path):
    try:
        return [int(v) for v in open(path).read().split()]
    except Exception:
        return []


def counts(tag):
    region = (num(f"{rundir}/region-{tag}.txt") or [0])[0]
    frames = chain = 0
    try:
        for line in open(f"{rundir}/stats-{tag}.txt"):
            if "渲染统计:" in line:
                frames += int(line.split("渲染统计:")[1].split("帧")[0].strip())
            if "chain " in line:
                chain += int(line.split("chain ")[1].split(")")[0])
    except Exception:
        pass
    return region, chain, frames


beats = num(f"{rundir}/beats-trans.txt")
region_t, chain_t, frames_t = counts("trans")
region_o, chain_o, _ = counts("opaque")

if beats and beats[0] > 100:
    print(f"  PASS  底下那个窗口一直在出帧（{beats[0]} 帧，状态栏的底衬真的在变）")
else:
    print(f"  FAIL  底下那个窗口没出几帧（{beats}）—— 底衬没在变，下面两条判据不算数")
    ok = False
print(f"  半透明状态栏：5s 内 layer region 重抓 {region_t} 次、kawase 链条 {chain_t} 趟（{frames_t} 帧）")
print(f"  不透明状态栏：5s 内 layer region 重抓 {region_o} 次、kawase 链条 {chain_o} 趟")

if region_t >= 100 and chain_t >= 100:
    print(f"  PASS  半透明的状态栏每帧都在重抓底衬（{region_t} 次 / {chain_t} 趟）")
else:
    print(f"  FAIL  半透明状态栏没在每帧重抓（{region_t} 次 / {chain_t} 趟）—— 前提没复现")
    ok = False
if region_o == 0 and chain_o == 0:
    print("  PASS  整块不透明的状态栏一次都没抓、一趟都没跑（模糊一个像素都透不出来）")
else:
    print(f"  FAIL  不透明的状态栏还在抓底衬（{region_o} 次 / {chain_o} 趟）—— 那笔模糊白算")
    ok = False


def bar_row(path):
    a = np.asarray(Image.open(path).convert('RGB')).astype(np.float32)
    return a[8:32, 60:1220]  # 状态栏中间那一条（避开上下边界）


def spread(row):
    # 沿 x 的空间起伏（逐通道标准差取最大）——「底下透出来多少」就看这个。
    # 别用 row.std()：那会把三个通道之间的差（0.1/0.1/0.15 本身就有 6.1）算进去。
    return float(row.reshape(-1, 3).std(axis=0).max())


try:
    t = bar_row(f"{rundir}/bar-trans.png")
    o = bar_row(f"{rundir}/bar-opaque.png")
    print(f"  状态栏那条：半透明 平均色 {t.mean(axis=(0, 1)).round(1)} 起伏 {spread(t):5.2f}｜"
          f"不透明 平均色 {o.mean(axis=(0, 1)).round(1)} 起伏 {spread(o):5.2f}")
    # 半透明：0.5 * 栏 + 0.5 * 背后的糊，底下壁纸 + 窗口 → 那条沿 x 有强起伏
    if spread(t) > 5.0:
        print(f"  PASS  半透明状态栏透出底下的糊（沿 x 起伏 {spread(t):.2f}）")
    else:
        print(f"  FAIL  半透明状态栏那条是死的（沿 x 起伏 {spread(t):.2f}）")
        ok = False
    # 不透明：那一整条就是纯色 0.1/0.1/0.15（25.5/25.5/38.25），底下什么都不该透出来
    want = np.array([25.5, 25.5, 38.25], dtype=np.float32)
    if spread(o) < 1.0 and float(np.abs(o.mean(axis=(0, 1)) - want).max()) < 3.0:
        print(f"  PASS  不透明状态栏那条是纯色（沿 x 起伏 {spread(o):.2f}，"
              f"平均色 {o.mean(axis=(0, 1)).round(1)}）")
    else:
        print(f"  FAIL  不透明状态栏那条不纯（沿 x 起伏 {spread(o):.2f}，"
              f"平均色 {o.mean(axis=(0, 1)).round(1)}）")
        ok = False
except Exception as e:
    print(f"  FAIL  读截图失败：{e}")
    ok = False

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
