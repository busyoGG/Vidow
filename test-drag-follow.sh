#!/bin/bash
# 拖动跟手：浮动窗口必须**就贴在光标下面**，不许落在光标后面。
#
# 修之前：指针每动一下，place() 都给这个窗口起一个 190ms 的位移补间
# （window_move_ms）。补间每帧被重起一次，而弹性缓动在 t≈0 那一段几乎不动 ——
# 窗口于是**一直落后**光标。用户那份 dbg 日志里量出来的数：拖 273px/s 时中位
# 滞后 199px、最大 387px，起拖的头 125ms 窗口干脆没动。平铺窗口的拖拽（drag.c）
# 本来就是「只动场景节点、布局一行都不重排」，浮动窗口没理由不跟手。
#
# 怎么量「窗口画在哪儿」：靶子是 tools/pattern.py（左上角纯红，背景没有壁纸 =
# 纯黑），红色像素的包围盒左上角就是客户区左上角。它相对窗口盒的偏移在**静止态**
# 量一次当基准（标题栏 + 边框那几像素），再拿拖动中那一帧去比 —— 差多少像素，
# 窗口就落后光标多少像素。
#
# 判据：
#   1) 拖拽真的起来了（grab move: w1）
#   2) 拖拽期间一条位移补间都没有（move tween w1 = 0 行）
#   3) 拖到一半截屏：窗口画在哪儿 == IPC 报的几何（基准偏移，误差 ≤ 4px）
#   4) 松手后落点仍然对（跟手没把落点搞坏）
#   5) vidow 全程活着
#
# 用法：./test-drag-follow.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-dragfollow-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

for tool in grim python3; do
	command -v "$tool" >/dev/null || { echo "test-drag-follow.sh: 需要 $tool"; exit 1; }
done

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 壁纸故意不要：背景纯黑，红色像素只可能来自靶子窗口。
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
-- 指针手势（拖动 / 改列宽 / 滚轮换列）现在是键位：合成器不内置，测试也得自己绑。
vidow.bind("Mod+mouse-left", "interactive-move")
vidow.bind("Mod+mouse-right", "interactive-resize")
vidow.bind("Mod+wheel-up", "focus-column left")
vidow.bind("Mod+wheel-down", "focus-column right")
scale = "1"
gaps = 10
window_move_ms = 190
window_open_ms = 0
window_close_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }

# 客户区（红块）左上角在屏幕上的位置：红色像素包围盒的左上角
red_bbox() { # <png> -> "x y"
	python3 - "$1" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB')).astype(np.int16)
m = (a[..., 0] > 200) & (a[..., 1] < 60) & (a[..., 2] < 60)
if not m.any():
    print("? ?")
else:
    ys, xs = np.nonzero(m)
    print("%d %d" % (xs.min(), ys.min()))
PY
}

geom_of() { # <标题> -> "x y"（浮动就用浮动几何）
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
w = next((w for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), None)
if w is None:
    print("? ?")
else:
    g = w.get("float_geometry") or w.get("geometry")
    print("%d %d" % (g["x"], g["y"]))' "$1"
}
win_geom() { geom_of w1; }

shot() { grim -o HEADLESS-1 "$1" 2>/dev/null; }

setsid nohup python3 ./tools/pattern.py w1 > "$RUNDIR/w1.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do
	./build/vidowmsg windows 2>/dev/null | grep -q '"w1"' && break
	sleep 0.25
done
sleep 0.8
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.5
./build/vidowmsg set-window-position --title w1 200 200 >/dev/null
sleep 0.8

mark "静止态：量一次「客户区相对窗口盒」的偏移"

shot "$RUNDIR/rest.png"
read -r RX RY <<< "$(red_bbox "$RUNDIR/rest.png")"
read -r GX GY <<< "$(win_geom)"
if [ "$RX" = "?" ] || [ "$GX" = "?" ]; then
	fail "静止态没找到靶子（红块 $RX,$RY，窗口 $GX,$GY）"
	echo; echo "有判据没过（见上面的清单）"; exit 1
fi
OFF_X=$((RX - GX)); OFF_Y=$((RY - GY))
echo "    静止：窗口盒 $GX,$GY，客户区左上角 $RX,$RY → 固定偏移 $OFF_X,$OFF_Y"
if [ "$OFF_X" -ge 0 ] && [ "$OFF_Y" -ge 0 ]; then
	pass "基准偏移量出来了（客户区相对窗口盒 +$OFF_X,+$OFF_Y）"
else
	fail "客户区跑到窗口盒左上角之外了（偏移 $OFF_X,$OFF_Y）—— 基准不可信"
fi

mark "拖到一半：窗口必须已经在光标底下"

# 抓点取窗口正中；按住之后往右拖 300 逻辑像素
act "pointer-motion $((GX + 200)) $((GY + 150))"
act "pointer-button left press mod" 0.4
if grep -aq "grab move: w1" "$LOG"; then
	pass "拖拽起来了（grab move: w1）"
else
	fail "拖拽没起来（没有 grab move 日志）"
fi

N1=$(wc -l < "$LOG")
for i in $(seq 1 8); do
	act "pointer-motion $((GX + 200 + i * 40)) $((GY + 150))" 0.12
done
sleep 0.5
N2=$(wc -l < "$LOG")
read -r MGX MGY <<< "$(win_geom)"
if [ "$MGX" = "?" ]; then
	fail "拖动中问不到窗口几何"
	echo; echo "有判据没过（见上面的清单）"; exit 1
fi
sed -n "$((N1 + 1)),${N2}p" "$LOG" > "$RUNDIR/segment.log"

# 补间日志里就写着「画在哪儿 -> 该在哪儿」：两者的差就是滞后。没有补间 = 没有滞后。
TWEENS=$(grep -c "move tween w1" "$RUNDIR/segment.log" || true)
LAG=$(grep "move tween w1" "$RUNDIR/segment.log" | python3 -c '
import sys, re
worst = 0.0
for line in sys.stdin:
    m = re.search(r"move tween w1: (-?\d+),(-?\d+) -> (-?\d+),(-?\d+)", line)
    if m:
        dx, dy, tx, ty = (int(m.group(i)) for i in (1, 2, 3, 4))
        worst = max(worst, ((tx - dx) ** 2 + (ty - dy) ** 2) ** 0.5)
print("%.0f" % worst)')
echo "    拖动期间：窗口盒 $MGX,$MGY，位移补间 $TWEENS 条，画在哪儿 vs 该在哪儿最大差 ${LAG}px"

if [ "$TWEENS" -eq 0 ]; then
	pass "拖动期间没有位移补间（$TWEENS 条 move tween w1）"
else
	fail "拖动期间起了 $TWEENS 条位移补间 —— 位置不是跟着光标走的"
fi

WANT_X=$((GX + 320))
if [ "$MGX" -ge $((WANT_X - 6)) ] && [ "$MGX" -le $((WANT_X + 6)) ]; then
	pass "拖了 300 像素，窗口盒跟着走了 300（$GX → $MGX）"
else
	fail "窗口盒没跟上光标：$GX → $MGX（期望 ≈$WANT_X）"
fi

act "pointer-button left release mod" 0.8
read -r FX FY <<< "$(win_geom)"
if [ "$FX" -ge $((WANT_X - 6)) ] && [ "$FX" -le $((WANT_X + 6)) ]; then
	pass "松手后落点没跑（x=$FX，期望 ≈$WANT_X）"
else
	fail "松手后落点跑了：x=$FX（期望 ≈$WANT_X）"
fi
shot "$RUNDIR/after.png"
read -r AX AY <<< "$(red_bbox "$RUNDIR/after.png")"
if [ "$AX" = "?" ] || [ "$FX" = "?" ]; then
	fail "松手后没找到靶子（红块 $AX,$AY，窗口 $FX,$FY）"
elif [ "$((AX - FX - OFF_X))" -ge -4 ] && [ "$((AX - FX - OFF_X))" -le 4 ]; then
	pass "松手后窗口和客户区对齐（偏移回到基准）"
else
	fail "松手后偏移不对：$((AX - FX - OFF_X))（基准 $OFF_X）"
fi

mark "松手之后指针交还给客户端（拖完第一下点击不许点到别处）"

# 拖拽期间每一次移动都被 grab 吃掉了，客户端手里那份指针位置还是**起拖之前**的。
# 窗口跟着光标挪了 D 像素之后，客户端按旧位置算出来的局部坐标就偏了 D 像素 ——
# 用户报的「拖完点不着，鼠标移出去再移回来就好了」就是它（移出去/移回来会触发
# enter，位置才刷新）。这条判据用 X11 客户端当探针：只有 X11 窗口会打「x11: 指针」
# 那行日志（vidow_pointer_motion 里），拿它看「松手之后有没有补一次位置」。
XP_DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)
setsid nohup env DISPLAY="$XP_DISPLAY" ./build/xpopup popup normal 300 400 400 300 \
	> "$RUNDIR/xp.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do
	./build/vidowmsg windows 2>/dev/null | grep -q '"xp-popup"' && break
	sleep 0.25
done
./build/vidowmsg toggle-floating --title xp-popup >/dev/null 2>&1
sleep 0.6
read -r XX XY <<< "$(geom_of xp-popup)"
if [ "$XX" = "?" ]; then
	fail "X11 探针窗口没起来（xpopup）"
else
	pass "X11 探针窗口在 $XX,$XY"
	act "pointer-motion $((XX + 150)) $((XY + 150))" 0.5
	if grep -aq "x11: 指针" "$LOG"; then
		pass "X11 客户端收得到指针位置（转发这条路是通的）"
	else
		fail "X11 客户端一条指针位置都没收到"
	fi
	N3=$(wc -l < "$LOG")
	act "pointer-button left press mod" 0.3
	for i in 1 2 3 4; do
		act "pointer-motion $((XX + 150 + i * 50)) $((XY + 150))" 0.12
	done
	act "pointer-button left release mod" 0.5
	N4=$(wc -l < "$LOG")
	HAND=$(sed -n "$((N3 + 1)),${N4}p" "$LOG" | grep -c "x11: 指针" || true)
	if [ "$HAND" -ge 1 ]; then
		pass "松手之后客户端立刻拿到新的指针位置（$HAND 条 x11: 指针）"
	else
		fail "松手之后一条指针位置都没补给客户端 —— 第一下点击会偏掉整段拖拽距离"
	fi
fi

exec 9>&-
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "vidow 全程活着"
else
	fail "vidow 挂了"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-drag-follow.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
