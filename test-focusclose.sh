#!/bin/bash
# 关掉一个窗口之后，焦点该落到谁身上。
#
# 靶子：niri 的规矩是「位置不动」—— 谁滑到空出来的那个位置上，焦点就是谁：
#   1) 列里还有窗口 → 关掉那个的位置由它下面那个顶上（关的是最下面那个就往上找）
#   2) 整列没了     → 右边最近的那一列顶上
#   3) 右边没有了   → 才往左找最近的那一列（关掉最右边那列时）
# 以前这里只做 1 的一半（只看下面），剩下全丢给 focus_nearest_column()，而那个是
# 「从左往右找第一个」—— 于是关掉中间那个窗口，焦点蹦到最左边去了。
#
# 判据（a | b | c 三列，从左到右）：
#   1) 三个窗口真的排成 a 左 b 中 c 右（不然下面的断言没意义）
#   2) 关掉中间的 b（焦点在 b 上）→ 焦点是**右边**的 c，不是最左边的 a  ← 主靶子
#   3) 关掉最右边的 c（焦点在 c 上）→ 焦点是左边的 a（右边没人了才往左）
#   4) 列里叠两个（d 上 e 下）：关掉上面的 d → 焦点是下面的 e（列内往下顶）
#   5) 浮窗（不在列里）：关掉它 → 焦点是右边最近的那个（按位置算）；平铺关完了
#      还剩浮窗 → 焦点落到浮窗上，不是把键盘清空
#   6) 关掉最后一个 → 焦点清空（focused.window 是 null）
#   7) 全程 vidow 不崩（关窗口走 unmap / destroy 两条路，两条都测）
#
# 用法：./test-focusclose.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-focusclose-test
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

python3 -c "import gi" 2>/dev/null || { echo "test-focusclose.sh: 需要 python3-gobject"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-focusclose.sh: 先 ninja -C build"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
outputs = "HEADLESS-1:1"
window_close_ms = 0
CFG

BIN=${VIDOW_BIN:-./build/vidow}
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-focusclose.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

alive() { kill -0 "$VIDOW_PID" 2>/dev/null; }
msg() { timeout 5 ./build/vidowmsg "$@" 2>/dev/null; }

# 焦点窗口的标题（没窗口就是 (none)）
focused_title() {
	msg focused | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    print("?"); raise SystemExit
w = d.get("window")
print(w["title"] if w else "(none)")'
}
# 某个窗口的 x（左边缘），用来确认三列真的从左到右排着
win_x() {
	msg windows | python3 -c '
import sys, json
want = sys.argv[1]
for w in json.load(sys.stdin):
    if w["title"] == want:
        print(w["geometry"]["x"]); raise SystemExit
print(-1)' "$1"
}
# 等焦点变成某个标题（最多等 4 秒）
wait_focus() {
	local want="$1" i
	for i in $(seq 1 16); do
		[ "$(focused_title)" = "$want" ] && return 0
		alive || return 1
		sleep 0.25
	done
	return 1
}
open_win() {
	setsid nohup python3 ./tools/pattern.py "$1" > "$RUNDIR/$1.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 1.6
}
expect_focus() { # <期望标题> <说明>
	if wait_focus "$1"; then
		pass "$2（焦点 = $1）"
	else
		local got
		got=$(focused_title)
		fail "$2 —— 期望 $1，实际是 $got"
	fi
}

mark "1/5 三个窗口排成 a | b | c"
open_win a
open_win b
open_win c
XA=$(win_x a); XB=$(win_x b); XC=$(win_x c)
if [ "$XA" -lt "$XB" ] && [ "$XB" -lt "$XC" ]; then
	pass "三列从左到右是 a($XA) < b($XB) < c($XC)"
else
	fail "三列没排成 a < b < c（$XA / $XB / $XC）：这条测试测不到东西"
fi
expect_focus c "新窗口开在右边，焦点跟着它"

mark "2/5 关掉中间的 b：焦点该落到右边的 c（不是最左边的 a）"
msg focus-window-direction -1 >/dev/null # c → b
expect_focus b "焦点先挪到中间的 b"
msg close-window --title b >/dev/null
expect_focus c "关掉 b 之后焦点是右边的 c（旧代码会蹦到最左边的 a）"

mark "3/5 关掉最右边的 c：右边没人了才往左找"
# 这条走 destroy 路（客户端被 kill），上面那条走 unmap 路（客户端自己退）
CPID=""
for p in $CLIENT_PIDS; do
	[ -r "/proc/$p/cmdline" ] || continue
	{ tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null || true; } | grep -q "pattern.py c" && CPID=$p
done
if [ -n "$CPID" ]; then
	kill "$CPID" 2>/dev/null
	pass "c 的客户端已 kill（走 destroy 那条路）"
else
	fail "没找到 c 的客户端进程"
fi
expect_focus a "关掉最右边的 c 之后焦点是左边的 a"

mark "4/5 列里叠两个：关掉下面那个 d，焦点往上顶到 e"
open_win d
open_win e
expect_focus e "e 开在右边，焦点在 e 上"
msg move-window-direction -1 >/dev/null # e 并进 d 那一列（插在 d 上面）
sleep 0.4
expect_focus e "e 并进 d 的列之后焦点还是 e（e 在上面）"
XD2=$(win_x d); XE2=$(win_x e)
if [ "$XD2" = "$XE2" ]; then
	pass "d 和 e 现在同一列（x 都是 $XD2）"
else
	fail "d 和 e 没叠成一列（x = $XD2 / $XE2）"
fi
msg focus-window-direction 1 >/dev/null # 列内往下 → d（这列的底）
expect_focus d "列内往下挪一格，焦点到下面的 d"
msg close-window --title d >/dev/null
expect_focus e "关掉下面的 d，焦点往上顶到同列的 e（旧代码会蹦到最左边的 a）"
msg close-window --title e >/dev/null
expect_focus a "e 也没了，焦点落到左边剩下的 a"

mark "5/5 浮窗：不在列里，但「往右」这条规矩一样算"
open_win f1
open_win f2
expect_focus f2 "f2 开在右边，焦点在 f2 上"
msg toggle-floating --title f1 >/dev/null # f1 变浮窗，停在自己原来的位置上
sleep 0.6
msg focus-window --title f1 >/dev/null
expect_focus f1 "焦点挪到浮窗 f1 上"
XF1=$(win_x f1); XF2=$(win_x f2)
if [ "$XF1" -lt "$XF2" ]; then
	pass "浮窗 f1($XF1) 在 f2($XF2) 左边"
else
	fail "浮窗 f1($XF1) 不在 f2($XF2) 左边：这条测不到「右边最近」"
fi
msg close-window --title f1 >/dev/null
expect_focus f2 "关掉浮窗 f1，焦点是右边最近的 f2（旧代码会蹦到最左边的 a）"
msg toggle-floating --title f2 >/dev/null # 现在只剩 a 一个平铺 + f2 一个浮窗
sleep 0.6
msg focus-window --title a >/dev/null
expect_focus a "焦点挪到平铺的 a 上"
msg close-window --title a >/dev/null
expect_focus f2 "平铺关完了，焦点落到剩下的浮窗 f2 上（旧代码会把键盘清空）"
msg close-window --title f2 >/dev/null
sleep 0.8
LAST=$(focused_title)
if [ "$LAST" = "(none)" ]; then
	pass "最后一个窗口关掉之后焦点清空（focused.window = null）"
else
	fail "窗口都没了，焦点还指着 $LAST"
fi

mark "收尾"
alive && pass "全程 vidow 没崩" || fail "vidow 没了"
if [ "$FAILED" != 0 ]; then
	echo
	echo "--- 日志里的焦点轨迹 ---"
	grep -a "焦点:" "$LOG" | tail -10
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "test-focusclose.sh: 全部通过"
else
	echo "test-focusclose.sh: 有失败项"
fi
exit $FAILED
