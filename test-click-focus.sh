#!/bin/bash
# 点另一块屏的**空区域**：键盘焦点要跟着过去。
#
# 背景（用户报的）：4K（DP-2，右）上随便点，动的却是左边 DP-3 的窗口。
# 原因：pointer button 只改 server->focused_output，server->focused 还停在
# 上一块屏的窗口上；之后的 close-window / focus-column 全作用在那边。
# 现在空点击会走 vidow_workspace_focus_output()：换 focused_output，并把焦点
# 交给那块屏活动工作区记住的窗口（没有就第一列；再没有就清空）。
#
# 判据（两块 headless：HEADLESS-1 1x 左，HEADLESS-2 2x 右）：
#   1) w1 在左、w2 在右，焦点先给 w1
#   2) 点右屏空区 → 焦点变 w2
#   3) 点左屏空区 → 焦点变 w1
#   4) 日志里没有 assert / abort
#
# 用法：./test-click-focus.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-click-focus-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }

cleanup() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "auto"
vidow.output{ name = "HEADLESS-1", scale = 1, x = 0, y = 0, mode = "1920x1080" }
vidow.output{ name = "HEADLESS-2", scale = 2, x = 1920, y = 0, mode = "3840x2160" }
primary_output = "HEADLESS-2"
window_open_ms = 0
window_move_ms = 0
vidow.rule{ app_id = "^pattern%.py$", title = "^w1$", output = "HEADLESS-1" }
vidow.rule{ app_id = "^pattern%.py$", title = "^w2$", output = "HEADLESS-2" }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-click-focus.sh: vidow 没起来"; tail -5 "$LOG"; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 1
exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }

focused_title() {
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    if w.get("focused"):
        print(w["title"]); break
'
}

wait_title() { # wait_title <期望>
	for _ in $(seq 1 30); do
		[ "$(focused_title)" = "$1" ] && return 0
		sleep 0.1
	done
	return 1
}

act "pointer-motion 500 600" 0.3
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do ./build/vidowmsg windows 2>/dev/null | grep -q '"w1"' && break; sleep 0.25; done
act "pointer-motion 2500 600" 0.3
python3 ./tools/pattern.py w2 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do ./build/vidowmsg windows 2>/dev/null | grep -q '"w2"' && break; sleep 0.25; done
sleep 0.5
./build/vidowmsg focus-window --title w1 >/dev/null 2>&1
wait_title w1 && pass "起点：焦点在左屏的 w1" || fail "起点焦点不是 w1：$(focused_title)"

# 右屏空区（w2 在 1926..，挑 2400,900 不会压到它）
act "pointer-motion 2400 900" 0.2
act "pointer-button left press" 0.2
act "pointer-button left release" 0.2
wait_title w2 && pass "点右屏空区 → 焦点换到右屏的 w2" ||
	fail "点右屏空区后焦点还是 $(focused_title)"

# 左屏空区（w1 在 6..，挑 1500,900）
act "pointer-motion 1500 900" 0.2
act "pointer-button left press" 0.2
act "pointer-button left release" 0.2
wait_title w1 && pass "点左屏空区 → 焦点换回左屏的 w1" ||
	fail "点左屏空区后焦点还是 $(focused_title)"

grep -aq "assert\|abort" "$LOG" && fail "日志里有 assert / abort" ||
	pass "日志里没有 assert / abort"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "test-click-focus.sh: 全部通过"
else
	echo "test-click-focus.sh: 有失败项"
fi
exit "$FAILED"
