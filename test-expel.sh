#!/bin/bash
# 把窗口从它那条列里拎出来、单独成一条列（niri 的 expel-window-from-column）。
#
# 起因是用户报的「窗口放入列后无法取出」：拖拽只能往列里插（drag.c 的落点判据），
# 取出来以前没有路 —— 这条动作 / IPC 就是它。
#
# 判据（headless 1280x720 一块屏 + pattern 客户端，全走 IPC，不截屏）：
#   1) 两个窗口各一条列 → move-window-direction -1 并成一列（1 列 2 窗口，顺序对）
#   2) expel-window-from-column 取出来 → 又是 2 列、各 1 窗口，新列在**右边**
#   3) 新列继承原来那条列的宽度（width_fraction 一样，取出来不该让尺寸突变）
#   4) 焦点还在被取出来的那个窗口上
#   5) 取到左边（`... left`）：新列插在原来那条列左边
#   6) 列里只剩它一个时取不出来：IPC 报人话、列结构一点没动
#   7) 浮窗取不出来（报人话）
#   8) 键位动作那条路也通（vidowmsg action expel-window-from-column）
#   9) 日志里没有 assert / abort
#
# 用法：./test-expel.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-expel-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in *pattern.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
}

cleanup() {
	stop_clients
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.3
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi" 2>/dev/null || { echo "test-expel.sh: 需要 python3-gobject"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
xwayland = false
window_open_ms = 0
window_move_ms = 0
vidow.bind("Mod+Period", "expel-window-from-column")
vidow.bind("Mod+Shift+Period", "expel-window-from-column left")
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || {
	echo "test-expel.sh: vidow 没起来"
	tail -5 "$LOG"
	exit 1
}
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

# ------------------------------------------------------------ 工具
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

win_field() { # <标题> <字段>
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
title, key = sys.argv[1], sys.argv[2]
for w in json.load(sys.stdin):
    if w["title"] == title:
        v = w.get(key)
        print("true" if v is True else "false" if v is False else v)
        break
else:
    print("(没有这个窗口)")
' "$1" "$2"
}

col_field() { # <标题> <列字段>
	./build/vidowmsg windows > "$RUNDIR/windows.json" 2>/dev/null
	./build/vidowmsg columns > "$RUNDIR/columns.json" 2>/dev/null
	python3 - "$1" "$2" "$RUNDIR/windows.json" "$RUNDIR/columns.json" <<'PY'
import json, sys
title, key = sys.argv[1], sys.argv[2]
wins = json.load(open(sys.argv[3], encoding='utf-8'))
wid = next((w["id"] for w in wins if w["title"] == title), None)
cols = json.load(open(sys.argv[4], encoding='utf-8'))
if wid is None:
    print("(没有这个窗口)"); raise SystemExit
for c in cols:
    if wid in c["windows"]:
        v = c.get(key)
        print("true" if v is True else "false" if v is False else
              ("%g" % v if isinstance(v, float) else v))
        break
else:
    print("(没有这条列)")
PY
}

# 列结构：列数 + 每列有几个窗口 + 每列里的窗口标题（从左到右）
shape() {
	./build/vidowmsg columns > "$RUNDIR/columns.json" 2>/dev/null
	./build/vidowmsg windows > "$RUNDIR/windows.json" 2>/dev/null
	python3 - "$RUNDIR/columns.json" "$RUNDIR/windows.json" <<'PY'
import json, sys
cols = json.load(open(sys.argv[1], encoding='utf-8'))
wins = json.load(open(sys.argv[2], encoding='utf-8'))
title = {w["id"]: w["title"] for w in wins}
print(" | ".join("[%s]" % " ".join(title.get(i, "?") for i in c["windows"])
                 for c in cols))
PY
}

focused_title() {
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w.get("focused"):
        print(w["title"]); break
else:
    print("(没有)")'
}

spawn_pattern() { # <标题>
	python3 tools/pattern.py "$1" >/dev/null 2>&1 &
	for _ in $(seq 1 40); do
		[ "$(win_id "$1")" != "" ] && break
		sleep 0.2
	done
	sleep 0.5
}

check() { # <期望> <实际> <说明>
	if [ "$1" = "$2" ]; then
		pass "$3（$2）"
	else
		fail "$3（期望 $1，实际 $2）"
	fi
}

# ------------------------------------------------------------ 1) 并成一列
mark "1) 两个窗口并成一列（先造出「放进列里」这个状态）"
spawn_pattern a1
spawn_pattern a2
check "[a1] | [a2]" "$(shape)" "两个窗口各一条列（a2 在右边）"
check "a2" "$(focused_title)" "焦点在 a2（刚开的那个）"

./build/vidowmsg move-window-direction -1 >/dev/null 2>&1
sleep 0.6
check "[a2 a1]" "$(shape)" "a2 并进 a1 那条列（上下叠起来）"
check "0" "$(col_field a2 index)" "只剩一条列了（index=0）"
# 给这条列一个不一样的宽度：取出来之后新列该继承它（0.5 是默认值，看不出继承）
./build/vidowmsg set-column-width 0.3 >/dev/null 2>&1
sleep 0.4
check "0.3" "$(col_field a2 width_fraction)" "并起来这条列宽度改成 0.3"

# ------------------------------------------------------------ 2) 取出来
mark "2) expel-window-from-column：取出来单独成列（右边）"
./build/vidowmsg expel-window-from-column >/dev/null 2>&1
sleep 0.6
check "[a1] | [a2]" "$(shape)" "又是两条列、各一个窗口，新列在右边"
check "0" "$(col_field a1 index)" "a1 那条列还在原位（index=0）"
check "1" "$(col_field a2 index)" "a2 的新列在它右边（index=1）"
check "a2" "$(focused_title)" "焦点还在 a2 上（取出来不换焦点）"
check "0.3" "$(col_field a2 width_fraction)" "新列继承原来那条列的宽度"
check "0.3" "$(col_field a1 width_fraction)" "原来那条列宽度没变"
if grep -aq "取出窗口: a2 从列 #" "$LOG"; then
	pass "日志说了「取出窗口: a2 从列 #N 到新列 #M（右边）」"
else
	fail "日志里没有取出窗口的记录：$(grep -a '取出窗口' "$LOG" | tail -1)"
fi

# ------------------------------------------------------------ 3) 取到左边
mark "3) 取到左边（expel-window-from-column left）"
./build/vidowmsg move-window-direction -1 >/dev/null 2>&1
sleep 0.6
check "[a2 a1]" "$(shape)" "先再并成一列"
./build/vidowmsg expel-window-from-column --direction left >/dev/null 2>&1
sleep 0.6
check "[a2] | [a1]" "$(shape)" "取到左边：新列插在原来那条列左边"
check "0" "$(col_field a2 index)" "a2 的新列 index=0"
check "1" "$(col_field a1 index)" "a1 那条列往右挪了一位"

# ------------------------------------------------------------ 4) 取不出来的那些
mark "4) 取不出来的时候要报人话（不是默默什么都不做）"
ERR=$(./build/vidowmsg expel-window-from-column 2>&1 >/dev/null)
sleep 0.4
case "$ERR" in
*"就它一个"*)
	pass "列里只剩它一个：报「就它一个（本来就是单独一条列）」" ;;
*)
	fail "报的话不对：$ERR" ;;
esac
check "[a2] | [a1]" "$(shape)" "取不出来的时候列结构一点没动"

./build/vidowmsg toggle-floating >/dev/null 2>&1
sleep 0.6
check "true" "$(win_field a2 floating)" "a2 变成浮窗了"
ERR=$(./build/vidowmsg expel-window-from-column 2>&1 >/dev/null)
case "$ERR" in
*"浮窗"*)
	pass "浮窗：报「浮窗 / 弹出物」（浮窗本来就不在列里）" ;;
*)
	fail "浮窗报的话不对：$ERR" ;;
esac
./build/vidowmsg toggle-floating >/dev/null 2>&1
sleep 0.6

# ------------------------------------------------------------ 5) 动作串那条路
mark "5) 键位动作串走的是同一条路（vidowmsg action）"
./build/vidowmsg move-window-direction -1 >/dev/null 2>&1
sleep 0.6
check "[a2 a1]" "$(shape)" "再并成一列"
./build/vidowmsg action expel-window-from-column >/dev/null 2>&1
sleep 0.6
check "[a1] | [a2]" "$(shape)" "动作串取出来（默认右边）"
grep -aq "action: expel-window-from-column -> 把窗口从列里取出来" "$LOG" &&
	pass "日志里记了这条动作" || fail "日志里没有动作记录"

./build/vidowmsg move-window-direction -1 >/dev/null 2>&1
sleep 0.6
./build/vidowmsg action "expel-window-from-column left" >/dev/null 2>&1
sleep 0.6
check "[a2] | [a1]" "$(shape)" "动作串带 left 参数"

# ------------------------------------------------------------ 6) 收尾
mark "6) 收尾"
grep -aq "Assertion" "$LOG" || grep -aq "abort" "$LOG" &&
	fail "日志里有 assert / abort" || pass "日志里没有 assert / abort"
[ "$(win_id a1)" != "" ] && [ "$(win_id a2)" != "" ] &&
	pass "两个窗口都还活着" || fail "有窗口不见了"

echo
if [ "$FAILED" = 0 ]; then
	echo "test-expel.sh: OK"
else
	echo "test-expel.sh: 有失败项"
fi
exit $FAILED
