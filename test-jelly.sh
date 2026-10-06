#!/bin/bash
# 拖窗口的果冻效果（config 的 jelly / jelly_strength，见 jelly.c）。
#
# 判据（headless 1280x720 一块屏，截图用 grim 当裁判 —— 弯没弯由像素说了算）：
#   1) 拖着走的时候窗口是**弯的**：左边缘在窗口顶部与底部的 x 差 ≥ 5px
#      （抓点在标题栏 = 顶部，往右拖 → 顶部在前、底部拖后腿）
#   2) 松手晃完 → 又直了，而且**窗口还在**（不是变全黑 / 消失 —— 交接时装饰节点
#      的透明度没还回去就踩过这个）
#   3) jelly = false：同一个拖法，边缘是直的（差值 ≤ 2px），日志里没有 jelly
#   4) jelly_strength = 0：还走果冻那条路（日志里有起手），但不弯
#   5) 改大小（Mod+右键）不弯：那条路每帧换源缓冲尺寸，网格对不上
#   6) 平铺窗口（没浮动）拖拽也弯
#   7) 窗口最后落在光标松手的地方（果冻不该把位置语义搞坏）
#   8) 日志里没有 assert / abort
#
# 用法：./test-jelly.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py / wall.py。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-jelly-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*wall.py*|*build/frames*|*/wev*|*grim*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

cleanup() {
	stop_clients
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT
# 被 timeout / Ctrl-C 杀掉时也要收尾：bash 收到 SIGTERM 默认不跑 EXIT 陷阱，
# 直接死掉会留下一串没主的 vidow（它们已经 reparent 到 systemd 了，没人再管）。
# `exit 143` 会让 EXIT 陷阱照常跑。
trap 'exit 143' TERM INT

command -v grim >/dev/null || { echo "test-jelly.sh: 需要 grim"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "test-jelly.sh: 需要 python3-PIL"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-jelly.sh: 先构建"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"

start_vidow() { # <额外配置行>
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
scale = "1"
gaps = 10
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
blur_enabled = false
$1
CFG
	stop_clients
	rm -f "$RUNDIR/actions" "$LOG"
	mkfifo "$RUNDIR/actions"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	for _ in $(seq 1 60); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		echo "vidow 没起来"; tail -5 "$LOG"; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	exec 9> "$RUNDIR/actions"
	sleep 0.4
}

stop_vidow() {
	stop_clients
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.5
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
		VIDOW_PGID=""
	fi
}

act() { echo "$1" >&9; sleep "${2:-0.3}"; }

# 窗口的浮动态 / 位置尺寸
win_geom() { # <标题> -> "x y w h floating"
	./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    if w["title"] != sys.argv[1]:
        continue
    g = w["float_geometry"] if w["floating"] else w["geometry"]
    print(g["x"], g["y"], g["width"], g["height"], "1" if w["floating"] else "0")' "$1"
}

# 左边缘轮廓：窗口顶部 / 中间 / 底部那三行的最左边非黑像素
edge() { # <png> -> "top mid bottom"（没有窗口就打印 none）
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
W, H = im.size
px = im.load()
ys = [y for y in range(H) if any(px[x, y] != (0, 0, 0) for x in range(W))]
if not ys:
    print("none")
    sys.exit()
y0, y1 = min(ys), max(ys)
out = []
for frac in (0.05, 0.5, 0.95):
    y = int(y0 + (y1 - y0) * frac)
    row = [x for x in range(W) if px[x, y] != (0, 0, 0)]
    out.append(row[0] if row else -1)
print("%d %d %d" % (out[0], out[1], out[2]))
PY
}

# 窗口宽（非黑像素的外接盒），用来确认「窗口还在」
win_w() { # <png>
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
W, H = im.size
px = im.load()
xs = [x for y in range(0, H, 4) for x in range(W) if px[x, y] != (0, 0, 0)]
print(0 if not xs else max(xs) - min(xs) + 1)
PY
}

# 从标题栏起拖：往右快速拖 400px，中途截一张，然后松手
drag_and_shot() { # <png> <x> <y> <dx>
	local png=$1 cx=$2 cy=$3 dx=$4
	act "pointer-motion $cx $cy"
	act "pointer-button left press mod"
	local i
	for i in $(seq 1 8); do
		act "pointer-motion $((cx + i * dx / 8)) $cy" 0.02
	done
	grim "$png" 2>/dev/null
	act "pointer-button left release mod" 0.05
}

# ---------------------------------------------------------------- 1) 浮动窗口
mark "浮动窗口：拖着走是弯的，松手又直回来"
start_vidow ""
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
if [ "$FLOAT" = "1" ] && [ "$WW" -gt 200 ]; then
	pass "浮动窗口 w1：$WX,$WY ${WW}x${WH}"
else
	fail "没拿到浮动窗口（$WX $WY $WW $WH float=$FLOAT）"
fi
drag_and_shot "$RUNDIR/drag.png" "$((WX + WW / 2))" "$((WY + 20))" 400
E=$(edge "$RUNDIR/drag.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "拖动中截不到窗口"
else
	pass "拖动中窗口还在（左边缘 $ET/$EM/$EB）"
	if [ $((ET - EB)) -ge 5 ]; then
		pass "拖着走是弯的：顶部比底部靠前 $((ET - EB))px（≥5）"
	else
		fail "没弯：顶部 $ET、底部 $EB（差 $((ET - EB))，要 ≥5）"
	fi
fi
if grep -aq "jelly: w1 起手" "$LOG"; then
	pass "日志记下了果冻起手：$(grep -a 'jelly: w1 起手' "$LOG" | head -1 | sed 's/.*jelly: /jelly: /')"
else
	fail "没有果冻起手日志"
fi

sleep 1.4
grim "$RUNDIR/settled.png" 2>/dev/null
E=$(edge "$RUNDIR/settled.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "松手晃完之后窗口没了（截图全黑）"
else
	if [ $((ET - EB)) -le 2 ] && [ $((EB - ET)) -le 2 ]; then
		pass "晃完又直了：$ET/$EM/$EB（差 $((ET - EB))）"
	else
		fail "晃完还弯着：$ET/$EM/$EB"
	fi
	W=$(win_w "$RUNDIR/settled.png")
	if [ "$W" -gt 200 ]; then
		pass "窗口还在：宽 ${W}px"
	else
		fail "窗口不见了（宽 ${W}px）"
	fi
fi
# 落点：窗口跟着光标走多远就落多远（抓点相对窗口不动，所以位移 == 光标位移）
read -r X1 Y1 W1 H1 F1 <<<"$(win_geom w1)"
WANT=$((WX + 400))
if [ "$X1" -ge $((WANT - 40)) ] && [ "$X1" -le $((WANT + 40)) ]; then
	pass "落在光标松手的地方：x=$X1（期望 ≈$WANT）"
else
	fail "落点不对：x=$X1（期望 ≈$WANT）"
fi
stop_vidow

# ---------------------------------------------------------------- 2) jelly = false
mark "jelly = false：同一个拖法，不弯"
start_vidow "jelly = false"
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
drag_and_shot "$RUNDIR/off.png" "$((WX + WW / 2))" "$((WY + 20))" 400
E=$(edge "$RUNDIR/off.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "jelly=false 时截不到窗口"
elif [ $((ET - EB)) -le 2 ] && [ $((EB - ET)) -le 2 ]; then
	pass "关掉之后边缘是直的：$ET/$EM/$EB（差 $((ET - EB))）"
else
	fail "关掉了还弯：$ET/$EM/$EB（差 $((ET - EB))）"
fi
if grep -aq "jelly: " "$LOG"; then
	fail "关掉了还在起手：$(grep -a 'jelly: ' "$LOG" | head -1)"
else
	pass "关掉之后日志里没有果冻"
fi
stop_vidow

# ---------------------------------------------------------------- 3) 倍率 0
mark "jelly_strength = 0：还走这条路，但不弯"
start_vidow "jelly_strength = 0"
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
drag_and_shot "$RUNDIR/zero.png" "$((WX + WW / 2))" "$((WY + 20))" 400
E=$(edge "$RUNDIR/zero.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "倍率 0 时截不到窗口"
elif [ $((ET - EB)) -le 2 ] && [ $((EB - ET)) -le 2 ]; then
	pass "倍率 0：不弯（$ET/$EM/$EB）"
else
	fail "倍率 0 还弯：$ET/$EM/$EB"
fi
if grep -aq "jelly: w1 起手" "$LOG"; then
	pass "倍率 0 还是走果冻那条路（日志里有起手）"
else
	fail "倍率 0 没走果冻那条路"
fi
stop_vidow

# ---------------------------------------------------------------- 4) 改大小
mark "改大小（Mod+右键）不弯"
start_vidow ""
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
CX=$((WX + WW - 30)); CY=$((WY + WH - 30))
act "pointer-motion $CX $CY"
act "pointer-button right press mod"
for i in 1 2 3 4 5 6; do
	act "pointer-motion $((CX + i * 20)) $((CY + i * 15))" 0.03
done
grim "$RUNDIR/resize.png" 2>/dev/null
act "pointer-button right release mod" 0.1
if grep -aq "jelly: " "$LOG"; then
	fail "改大小也起手了果冻：$(grep -a 'jelly: ' "$LOG" | head -1)"
else
	pass "改大小不弯（日志里没有果冻起手）"
fi
read -r X2 Y2 W2 H2 F2 <<<"$(win_geom w1)"
if [ "$W2" -gt "$WW" ]; then
	pass "窗口确实变大了：${WW} → ${W2}"
else
	fail "改大小没生效：${WW} → ${W2}"
fi
stop_vidow

# ---------------------------------------------------------------- 5) 平铺窗口
mark "平铺窗口（没浮动）拖拽也弯"
start_vidow ""
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
if [ "$FLOAT" = "0" ]; then
	pass "w1 是平铺窗口：$WX,$WY ${WW}x${WH}"
else
	fail "w1 不该是浮动的"
fi
drag_and_shot "$RUNDIR/tile.png" "$((WX + WW / 2))" "$((WY + 20))" 400
E=$(edge "$RUNDIR/tile.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "平铺拖拽时截不到窗口"
elif [ $((ET - EB)) -ge 5 ]; then
	pass "平铺窗口拖着走也是弯的：差 $((ET - EB))px"
else
	fail "平铺窗口没弯：$ET/$EM/$EB（差 $((ET - EB))）"
fi
if grep -aq "jelly: w1 起手" "$LOG"; then
	pass "平铺拖拽也起了果冻"
else
	fail "平铺拖拽没起果冻"
fi
sleep 1.2
grim "$RUNDIR/tile-settled.png" 2>/dev/null
E=$(edge "$RUNDIR/tile-settled.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "平铺窗口松手晃完之后没了"
elif [ $((ET - EB)) -le 2 ] && [ $((EB - ET)) -le 2 ]; then
	pass "平铺窗口晃完也直了：$ET/$EM/$EB"
else
	fail "平铺窗口晃完还弯：$ET/$EM/$EB"
fi
stop_vidow

# ------------------------------------------------- 6) 拖到一半客户端退出
mark "拖到一半客户端退出：不能撞上「节点身上还挂着 destroy 钩子」那条断言"
start_vidow ""
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
CX=$((WX + WW / 2)); CY=$((WY + 20))
act "pointer-motion $CX $CY"
act "pointer-button left press mod"
act "pointer-motion $((CX + 60)) $CY" 0.05
act "pointer-motion $((CX + 140)) $CY" 0.05
if grep -aq "jelly: w1 起手" "$LOG"; then
	pass "拖到一半，果冻正开着"
else
	fail "拖到一半没起果冻（这条测不到东西）"
fi
stop_clients
sleep 0.6
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "客户端退出之后合成器还活着"
else
	fail "客户端退出把合成器带崩了"
fi
if grep -aqE "assert|abort|Assertion" "$LOG"; then
	fail "撞上了断言：$(grep -aE 'assert|abort|Assertion' "$LOG" | head -2)"
else
	pass "日志里没有断言"
fi
act "pointer-button left release mod" 0.1
stop_vidow

# ---------------------------------------------------------------- 7) 日志
mark "日志里没有 assert / abort"
if grep -aqE "assert|abort|Assertion" "$LOG"; then
	fail "日志里有 assert / abort：$(grep -aE 'assert|abort|Assertion' "$LOG" | head -2)"
else
	pass "日志干净"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-jelly.sh: OK"
else
	echo "test-jelly.sh: 有项目没通过"
	exit 1
fi
