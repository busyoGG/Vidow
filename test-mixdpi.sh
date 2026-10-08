#!/bin/bash
# 混合 DPI 下**拖**窗口跨屏：拖着的那一段就得按对面那块屏的倍率烘。
#
# 用户报的：窗口从 1080p 拖到 4K 上「先模糊后清晰」。根因是装饰/着色器的离屏缓冲
# 按「窗口**属于**哪块屏的工作区」（view->output）算倍率，而布局归属要到**松手**才
# 换 —— 拖到 4K 屏上的那一段，缓冲还是 1 倍（实测 2076x1236），整块窗口被放大到 2 倍
# 屏上，于是一直糊着，松手（归属换了）才清楚。现在按「窗口**看得见**的那些屏里最大
# 的倍率」烘（view.c 的 view_scale）。
#
# 判据（headless：HEADLESS-1 1920x1080@1x，HEADLESS-2 3840x2160@2x —— 两块屏的
# 逻辑尺寸都是 1920x1080，正是 1080p + 4K@2x 那套）：
#   1) 起点：窗口在 1 倍屏上，离屏缓冲是 1 倍大小（< 2500 宽）
#   2) 拖到 2 倍屏上、**还没松手**：归属还没变（IPC 里 output 仍是 1 倍屏）——
#      这正是要测的那一段
#   3) 同一时刻离屏缓冲已经按 2 倍烘（出现 > 3000 宽的 fx: target）
#   4) 松手：归属换成 2 倍屏，缓冲还是 2 倍
#   5) 再拖回 1 倍屏：缓冲回到 1 倍（不是一直按 2 倍烘）
#   6) 日志里没有 ERROR / assert
#
# 用法：./test-mixdpi.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-mixdpi-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	stop_clients
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

python3 -c "import gi" 2>/dev/null || { echo "test-mixdpi.sh: 需要 python3-gobject"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "auto"
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1920x1080" }
vidow.output{ name = "HEADLESS-2", scale = 2, mode = "3840x2160" }
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "test-mixdpi.sh: vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
sleep 1

exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }

# 窗口一行：标题 / 在哪块屏 / 位置尺寸
wins() {
	./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    g = w["float_geometry"] if w["floating"] else w["geometry"]
    print("%s %s %d %d %d %d" % (w["title"], w["output"], g["x"], g["y"],
                                 g["width"], g["height"]))'
}
field() { wins | awk -v t="$1" '$1 == t { print $'"$2"' }'; }

# 这块屏的布局盒（逻辑坐标）
out_box() { # <输出名> -> "x y w h"
	./build/vidowmsg outputs | python3 -c '
import sys, json
for o in json.load(sys.stdin):
    if o["name"] == sys.argv[1]:
        b = o["layout"]
        print(b["x"], b["y"], b["width"], b["height"])
        break
' "$1"
}

# 这一段日志里最大的 fx: target 宽度（0 = 没有）
target_max() { # <起始行>
	tail -n +"$1" "$LOG" | grep -ao "fx: target [0-9]*x" |
		awk '{ if ($3 + 0 > m) m = $3 + 0 } END { print m + 0 }'
}
target_last() {
	grep -ao "fx: target [0-9]*x" "$LOG" | tail -1 | awk '{ print $3 + 0 }'
}

python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do ./build/vidowmsg windows 2>/dev/null | grep -q '"w1"' && break; sleep 0.25; done
sleep 1
./build/vidowmsg set-column-width 1 >/dev/null
./build/vidowmsg move-window-to-output HEADLESS-1 --title w1 >/dev/null
sleep 3

read -r O1X O1Y O1W O1H <<< "$(out_box HEADLESS-1)"
read -r O2X O2Y O2W O2H <<< "$(out_box HEADLESS-2)"
read -r WX WY WW WH <<< "$(wins | awk -v t=w1 '$1 == t { print $3, $4, $5, $6 }')"
OUT0=$(field w1 2)
echo "    布局：HEADLESS-1 $O1X,$O1Y ${O1W}x${O1H}；HEADLESS-2 $O2X,$O2Y ${O2W}x${O2H}"
echo "    窗口 w1：$OUT0 上 $WX,$WY ${WW}x${WH}"

if [ "$OUT0" != "HEADLESS-1" ]; then
	fail "起点不对：w1 应该在 HEADLESS-1 上（现在 $OUT0）"
fi
BASE=$(target_last)
if [ "$BASE" -gt 0 ] && [ "$BASE" -lt 2500 ]; then
	pass "起点：1 倍屏上最后一块离屏缓冲 $BASE 宽（1 倍）"
else
	fail "起点：1 倍屏上的离屏缓冲是 $BASE 宽，不像 1 倍（期望 < 2500）"
fi

# 拖到 2 倍屏中间：起点取窗口中心，落点取另一块屏的中心
CX=$((WX + WW / 2)); CY=$((WY + WH / 2))
TX=$((O2X + O2W / 2)); TY=$((O2Y + O2H / 2))
mark "拖到 2 倍屏上（不松手）"
MARK=$(wc -l < "$LOG")
act "pointer-motion $CX $CY" 0.3
act "pointer-button left press mod" 0.4
for i in $(seq 1 8); do
	act "pointer-motion $((CX + (TX - CX) * i / 8)) $((CY + (TY - CY) * i / 8))" 0.15
done
sleep 1.2

OUT_DRAG=$(field w1 2)
if [ "$OUT_DRAG" = "HEADLESS-1" ]; then
	pass "拖拽中归属还没变（IPC 里仍是 $OUT_DRAG）—— 测的就是这一段"
else
	fail "拖拽中归属已经变成 $OUT_DRAG（这个测试要的是「归属没变、人已经在对面屏上」）"
fi
DRAG_MAX=$(target_max "$((MARK + 1))")
if [ "$DRAG_MAX" -gt 3000 ]; then
	pass "拖拽中离屏缓冲已经按 2 倍烘（fx: target 最大 ${DRAG_MAX} 宽）"
else
	fail "拖拽中离屏缓冲还是 $DRAG_MAX 宽（1 倍）—— 2 倍屏上整块窗口是放大出来的，糊"
fi

mark "松手：归属换成 2 倍屏"
act "pointer-button left release mod" 0.8
sleep 1.5
OUT1=$(field w1 2)
if [ "$OUT1" = "HEADLESS-2" ]; then
	pass "松手后归属换成 HEADLESS-2"
else
	fail "松手后归属是 $OUT1（期望 HEADLESS-2）"
fi
AFTER=$(target_last)
if [ "$AFTER" -gt 3000 ]; then
	pass "松手后离屏缓冲还是 2 倍（$AFTER 宽）"
else
	fail "松手后离屏缓冲变成 $AFTER 宽（期望 2 倍）"
fi

mark "再拖回 1 倍屏"
read -r WX WY WW WH <<< "$(wins | awk -v t=w1 '$1 == t { print $3, $4, $5, $6 }')"
CX=$((WX + WW / 2)); CY=$((WY + WH / 2))
TX=$((O1X + O1W / 2)); TY=$((O1Y + O1H / 2))
act "pointer-motion $CX $CY" 0.3
act "pointer-button left press mod" 0.4
for i in $(seq 1 8); do
	act "pointer-motion $((CX + (TX - CX) * i / 8)) $((CY + (TY - CY) * i / 8))" 0.15
done
act "pointer-button left release mod" 0.8
sleep 1.5
OUT2=$(field w1 2)
BACK=$(target_last)
if [ "$OUT2" = "HEADLESS-1" ] && [ "$BACK" -lt 2500 ]; then
	pass "拖回 1 倍屏后离屏缓冲回到 1 倍（$BACK 宽，归属 $OUT2）"
else
	fail "拖回 1 倍屏后：归属 $OUT2、缓冲 $BACK 宽（期望 HEADLESS-1 + < 2500）"
fi

mark "收尾"
if grep -aqE "ERROR|assert|abort|SIGSEGV" "$LOG"; then
	fail "日志里有 ERROR / assert / 崩溃：$(grep -aE 'ERROR|assert|abort|SIGSEGV' "$LOG" | head -2 | tr '\n' ' ')"
else
	pass "日志里没有 ERROR / assert / 崩溃"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "test-mixdpi.sh: 全部通过"
else
	echo "test-mixdpi.sh: 有失败项（运行目录 $RUNDIR，VIDOW_TEST_KEEP=1 可保留）"
fi
exit "$FAILED"
