#!/bin/bash
# 两块屏 + 重叠窗口：私有模糊区域的金字塔缓冲**每帧都不许重建**。
#
# 靶子：两块 headless 屏（HEADLESS-1 1 倍 / HEADLESS-2 2 倍），壁纸 + 半透明浮窗。
# 互相压着的窗口走「私有区域底衬」那条路（每帧重抓它背后那一块 + 重跑金字塔链），
# 它那块金字塔是**挂在窗口身上**的（view->fx.blur_set）。
#
# 修之前：vidow_blur_update() 的窗口循环不筛屏 —— 每块屏都按**自己的** scale 和
# 尺寸给同一个窗口算区域（1 倍屏把 400x300 的窗口算成 768x640，2 倍屏算成
# 1152x1024），两趟每帧互相把对方的缓冲丢掉重建：一次十几块 GBM 缓冲（最大那块
# 2304x2560 = 23 MB），拖窗口时每秒几千次分配 —— 屏幕上一顿一顿（用户报的拖拽
# 卡顿）。顺带也修了「区域和贴图归哪块屏」：以前最后写进 fx 的那趟说了算。
#
# 判据：
#   1) 前提：被压着的窗口真的走了私有区域那条路（blur: own region wB）
#   2) 移动窗口那段时间里金字塔缓冲一次都没重建（fx: target 行数 = 0）
#   3) 同一个窗口只有一种区域尺寸（修之前一块屏一个尺寸，两种轮流）
#   4) 区域尺寸跟它所在那块屏的倍率对得上（2 倍屏 → 区域宽 > 2 × 窗口逻辑宽）
#   5) vidow 全程活着，日志里没有 ERROR
#
# 用法：./test-blur2out.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blur2out-test
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

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-blur2out.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1920x1080" }
vidow.output{ name = "HEADLESS-2", scale = 2, mode = "1920x1080" }
blur_enabled = true
blur_all = true
blur_levels = 4
blur_offset = 1.5
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

mark "两块屏：一块 1 倍、一块 2 倍"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.8
grep -ac "output HEADLESS-" "$LOG" >/dev/null
if [ "$(./build/vidowmsg outputs | python3 -c 'import sys,json; print(len(json.load(sys.stdin)))')" = "2" ]; then
	pass "两块屏都在（$(./build/vidowmsg outputs | python3 -c '
import sys, json
print(" ".join("%s@%gx" % (o["name"], o["scale"]) for o in json.load(sys.stdin)))')）"
else
	fail "只看到一块屏（需要 WLR_HEADLESS_OUTPUTS=2）"
fi

setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5

# 半透明薄纱（400x300，不可改尺寸）：模糊从它背后透出来，压着的那个才有私有区域
spawn_sheet() {
	setsid nohup python3 ./tools/sheet.py "$1" "$2" "$3" "$4" 0.5 \
		> "$RUNDIR/$1.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do
		./build/vidowmsg windows 2>/dev/null | grep -q "\"$1\"" && break
		sleep 0.25
	done
}

win_json() { # <标题> -> 一行 JSON（没有就空）
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
w = next((w for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), None)
print(json.dumps(w) if w else "")' "$1"
}

to_output() { # <标题> <输出>：挪过去 + 浮动 + 摆到指定逻辑坐标
	local t="$1" out="$2" x="$3" y="$4"
	./build/vidowmsg move-window-to-output "$out" --title "$t" >/dev/null 2>&1
	sleep 0.5
	for _ in $(seq 1 12); do
		./build/vidowmsg toggle-floating --title "$t" >/dev/null 2>&1
		[ "$(win_json "$t" | python3 -c '
import sys, json
w = sys.stdin.read().strip()
print(json.loads(w)["floating"] if w else "?")')" = "True" ] && break
		sleep 0.3
	done
	./build/vidowmsg set-window-position --title "$t" "$x" "$y" >/dev/null 2>&1
	sleep 0.6
}

spawn_sheet wA 0.0 0.0 0.0
spawn_sheet wB 0.0 0.0 0.0
spawn_sheet wC 0.0 0.0 0.0

# wA 独占 HEADLESS-1（每次动它，那块屏就会渲染一帧）；wB / wC 在 HEADLESS-2 上
# 互相压着 —— 被压着的那个才有私有区域。
to_output wA HEADLESS-1 100 100
to_output wB HEADLESS-2 200 200
to_output wC HEADLESS-2 320 300
sleep 0.6

OUTB=$(win_json wB | python3 -c 'import sys,json; w=sys.stdin.read().strip(); print(json.loads(w)["output"] if w else "?")')
GEOB=$(win_json wB | python3 -c 'import sys,json
w = sys.stdin.read().strip()
g = json.loads(w)["float_geometry"] if w else None
print("%d %d" % (g["width"], g["height"]) if g else "? ?")')
LOGB=$(./build/vidowmsg outputs | python3 -c '
import sys, json
print(" ".join("%s %g" % (o["name"], o["scale"]) for o in json.load(sys.stdin)))')
SCALEB=$(echo "$LOGB" | tr ' ' '\n' | awk -v o="$OUTB" '$0 == o { getline; print; exit }')
if [ "$OUTB" = "HEADLESS-2" ]; then
	pass "wB 在 $OUTB 上（倍率 $SCALEB），wC 压着它：$GEOB"
else
	fail "wB 没挪到 HEADLESS-2（在 $OUTB）—— 后面的倍率判据会不准"
fi

mark "动窗口：金字塔缓冲必须复用"

# 动 wA（1 倍屏）让那块屏也渲染一帧 —— 两趟 blur_update 都跑了，才能看出
# 「同一个窗口被两块屏轮流重建」。wB 每步挪 130 逻辑像素（>128 的网格），
# 保证区域原点真的换格子（否则走的是「形状没变」的快路，测不出东西）。
#
# 挪动范围**留在 HEADLESS-2 自己那块屏里**（逻辑 960 宽，窗口 412 宽）：整块挪到
# 隔壁那块屏上会换掉烘窗口的倍率（离屏缓冲按「看得见的那块屏里最大的倍率」烘，
# 见 view.c 的 view_scale），那一下重建缓冲是**该发生的**，不在这个测试的判据里
# —— 跨屏换倍率由 test-mixdpi.sh 管。
N1=$(wc -l < "$LOG")
for i in $(seq 0 3); do
	./build/vidowmsg set-window-position --title wB $((40 + i * 130)) 200 >/dev/null 2>&1
	./build/vidowmsg set-window-position --title wA $((100 + i * 20)) 100 >/dev/null 2>&1
	sleep 0.12
done
sleep 0.5
N2=$(wc -l < "$LOG")
sed -n "$((N1 + 1)),${N2}p" "$LOG" > "$RUNDIR/segment.log"

ALLOCS=$(grep -c "fx: target" "$RUNDIR/segment.log" || true)
REBUILDS=$(grep -c "blur: own region wB" "$RUNDIR/segment.log" || true)
SIZES=$(grep -o "blur: own region wB [0-9]*x[0-9]*" "$RUNDIR/segment.log" |
	awk '{ print $5 }' | sort -u | tr '\n' ' ')
echo "    wB：重抓区域 $REBUILDS 次，重建缓冲 $ALLOCS 块，区域尺寸 [$SIZES]"

if [ "$REBUILDS" -ge 3 ]; then
	pass "前提成立：wB 走的是私有区域那条路（重抓 $REBUILDS 次）"
else
	fail "wB 没走私有区域那条路（只重抓 $REBUILDS 次）—— 测的不是这条路径"
fi

if [ "$ALLOCS" -eq 0 ]; then
	pass "移动窗口期间金字塔缓冲一块都没重建（$REBUILDS 次重抓，0 次分配）"
else
	fail "移动窗口期间重建了 $ALLOCS 块缓冲（$REBUILDS 次重抓）—— 每帧都在 GBM 分配"
fi

NDISTINCT=$(echo "$SIZES" | wc -w)
if [ "$NDISTINCT" = "1" ]; then
	pass "同一个窗口只有一种区域尺寸（$SIZES）"
else
	fail "同一个窗口有 $NDISTINCT 种区域尺寸（$SIZES）—— 两块屏各算了一份"
fi

W=$(echo "$SIZES" | awk '{ print $1 }' | sed 's/x.*//')
WW=$(echo "$GEOB" | awk '{ print $1 }')
if [ -n "$W" ] && [ -n "$WW" ] && [ "$W" != "0" ]; then
	if [ "$SCALEB" = "2" ] && [ "$W" -gt $((WW * 2)) ]; then
		pass "区域按 wB 那块屏的倍率算（${W}px 宽 > 2 × ${WW}px 逻辑宽）"
	elif [ "$SCALEB" = "1" ] && [ "$W" -lt $((WW * 2)) ]; then
		pass "区域按 wB 那块屏的倍率算（${W}px 宽 < 2 × ${WW}px 逻辑宽，1 倍屏）"
	else
		fail "区域尺寸和 wB 所在屏的倍率对不上（区域 ${W}px，窗口 ${WW}px 逻辑，屏倍率 $SCALEB）"
	fi
else
	fail "没拿到区域尺寸"
fi

if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "vidow 全程活着"
else
	fail "vidow 挂了"
fi
if grep -aq "\[ERROR\]" "$LOG"; then
	fail "日志里有 ERROR：$(grep -a "\[ERROR\]" "$LOG" | head -1)"
else
	pass "日志里没有 ERROR"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-blur2out.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
