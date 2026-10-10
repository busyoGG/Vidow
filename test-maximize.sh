#!/bin/bash
# 列最大化（niri 的 maximize-column）：整列撑满工作区宽度，再按一次还原成**原来那个
# 比例**（不是固定值）。vidow 没有「窗口级」最大化 —— 列是布局的最小单位，客户端
# set_maximized 也映射到它所在的这条列上。
#
# 判据（headless 1280x720 一块屏 + pattern / preinit 客户端）：
#   1) IPC toggle-maximize-column：列 JSON 报 maximized=true、width_fraction 1.0、
#      width = 工作区宽 1268，窗口的 place 行跟着变成整块工作区
#   2) 再按一次：回到原来的 0.5（631 = (1268+6)×0.5−6），place 也回来
#   3) 记住的是**原比例**：先 set-column-width 0.25，最大化再还原 -> 回到 0.25
#   4) 手动改宽度 = 退出最大化状态：cycle-column-width / set-column-width /
#      expand-column-to-available-width 都把 maximized 按掉
#   5) 键位动作那套名字也能用：vidowmsg action maximize-column
#   6) 客户端上屏之后请求（preinit --maximize --after-commit）：列最大化，而且客户端
#      真的收到一个 states=[maximized] 的 configure（第一帧那个是不带的）
#   7) 抢跑请求（第一次 commit 之前，kate 那种开窗方式）：合成器不崩（wlroots 那句
#      assert 的坑），**第一个** configure 就带 maximized
#   8) 用户按出来的最大化，客户端随手 set_maximized(false) 撤不掉：列还是最大化的，
#      而且客户端被告知「你仍然是最大化的」
#   9) 客户端自己要来的最大化，它自己撤销就理：列还原
#  10) column-maximize-changed 事件（订阅能收到，载荷是那条列）
#  11) 浮窗没有列：toggle-maximize-column 当场报错
#  12) X11：wmctrl 请求最大化 -> 列最大化 + X 那边 _NET_WM_STATE 里有 MAXIMIZED_VERT/
#      HORZ；wmctrl 撤销 -> 列还原 + 属性没了
#  13) 日志里没有 assert / abort，收尾没有残留
#
# 用法：./test-maximize.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 每个测试收尾都按 cmdline 杀 pattern.py / preinit，
# 同时跑会把对方的客户端一起杀掉。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-maximize-test
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
		*pattern.py*|*build/preinit*|*build/xpopup*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

python3 -c "import gi" 2>/dev/null || { echo "test-maximize.sh: 需要 python3-gobject"; exit 1; }
[ -x ./build/preinit ] || { echo "test-maximize.sh: 先构建（./build/preinit 不在）"; exit 1; }

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

win_id() { # <标题> -> id（没有就打空）
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["title"] == sys.argv[1]:
        print(w["id"]); break
else:
    print("")
' "$1"
}

# 这个窗口所在那条列的字段（列 JSON 里 windows 是 id 列表）
col_field() { # <标题> <字段>
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, subprocess, sys
title, key = sys.argv[1], sys.argv[2]
wid = None
for w in json.load(sys.stdin):
    if w["title"] == title:
        wid = w["id"]; break
if wid is None:
    print("(没有这个窗口)"); raise SystemExit
out = subprocess.run(["./build/vidowmsg", "columns"],
                     capture_output=True, text=True).stdout
for c in json.loads(out):
    if wid in c["windows"]:
        v = c.get(key)
        if isinstance(v, bool):
            print("true" if v else "false")
        elif isinstance(v, float):
            print("%g" % v)
        else:
            print(v)
        break
else:
    print("(没有这条列)")
' "$1" "$2"
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

spawn_pattern() { # <标题>
	python3 tools/pattern.py "$1" >/dev/null 2>&1 &
	for _ in $(seq 1 30); do
		grep -aq "place $1 -> " "$LOG" && break
		sleep 0.2
	done
	sleep 0.4
}

# 起一个 preinit 客户端：<输出文件> <标题> <preinit 参数…>
spawn_preinit() { # <输出文件> <标题> <参数…>
	local out="$1" title="$2"; shift 2
	setsid nohup ./build/preinit "$@" preinit "$title" > "$out" 2>&1 &
	local pid=$!
	for _ in $(seq 1 40); do
		grep -aq "preinit mapped" "$out" && break
		sleep 0.2
	done
	sleep 0.6
}

# 某个标记之后有没有出现过带 maximized 的 configure
conf_after() { # <输出文件> <标记正则>
	python3 - "$1" "$2" <<'PY'
import re, sys
lines = open(sys.argv[1], encoding="utf-8", errors="replace").read().splitlines()
mark = re.compile(sys.argv[2])
idx = -1
for i, l in enumerate(lines):
	if mark.search(l):
		idx = i
hit = "no"
for l in lines[idx + 1:]:
	if "preinit configure" in l and "maximized" in l:
		hit = "yes"
		break
print(hit)
PY
}

# ---------------------------------------------------------------- 基本行为
mark "1) 列最大化：整列撑满工作区（0.5 档 631 -> 1268）"
start_vidow false
spawn_pattern a1
check "false" "$(col_field a1 maximized)" "一开始不是最大化的"
check "0.5" "$(col_field a1 width_fraction)" "一开始是默认的 0.5"
check "631" "$(col_field a1 width)" "列宽 = (1268+6) x 0.5 - 6 = 631"
check "6,6 631x708" "$(place_of a1)" "窗口占半宽（6,6 631x708）"
if ./build/vidowmsg toggle-maximize-column >/dev/null 2>&1; then
	pass "IPC toggle-maximize-column 成功"
else
	fail "IPC toggle-maximize-column 失败"
fi
sleep 0.5
check "true" "$(col_field a1 maximized)" "现在报 maximized=true"
check "1" "$(col_field a1 width_fraction)" "宽度比例顶到 1.0"
check "1268" "$(col_field a1 width)" "列宽 = 整块工作区 1268"
check "6,6 1268x708" "$(place_of a1)" "窗口撑满工作区（6,6 1268x708）"

mark "2) 再按一次：还原成原来那个比例"
./build/vidowmsg toggle-maximize-column >/dev/null 2>&1
sleep 0.5
check "false" "$(col_field a1 maximized)" "还原了"
check "0.5" "$(col_field a1 width_fraction)" "比例回到 0.5"
check "6,6 631x708" "$(place_of a1)" "窗口回到半宽"

mark "3) 记住的是原比例，不是写死的 0.5"
./build/vidowmsg set-column-width 0.25 >/dev/null 2>&1
sleep 0.4
check "0.25" "$(col_field a1 width_fraction)" "先手动改成 0.25"
./build/vidowmsg toggle-maximize-column >/dev/null 2>&1
sleep 0.4
check "1" "$(col_field a1 width_fraction)" "最大化之后是满宽"
./build/vidowmsg toggle-maximize-column >/dev/null 2>&1
sleep 0.4
check "0.25" "$(col_field a1 width_fraction)" "还原回 0.25（不是 0.5）"
check "312" "$(col_field a1 width)" "列宽 (1268+6) x 0.25 - 6 = 312"

mark "4) 手动改宽度 = 退出最大化状态"
./build/vidowmsg toggle-maximize-column >/dev/null 2>&1
sleep 0.4
check "true" "$(col_field a1 maximized)" "先最大化"
./build/vidowmsg cycle-column-width >/dev/null 2>&1
sleep 0.4
check "false" "$(col_field a1 maximized)" "cycle-column-width 把最大化按掉了"
check "0.333333" "$(col_field a1 width_fraction)" "而且满宽再循环回到 1/3"
./build/vidowmsg toggle-maximize-column >/dev/null 2>&1
sleep 0.3
./build/vidowmsg set-column-width 0.5 >/dev/null 2>&1
sleep 0.4
check "false" "$(col_field a1 maximized)" "set-column-width 也把最大化按掉了"
check "0.5" "$(col_field a1 width_fraction)" "宽度听用户的"
./build/vidowmsg toggle-maximize-column >/dev/null 2>&1
sleep 0.3
./build/vidowmsg expand-column-to-available-width >/dev/null 2>&1
sleep 0.4
check "false" "$(col_field a1 maximized)" "expand-column-to-available-width 同样按掉（它只改宽度、不记原值）"
check "1" "$(col_field a1 width_fraction)" "宽度还是满宽"

mark "5) 键位动作那套名字也能用"
./build/vidowmsg action "maximize-column" >/dev/null 2>&1
sleep 0.5
check "true" "$(col_field a1 maximized)" "action maximize-column 生效了"
if grep -aq "action: maximize-column -> 列最大化" "$LOG"; then
	pass "日志说了「action: maximize-column -> 列最大化」"
else
	fail "日志里没有这条动作记录"
fi
./build/vidowmsg action "toggle-maximize-column" >/dev/null 2>&1
sleep 0.5
check "false" "$(col_field a1 maximized)" "别名 toggle-maximize-column 也能还原"
if grep -aq "action: toggle-maximize-column -> 列还原" "$LOG"; then
	pass "日志说了「列还原」"
else
	fail "日志里没有还原那条记录"
fi

# ---------------------------------------------------------------- 客户端请求
mark "6) 客户端上屏之后请求最大化（preinit --after-commit）"
P1="$RUNDIR/p1.txt"
spawn_preinit "$P1" p1 --maximize --after-commit --linger=4 --timeout=8
check "true" "$(col_field p1 maximized)" "它那条列最大化了"
check "1268" "$(col_field p1 width)" "列宽 = 整块工作区"
check "false" "$(col_field a1 maximized)" "a1 那条列没被牵连"
check "no" "$(python3 - "$P1" <<'PY'
import sys
first = ""
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
	if "preinit configure" in l:
		first = l; break
print("yes" if "maximized" in first else "no")
PY
)" "第一帧那个 configure 不带 maximized（请求是上屏之后才发的）"
check "yes" "$(conf_after "$P1" "preinit request maximize")" "请求之后收到了带 maximized 的 configure"
if grep -aq "place p1 -> 6,6 1268x708" "$LOG"; then
	pass "窗口真的被摆成整块工作区（place p1 -> 6,6 1268x708）"
else
	fail "窗口没被摆成整块工作区"
fi

mark "7) 抢跑请求（第一次 commit 之前，kate 那种开窗方式）"
P2="$RUNDIR/p2.txt"
spawn_preinit "$P2" p2 --maximize --linger=4 --timeout=8
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "合成器还活着（wlroots 那句 assert 的坑没踩到）"
else
	fail "合成器死了（抢跑的最大化请求）"
fi
check "true" "$(col_field p2 maximized)" "列最大化了"
check "yes" "$(python3 - "$P2" <<'PY'
import sys
first = ""
for l in open(sys.argv[1], encoding="utf-8", errors="replace"):
	if "preinit configure" in l:
		first = l; break
print("yes" if "maximized" in first else "no")
PY
)" "**第一个** configure 就带 maximized（抢跑的请求被兑现了）"
if grep -aq "preinit request maximize before-commit" "$P2"; then
	pass "确认它是在 commit 之前发的请求"
else
	fail "这个客户端没按预期在 commit 之前发请求"
fi

mark "8) 用户按出来的最大化，客户端撤不掉"
# 这个客户端**从来没要过**最大化（--none），上屏 2.5 秒后却发一个 set_maximized(false)
P3="$RUNDIR/p3.txt"
spawn_preinit "$P3" p3 --none --unmaximize-after=2500 --linger=8 --timeout=12
./build/vidowmsg set-maximize-column true --title p3 >/dev/null 2>&1
sleep 0.4
check "true" "$(col_field p3 maximized)" "先由用户（IPC）把它最大化"
for _ in $(seq 1 30); do
	grep -aq "preinit request unmaximize" "$P3" && break
	sleep 0.2
done
sleep 0.6
check "true" "$(col_field p3 maximized)" "客户端发 set_maximized(false) 之后，列**还是**最大化的"
check "yes" "$(conf_after "$P3" "preinit request unmaximize")" "而且客户端被告知「你仍然是最大化的」（带 maximized 的 configure）"

mark "9) 客户端自己要来的最大化，它自己撤销就理"
P4="$RUNDIR/p4.txt"
spawn_preinit "$P4" p4 --maximize --unmaximize-after=2500 --linger=14 --timeout=18
check "true" "$(col_field p4 maximized)" "它自己要的：列最大化了"
for _ in $(seq 1 30); do
	grep -aq "preinit request unmaximize" "$P4" && break
	sleep 0.2
done
sleep 0.6
check "false" "$(col_field p4 maximized)" "它自己撤销：列还原了"
check "631" "$(col_field p4 width)" "列宽回到 0.5 档 = 631"
check "no" "$(conf_after "$P4" "preinit request unmaximize")" "撤销之后收到的 configure 不带 maximized"

# ---------------------------------------------------------------- 事件 / 报错
mark "10) column-maximize-changed 事件"
timeout 12 ./build/vidowmsg --raw watch column-maximize-changed \
	> "$RUNDIR/watch.txt" 2>&1 &
WATCH_PID=$!
sleep 0.8
./build/vidowmsg toggle-maximize-column --title p4 >/dev/null 2>&1
sleep 0.8
kill "$WATCH_PID" 2>/dev/null
if grep -aq '"event":"column-maximize-changed"' "$RUNDIR/watch.txt"; then
	pass "收到 column-maximize-changed"
else
	fail "没收到 column-maximize-changed"
fi
if grep -aq '"maximized":true' "$RUNDIR/watch.txt"; then
	pass "事件载荷里带着 maximized=true"
else
	fail "事件载荷里没有 maximized"
fi

mark "11) 浮窗没有列：当场报错"
./build/vidowmsg toggle-floating --title p4 >/dev/null 2>&1
sleep 0.6
if ./build/vidowmsg toggle-maximize-column --title p4 2>"$RUNDIR/err.txt" >/dev/null; then
	fail "浮窗上居然成功了"
else
	pass "浮窗上当场失败"
fi
if grep -aq "不在列里" "$RUNDIR/err.txt"; then
	pass "错误信息说清了原因：$(cat "$RUNDIR/err.txt")"
else
	fail "错误信息没说清（$(cat "$RUNDIR/err.txt")）"
fi
./build/vidowmsg toggle-floating --title p4 >/dev/null 2>&1
sleep 0.5

stop_vidow

# ---------------------------------------------------------------- X11
mark "12) X11：wmctrl 请求最大化 -> 列最大化 + _NET_WM_STATE"
if ! command -v wmctrl >/dev/null || ! command -v xprop >/dev/null; then
	echo "  （没装 wmctrl / xprop，跳过 X11 这节）"
else
	start_vidow true
	export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)
	setsid nohup ./build/xpopup popup normal 100 100 300 300 \
		> "$RUNDIR/xp.log" 2>&1 &
	XP_PID=$!
	for _ in $(seq 1 40); do
		[ "$(win_id xp-popup)" != "" ] && break
		sleep 0.2
	done
	sleep 1.0
	XWID=$(xdotool search --name "xp-popup" 2>/dev/null | head -1)
	if [ -z "$XWID" ]; then
		fail "找不到 X 窗口（xdotool search 空）"
	else
		pass "X 窗口在（id $XWID）"
		check "false" "$(col_field xp-popup maximized)" "X 窗口一开始不是最大化的"
		wmctrl -i -r "$XWID" -b add,maximized_vert,maximized_horz 2>/dev/null
		sleep 1.2
		check "true" "$(col_field xp-popup maximized)" "X 客户端请求最大化 -> 它那条列最大化了"
		check "1268" "$(col_field xp-popup width)" "列宽 = 整块工作区"
		XPROP=$(xprop -id "$XWID" _NET_WM_STATE 2>/dev/null)
		case "$XPROP" in
		*_NET_WM_STATE_MAXIMIZED_VERT*) pass "X 那边 _NET_WM_STATE 里有 MAXIMIZED_VERT" ;;
		*) fail "X 那边 _NET_WM_STATE 里没有 MAXIMIZED_VERT（$XPROP）" ;;
		esac
		wmctrl -i -r "$XWID" -b remove,maximized_vert,maximized_horz 2>/dev/null
		sleep 1.2
		check "false" "$(col_field xp-popup maximized)" "X 客户端撤销 -> 列还原"
		check "631" "$(col_field xp-popup width)" "列宽回到 631"
		XPROP2=$(xprop -id "$XWID" _NET_WM_STATE 2>/dev/null)
		case "$XPROP2" in
		*_NET_WM_STATE_MAXIMIZED_VERT*) fail "X 那边的 MAXIMIZED_VERT 没撤掉（$XPROP2）" ;;
		*) pass "X 那边 MAXIMIZED_VERT 也撤掉了" ;;
		esac
		# 反过来：用户按出来的最大化，X 那边也得知道
		./build/vidowmsg toggle-maximize-column --title xp-popup >/dev/null 2>&1
		sleep 1.2
		check "true" "$(col_field xp-popup maximized)" "用户（IPC）最大化 X 窗口的列"
		XPROP3=$(xprop -id "$XWID" _NET_WM_STATE 2>/dev/null)
		case "$XPROP3" in
		*_NET_WM_STATE_MAXIMIZED_HORZ*) pass "X 客户端被告知自己最大化了（HORZ 也在）" ;;
		*) fail "X 客户端不知道自己是最大化的（$XPROP3）" ;;
		esac
	fi
	kill "$XP_PID" 2>/dev/null
	stop_vidow
fi

# ---------------------------------------------------------------- 收尾
mark "13) 收尾"
if grep -aq "Assertion" "$LOG" || grep -aqi "abort" "$LOG"; then
	fail "日志里有 assert / abort"
else
	pass "日志里没有 assert / abort"
fi
sleep 0.5
LEFT=$(pgrep -x vidow 2>/dev/null | wc -l)
if [ "$LEFT" = "0" ]; then
	pass "没有残留的 vidow 进程"
else
	fail "收尾还剩 $LEFT 个 vidow"
fi

echo
if [ $FAILED -eq 0 ]; then
	echo "test-maximize.sh: OK"
else
	echo "test-maximize.sh: 有失败项"
fi
exit $FAILED
