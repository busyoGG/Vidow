#!/bin/bash
# 状态请求抢跑：客户端在**第一次 commit 之前**就 set_maximized / set_minimized /
# set_fullscreen（xdg-shell 允许）。wlroots 收到请求立刻发 request_* 信号，而它给
# 「还没 initialized 的 surface 排 configure」用的是一句 assert：
#
#   vidow: types/xdg_shell/wlr_xdg_surface.c:168: wlr_xdg_surface_schedule_configure:
#          Assertion `surface->initialized' failed.
#
# 合成器在信号里当场应答（wlr_xdg_toplevel_set_maximized /
# wlr_xdg_surface_schedule_configure）就会被客户端打死 —— Qt/KDE 恢复「上次是
# 最大化」的窗口就是这么开窗的（kate 从 tty 起合成器、一开就崩，栈顶
# view_request_maximize）。
#
# 判据（headless 1280x720 一块屏 + tools/preinit 客户端）：
#   1) 抢跑 set_maximized：vidow 活着，客户端拿到 configure，而且**第一个** configure
#      就带 maximized（vidow 现在有列最大化了，请求兑现成「它那条列最大化」，见
#      changelog 177），窗口正常进列表
#   2) 对照组：上屏之后再 set_maximized（正常时序）同样活着、同样兑现
#   3) 抢跑 set_minimized：vidow 活着，窗口状态真的是 minimized（请求没被丢掉）
#   4) 抢跑 set_fullscreen：vidow 活着，而且**兑现**了 —— 窗口 fullscreen=true，
#      客户端拿到的 configure 里带 fullscreen（不是「布局当它全屏、客户端不知道」）
#   5) 日志里没有 assert / abort；收尾一个 vidow 都不剩
#
# 用法：./test-preinit.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 每个测试收尾都按 cmdline 杀自己的客户端。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-preinit-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*build/preinit*) kill "$pid" 2>/dev/null ;;
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
	# setsid 有可能 fork，$! 是已经退出的父进程：按 cmdline 兜一遍
	for p in $(pgrep -x vidow 2>/dev/null); do
		kill -TERM "$p" 2>/dev/null
	done
	sleep 0.3
	for p in $(pgrep -x vidow 2>/dev/null); do
		kill -KILL "$p" 2>/dev/null
	done
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/vidow ] || { echo "test-preinit.sh: 先构建（./build/vidow 不在）"; exit 1; }
[ -x ./build/preinit ] || { echo "test-preinit.sh: 先构建（./build/preinit 不在）"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

cat > "$VIDOW_CONFIG_LUA" <<'CFG'
scale = "1"
window_open_ms = 60
window_close_ms = 60
xwayland = false
CFG

start_vidow() {
	export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
	rm -f "$LOG"
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

vidow_alive() { [ "$(pgrep -x vidow | wc -l)" -ge 1 ]; }

# 抢跑的客户端：后台起，等它报出 <等待的字样>（拿到 configure / 上屏）再往下走，
# 这样断言「窗口在列表里」的时候它还活着。
start_preinit() { # <输出文件> <等待的字样> <参数...>
	local out=$1 wait_for=$2
	shift 2
	: > "$out"
	./build/preinit "$@" > "$out" 2>&1 &
	CLIENT_PID=$!
	for _ in $(seq 1 60); do
		grep -aq "$wait_for" "$out" && break
		sleep 0.25
	done
}

# 收客户端：退出码放进 CLIENT_RC —— 别用 $(wait_preinit)，命令替换是子 shell，
# 子 shell 里 wait 等不了父 shell 的孩子（"不是当前 shell 的子进程"）。
wait_preinit() {
	CLIENT_RC=0
	wait "$CLIENT_PID" || CLIENT_RC=$?
	CLIENT_PID=""
}

# 一个窗口的 JSON 字段（标题找窗口）
win_field() { # <标题> <字段>
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
title, key = sys.argv[1], sys.argv[2]
try:
    wins = json.load(sys.stdin)
except Exception:
    wins = []   # 合成器死了 / 没吐 JSON：当成「没有这个窗口」
for w in wins:
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

# ---------------------------------------------------------------- 1/3 最大化抢跑

mark "1/4 抢跑 set_maximized（第一次 commit 之前）—— 兑现成列最大化"
start_vidow
start_preinit "$RUNDIR/out1" "preinit mapped" --maximize --linger=2 preinit 抢跑
sleep 0.4

if vidow_alive; then
	pass "vidow 还活着（旧代码：assert surface->initialized → abort）"
else
	fail "vidow 死了：$(grep -a 'Assertion' "$LOG" | tail -1)"
fi

# 请求真的发在第一次 commit 之前（工具自己这么报的：顺序也断言上）
if awk '/request maximize before-commit/{r=NR} /preinit mapped/{m=NR}
	END{exit !(r && m && r < m)}' "$RUNDIR/out1"; then
	pass "状态请求确实发在 commit 之前（request 行在 mapped 行前面）"
else
	fail "请求时序不对：$(tr '\n' '|' < "$RUNDIR/out1")"
fi

if grep -aq "preinit configure" "$RUNDIR/out1"; then
	pass "客户端拿到了 configure（没被卡在等应答上）"
else
	fail "客户端一次 configure 都没收到"
fi

FIRST1=$(grep -a "preinit configure" "$RUNDIR/out1" | head -1)
case "$FIRST1" in
*maximized*)
	pass "第一个 configure 就带 maximized（抢跑的最大化请求被兑现了：$FIRST1）" ;;
*)
	fail "第一个 configure 里没有 maximized：$FIRST1" ;;
esac

if grep -aq "preinit configure" "$RUNDIR/out1" &&
		! grep -aq "0x0" "$RUNDIR/out1"; then
	pass "configure 带的是真实尺寸（不是 0x0）"
else
	fail "configure 尺寸不对：$(grep -a 'preinit configure' "$RUNDIR/out1" | tr '\n' '|')"
fi

if [ "$(win_field 抢跑 visible)" = "true" ] &&
		[ "$(win_field 抢跑 minimized)" = "false" ]; then
	pass "窗口正常进列表（visible=true，minimized=false）"
else
	fail "窗口状态不对：visible=$(win_field 抢跑 visible) minimized=$(win_field 抢跑 minimized)"
fi

wait_preinit
RC=$CLIENT_RC
if [ "$RC" = "0" ]; then
	pass "客户端正常退出（连接没断）"
else
	fail "客户端退出码 $RC（合成器把它踢了？）"
fi
stop_vidow

# ---------------------------------------------------------------- 2/3 对照组

mark "2/4 对照组：上屏之后再 set_maximized（正常时序）"
start_vidow
start_preinit "$RUNDIR/out2" "preinit mapped" --maximize --after-commit --linger=2 preinit 对照
sleep 0.4

if vidow_alive; then
	pass "vidow 还活着"
else
	fail "vidow 死了：$(grep -a 'Assertion' "$LOG" | tail -1)"
fi

if grep -aq "request maximize after-commit" "$RUNDIR/out2" &&
		grep -aq "preinit configure" "$RUNDIR/out2"; then
	pass "请求发在上屏之后，客户端照样拿到 configure"
else
	fail "对照组没走完：$(tr '\n' '|' < "$RUNDIR/out2")"
fi

if grep -aq "preinit configure.*maximized" "$RUNDIR/out2"; then
	pass "应答里带 maximized（列最大化被兑现，客户端知道）"
else
	fail "configure 里没有 maximized：$(grep -a 'preinit configure' "$RUNDIR/out2" | tr '\n' '|')"
fi

if [ "$(win_field 对照 visible)" = "true" ]; then
	pass "窗口在列表里（visible=true）"
else
	fail "窗口没进列表：visible=$(win_field 对照 visible)"
fi

wait_preinit
if [ "$CLIENT_RC" = "0" ]; then
	pass "客户端正常退出（连接没断）"
else
	fail "客户端退出码 $CLIENT_RC"
fi
stop_vidow

# ---------------------------------------------------------------- 3/3 最小化抢跑

mark "3/4 抢跑 set_minimized"
start_vidow
start_preinit "$RUNDIR/out3" "preinit request" --minimize --timeout=3 preinit 收起
sleep 0.8

if vidow_alive; then
	pass "vidow 还活着（旧代码：view_request_minimize → schedule_configure → abort）"
else
	fail "vidow 死了：$(grep -a 'Assertion' "$LOG" | tail -1)"
fi

if [ "$(win_field 收起 minimized)" = "true" ]; then
	pass "请求没被丢掉：窗口状态就是 minimized"
else
	fail "窗口没被最小化：minimized=$(win_field 收起 minimized)"
fi

if grep -aq "最小化: 收起 收起" "$LOG"; then
	pass "日志写清楚了「最小化: 收起 收起」"
else
	fail "日志里没有最小化那行：$(grep -a '最小化' "$LOG" | tail -1)"
fi

wait_preinit
RC=$CLIENT_RC
if [ "$RC" = "0" ]; then
	pass "客户端正常退出"
else
	fail "客户端退出码 $RC"
fi
stop_vidow

# ---------------------------------------------------------------- 4/4 全屏抢跑

mark "4/4 抢跑 set_fullscreen"
start_vidow
start_preinit "$RUNDIR/out4" "preinit mapped" --fullscreen --linger=2 preinit 全屏抢跑
sleep 0.4

if vidow_alive; then
	pass "vidow 还活着"
else
	fail "vidow 死了：$(grep -a 'Assertion' "$LOG" | tail -1)"
fi

if [ "$(win_field 全屏抢跑 fullscreen)" = "true" ]; then
	pass "抢跑的全屏请求被兑现了：窗口 fullscreen=true"
else
	fail "窗口没进全屏：fullscreen=$(win_field 全屏抢跑 fullscreen)"
fi

if grep -aq "preinit configure.*fullscreen" "$RUNDIR/out4"; then
	pass "客户端拿到的 configure 里带 fullscreen（两边状态一致）"
else
	fail "configure 里没有 fullscreen：$(grep -a 'preinit configure' "$RUNDIR/out4" | tr '\n' '|')"
fi

wait_preinit
RC=$CLIENT_RC
if [ "$RC" = "0" ]; then
	pass "客户端正常退出"
else
	fail "客户端退出码 $RC"
fi
stop_vidow

# ---------------------------------------------------------------- 收尾

mark "收尾"
if ! grep -aqE "Assertion|Aborted|段错误" "$LOG"; then
	pass "日志里没有 assert / abort / 段错误"
else
	fail "日志里有崩溃痕迹：$(grep -aE 'Assertion|Aborted|段错误' "$LOG" | tail -1)"
fi

LEFT=$(pgrep -x vidow 2>/dev/null | wc -l)
if [ "$LEFT" = "0" ]; then
	pass "收尾干净：一个 vidow 都不剩"
else
	fail "收尾还剩 $LEFT 个 vidow（$(pgrep -x vidow | tr '\n' ' ')）"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "test-preinit.sh: 全部通过"
else
	echo "test-preinit.sh: 有失败"
fi
exit "$FAILED"
