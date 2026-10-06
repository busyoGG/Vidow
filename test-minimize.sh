#!/bin/bash
# 最小化：窗口留在原来的列里（列宽 / 上下顺序 / 滚动位置 / 归属工作区全都不用重建），
# 但不占位、不显示、不参与焦点。放回来是瞬时的，而且**位置就是原来那个**。
#
# 判据（headless 1280x720 一块屏 + pattern / frames / xpopup 客户端）：
#   1) IPC minimize-window：vidowmsg 报 minimized=true / visible=false，日志说清楚了；
#      左边那条列收起来之后，右边那条滑到最左边（place 行）
#   2) 最小化当前**聚焦**的窗口：焦点交给邻居（日志 `焦点: a2 -> a1`）
#   3) 同列两个窗口：上面那个收起来，下面那个撑满整列高度（place 高度变了）
#   4) 放回：unminimize-window（不带参数 = 最近收起来的那个）回到原位（place 行一致）
#   5) 键位动作那一套名字也能用：vidowmsg action minimize-window / restore-window
#   6) 整列都最小化：那条列从条带里消失，新窗口落在最左边
#   7) 客户端自己请求（xdg_toplevel.set_minimized）：合成器认，而且**不给它补帧**
#      （没有规则时客户端就此停帧 —— 这正是省电的那一半）
#   8) 规则 force_render 的窗口最小化之后**照样补帧**（录屏 / 预览要的是新画面），
#      日志写明了原因是「它最小化了」
#   9) SIGHUP 重载配置不改最小化状态
#  10) X11：xdotool windowminimize -> 日志 + X 那边 WM_STATE=IconicState；
#      放回之后回到 NormalState（X 客户端自己也知道自己收起来了）
#  11) 日志里没有 assert / abort
#
# 用法：./test-minimize.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 每个测试收尾都按 cmdline 杀 pattern.py / frames，
# 同时跑会把对方的客户端一起杀掉（实测过一次：11 项假失败）。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-minimize-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
VIDOW_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	stop_clients
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -TERM -- "-$pid" 2>/dev/null
	done
	sleep 0.4
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -KILL -- "-$pid" 2>/dev/null
	done
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*build/frames*|*build/xpopup*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

python3 -c "import gi" 2>/dev/null || { echo "test-minimize.sh: 需要 python3-gobject"; exit 1; }
[ -x ./build/frames ] || { echo "test-minimize.sh: 先构建（./build/frames 不在）"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

write_config() { # <额外配置行>
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
scale = "1"
window_open_ms = 60
window_close_ms = 60
window_move_ms = 60
$1
CFG
}

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

start_vidow() { # <xwayland: true|false>
	export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
	write_config "xwayland = $1"
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	VIDOW_PIDS="$VIDOW_PIDS $VIDOW_PGID"
	for _ in $(seq 1 60); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		echo "vidow 没起来"; tail -5 "$LOG"; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	sleep 0.5
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

# 一个窗口的 JSON 字段（标题找窗口：pattern / frames 的标题就是参数）
win_field() { # <标题> <字段>
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
title, key = sys.argv[1], sys.argv[2]
for w in json.load(sys.stdin):
    if w["title"] == title:
        v = w.get(key)
        if isinstance(v, bool):
            print("true" if v else "false")
        elif isinstance(v, float):
            print("%g" % v)
        else:
            print(v)
        break
else:
    print("(没有这个窗口)")
' "$1" "$2"
}

win_id() { # <标题> -> id
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["title"] == sys.argv[1]:
        print(w["id"]); break
else:
    print("")
' "$1"
}

# 日志里最后一次 place <标题> -> x,y WxH（没出现过就打 -）
place_of() { # <标题>
	python3 - "$LOG" "$1" <<'PY'
import re, sys
log, title = sys.argv[1], sys.argv[2]
last = "-"
for line in open(log, encoding="utf-8", errors="replace"):
	m = re.search(r"place %s -> (-?\d+),(-?\d+) (\d+)x(\d+)" % re.escape(title), line)
	if m:
		last = "%s,%s %sx%s" % m.groups()
print(last)
PY
}

check() { # <期望> <实际> <说明>
	if [ "$1" = "$2" ]; then
		pass "$3（$2）"
	else
		fail "$3（期望 $1，实际 $2）"
	fi
}

check_ne() { # <不期望> <实际> <说明>
	if [ "$1" != "$2" ]; then
		pass "$3（$2）"
	else
		fail "$3（不该是 $1）"
	fi
}

spawn_pattern() { # <标题>
	python3 tools/pattern.py "$1" >/dev/null 2>&1 &
	for _ in $(seq 1 30); do
		grep -aq "place $1 -> " "$LOG" && break
		sleep 0.2
	done
	sleep 0.4
}

# 等一个新窗口出现（按标题轮询 vidowmsg）
wait_win() { # <标题>
	for _ in $(seq 1 40); do
		[ "$(win_id "$1")" != "" ] && break
		sleep 0.2
	done
}

# ---------------------------------------------------------------- 基本行为
mark "1) 两个窗口各一条列：最小化左边那条，右边滑到最左边"
start_vidow false
spawn_pattern a1
spawn_pattern a2
BEFORE_A2=$(place_of a2)
# a1 的 x 是 0 还是 6 取决于条带有没有滚动（两条列刚好比工作区宽一点点，
# 焦点跟到右边那条时视口会滚 6 像素），所以这里只认 a2 的位置变化。
check "640,6 634x708" "$BEFORE_A2" "a2 一开始在 a1 右边（640,6）"

ID1=$(win_id a1)
./build/vidowmsg minimize-window --id "$ID1" >/dev/null
sleep 0.8
check "true" "$(win_field a1 minimized)" "a1 报 minimized=true"
check "false" "$(win_field a1 visible)" "a1 不显示（visible=false）"
check "true" "$(win_field a1 mapped)" "a1 客户端还活着（mapped=true）"
check "6,6 634x708" "$(place_of a2)" "a2 滑到最左边（条带合拢：x 640 -> 6）"
check "false" "$(win_field a2 minimized)" "a2 没被牵连"
if grep -aq "最小化: a1 收起" "$LOG"; then
	pass "日志说了「最小化: a1 收起」"
else
	fail "日志里没有最小化的记录"
fi

# ---------------------------------------------------------------- 焦点交接
mark "2) 最小化当前聚焦的窗口：焦点交给邻居"
# 先把 a1 放回来（不然收掉 a2 之后这块屏上没别的窗口，焦点只能落到「无」）
./build/vidowmsg unminimize-window >/dev/null
sleep 0.8
./build/vidowmsg focus-window --id "$(win_id a2)" >/dev/null
sleep 0.5
check "a2" "$(./build/vidowmsg windows 2>/dev/null | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if w["focused"]: print(w["title"]); break
else: print("-")')" "现在聚焦的是 a2"
./build/vidowmsg minimize-window >/dev/null
sleep 0.8
check "a1" "$(./build/vidowmsg windows 2>/dev/null | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if w["focused"]: print(w["title"]); break
else: print("-")')" "焦点交给了 a1（不是留在收起来的窗口上）"
check "true" "$(win_field a2 minimized)" "a2 收起来了"
if grep -aq "焦点: a2 -> a1" "$LOG"; then
	pass "日志里有「焦点: a2 -> a1」"
else
	fail "日志里没有焦点交接"
fi

# ---------------------------------------------------------------- 同列上下
mark "3) 同列两个窗口：上面那个收起来，下面那个撑满整列"
./build/vidowmsg unminimize-window >/dev/null   # a2 回来
sleep 0.8
# 把 a1 塞进 a2 那条列：焦点给 a1，然后往下搬 = 并进下一条列
./build/vidowmsg focus-window --id "$(win_id a1)" >/dev/null
sleep 0.5
./build/vidowmsg move-window-direction 1 >/dev/null 2>&1
sleep 0.8
A1_BEFORE=$(place_of a1)
A2_BEFORE=$(place_of a2)
echo "  （同列后 a1=$A1_BEFORE  a2=$A2_BEFORE）"
X1=$(echo "$A1_BEFORE" | cut -d, -f1)
X2=$(echo "$A2_BEFORE" | cut -d, -f1)
if [ -n "$X1" ] && [ "$X1" = "$X2" ]; then
	pass "两个窗口进了同一条列（x 相同：$A1_BEFORE / $A2_BEFORE）"
	# 同列时每个占半高，收起来一个之后另一个应该撑满整列
	H1=$(echo "$A2_BEFORE" | cut -d' ' -f2 | cut -dx -f2)
	./build/vidowmsg minimize-window --id "$(win_id a1)" >/dev/null
	sleep 0.8
	H2=$(place_of a2 | cut -d' ' -f2 | cut -dx -f2)
	if [ "${H2:-0}" -gt "${H1:-0}" ] 2>/dev/null; then
		pass "同列剩下的那个撑满了整列（高 $H1 -> $H2）"
	else
		fail "同列剩下的那个没撑满（高 $H1 -> $H2）"
	fi
	check "true" "$(win_field a1 minimized)" "同列上面那个收起来了"
else
	fail "两个窗口没进同一条列（$A1_BEFORE / $A2_BEFORE）—— 这节跳过"
fi

# ---------------------------------------------------------------- 放回
mark "4) 放回：回到原来的位置"
./build/vidowmsg unminimize-window >/dev/null
sleep 0.8
check "true" "$(win_field a1 visible)" "a1 又显示出来了"
check "false" "$(win_field a1 minimized)" "a1 报 minimized=false"
check "true" "$(win_field a1 focused)" "放回顺手给了焦点"
check "false" "$(win_field a2 minimized)" "a2 没被牵连"
if grep -aq "最小化: a1 放回" "$LOG"; then
	pass "日志说了「最小化: a1 放回」"
else
	fail "日志里没有放回的记录"
fi

# ---------------------------------------------------------------- 键位动作
mark "5) 键位那套动作名（vidow.bind 用的就是它们）"
./build/vidowmsg action minimize-window >/dev/null
sleep 0.8
check "true" "$(win_field a1 minimized)" "action minimize-window 收起当前窗口"
./build/vidowmsg action restore-window >/dev/null
sleep 0.8
check "false" "$(win_field a1 minimized)" "action restore-window 放回最近收起来的那个"

# ---------------------------------------------------------------- 整列都收起来
mark "6) 整列都最小化：那条列从条带里消失"
./build/vidowmsg minimize-window --id "$(win_id a1)" >/dev/null
./build/vidowmsg minimize-window --id "$(win_id a2)" >/dev/null
sleep 0.8
spawn_pattern a3
check "6,6 634x708" "$(place_of a3)" "新窗口落在最左边（两条列都让出了位置）"
check "true" "$(win_field a1 minimized)" "a1 还是收着的"
check "true" "$(win_field a2 minimized)" "a2 还是收着的"

# ---------------------------------------------------------------- 浮窗
mark "6b) 浮窗也能收起来（几何留着）"
./build/vidowmsg toggle-floating >/dev/null    # 焦点在 a3 上
sleep 0.8
check "true" "$(win_field a3 floating)" "a3 变成浮窗了"
FLOAT_PLACE=$(place_of a3)
./build/vidowmsg minimize-window >/dev/null
sleep 0.8
check "true" "$(win_field a3 minimized)" "浮窗收起来了"
check "false" "$(win_field a3 visible)" "浮窗不显示"
./build/vidowmsg unminimize-window >/dev/null
sleep 0.8
check "false" "$(win_field a3 minimized)" "浮窗放回来了"
check "true" "$(win_field a3 visible)" "浮窗又显示出来了"
check "$FLOAT_PLACE" "$(place_of a3)" "浮窗的几何没变（还是原来那个位置）"
./build/vidowmsg toggle-floating >/dev/null    # 变回平铺，后面的用例不受影响
sleep 0.8

# ---------------------------------------------------------------- 客户端自己请求
mark "7) 客户端自己请求最小化（xdg_toplevel.set_minimized）"
VIDOW_TEST_MINIMIZE_MS=700 ./build/frames 自缩 20 > "$RUNDIR/self.txt" 2>"$RUNDIR/self.err" &
SELF_PID=$!
wait_win 自缩
sleep 2.5
check "true" "$(win_field 自缩 minimized)" "合成器认了客户端的请求"
if grep -aq "minimize-request" "$RUNDIR/self.txt"; then
	pass "客户端自己打了一行 minimize-request"
else
	fail "客户端没发成请求（协议版本 / 时序）"
fi
F1=$(grep -c '^frame' "$RUNDIR/self.txt")
sleep 2.0
F2=$(grep -c '^frame' "$RUNDIR/self.txt")
if [ $((F2 - F1)) -eq 0 ]; then
	pass "没有规则就不补帧：收起来之后客户端停帧（0 帧/2 秒）"
else
	fail "收起来之后还在出帧（$((F2 - F1)) 帧/2 秒）—— 该停的没停"
fi
kill "$SELF_PID" 2>/dev/null

# ---------------------------------------------------------------- 规则 force_render
mark "8) 规则 force_render：最小化之后照样补帧（录屏 / 预览要新画面）"
cat >> "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.rule{ title = "心跳", force_render = true }
CFG
kill -HUP "$VIDOW_PGID" 2>/dev/null
sleep 1.0
./build/frames 心跳 25 > "$RUNDIR/hb.txt" 2>"$RUNDIR/hb.err" &
HB_PID=$!
wait_win 心跳
sleep 1.2
H1=$(grep -c '^frame' "$RUNDIR/hb.txt")
./build/vidowmsg minimize-window --id "$(win_id 心跳)" >/dev/null
sleep 2.5
H2=$(grep -c '^frame' "$RUNDIR/hb.txt")
check "true" "$(win_field 心跳 minimized)" "心跳收起来了"
if [ $((H2 - H1)) -ge 30 ]; then
	pass "最小化之后照样出帧（$((H2 - H1)) 帧/2.5 秒）"
else
	fail "最小化之后停了（$((H2 - H1)) 帧/2.5 秒）"
fi
if grep -aq "强制渲染: 心跳 它最小化了，照样补帧回调" "$LOG"; then
	pass "日志写明了原因：它最小化了"
else
	fail "日志没说清为什么补帧（最小化这条）"
fi
kill "$HB_PID" 2>/dev/null

# ---------------------------------------------------------------- SIGHUP
mark "9) SIGHUP 重载配置不改最小化状态"
# 心跳客户端这时候已经退出（窗口没了），拿还在的两个窗口看
kill -HUP "$VIDOW_PGID" 2>/dev/null
sleep 1.2
check "true" "$(win_field a1 minimized)" "重载之后 a1 还是收着的"
check "true" "$(win_field a2 minimized)" "重载之后 a2 还是收着的"
check "false" "$(win_field a3 minimized)" "重载之后 a3 还是正常显示的"
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "合成器还活着"
else
	fail "合成器死了"
fi

stop_vidow

# ---------------------------------------------------------------- X11
mark "10) X11：xdotool windowminimize -> WM_STATE=IconicState"
if ! command -v xdotool >/dev/null || ! command -v xprop >/dev/null; then
	echo "  （没装 xdotool / xprop，跳过 X11 这节）"
else
	start_vidow true
	export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)
	setsid nohup ./build/xpopup popup normal 100 100 300 300 fixed \
		> "$RUNDIR/xp.log" 2>&1 &
	XP_PID=$!
	wait_win xp-popup
	sleep 1.0
	XWID=$(xdotool search --name "xp-popup" 2>/dev/null | head -1)
	if [ -z "$XWID" ]; then
		fail "找不到 X 窗口（xdotool search 空）"
	else
		pass "X 窗口在（id $XWID）"
		xdotool windowminimize "$XWID" 2>/dev/null
		sleep 1.2
		check "true" "$(win_field xp-popup minimized)" "vidow 把 X 客户端收起来了"
		if grep -aq "最小化: xp-popup 收起" "$LOG"; then
			pass "日志说了「最小化: xp-popup 收起」"
		else
			fail "日志里没有 X11 最小化的记录"
		fi
		WM=$(xprop -id "$XWID" WM_STATE 2>/dev/null)
		case "$WM" in
		*Iconic*) pass "X 那边 WM_STATE=IconicState（客户端知道自己收起来了）" ;;
		*) fail "X 那边 WM_STATE 不是 IconicState（$WM）" ;;
		esac
		./build/vidowmsg unminimize-window >/dev/null
		sleep 1.2
		check "false" "$(win_field xp-popup minimized)" "放回之后 vidow 这边也回来了"
		WM2=$(xprop -id "$XWID" WM_STATE 2>/dev/null)
		case "$WM2" in
		*Normal*) pass "X 那边 WM_STATE 回到 NormalState" ;;
		*) fail "X 那边 WM_STATE 没回到 NormalState（$WM2）" ;;
		esac
	fi
	kill "$XP_PID" 2>/dev/null
	stop_vidow
fi

# ---------------------------------------------------------------- 收尾
mark "11) 收尾"
if grep -aq "Assertion" "$LOG" || grep -aqi "abort" "$LOG"; then
	fail "日志里有 assert / abort"
else
	pass "日志里没有 assert / abort"
fi

echo
if [ $FAILED -eq 0 ]; then
	echo "test-minimize.sh: OK"
else
	echo "test-minimize.sh: 有失败项"
fi
exit $FAILED
