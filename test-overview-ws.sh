#!/bin/bash
# 全景里换工作区：列表要跟着滚到新换的那一块。
#
# 现场（用户报的「能换屏幕了 但同一个屏幕的工作区还是不能换啊」）：全景开着的时候
# 用「换工作区」的绑定（默认配置 Alt+wheel-up/down = workspace up/down）切块，
# 工作区确实换过去了（state 里 active 变了），但**行表只在进全景那一刻滚过一次**
# （rows_scroll_to_active 只在 vidow_overview_toggle 里调用），列表还停在原来那几行
# —— 用户看到的画面几乎没变化，于是就报「不能换」。
#
# 修法：vidow_overview_workspace_changed()（src/overview.c）在全景期间切工作区时
# 重建行表 + 滚到新块，由 vidow_workspace_activate() 调用。
#
# 判据：
#   1) 四块工作区各一个窗口，进全景后行数 ≥ 4（列表放不下，滚才有意义）
#   2) 滚一格换到下一块：active 变、list_scroll 变大（列表跟着滚）
#   3) 再滚回来：active 回到原来那块、list_scroll 变小
#   4) 全程全景还开着
#   5) 无 ERROR
#
# 注：绑定写成不带 Mod 的 wheel-up/down —— 注入的键盘不被 pointer_modifiers() 认
# （见 src/input.c），带 Mod 的注入组合键到不了绑定表。
#
# 用法：./test-overview-ws.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-overview-ws-test
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

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
xwayland = false
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
window_open_ms = 0
window_move_ms = 0
vidow.bind("Mod+O", "toggle-overview")
vidow.bind("wheel-down", "workspace down")
vidow.bind("wheel-up", "workspace up")
CFG

v() { ./build/vidowmsg "$@" 2>&1; }
vmsg() { ./build/vidowmsg "$@"; }

# 打印 "active rows list_scroll"
ovstate() {
	v state | python3 -c '
import json, sys
d = json.load(sys.stdin)
ov = d["overview"]
act = [(w.get("name") or w["id"]) for w in d["workspaces"] if w.get("active")]
print("%s %d %.0f" % (act[0] if act else "-", ov["rows"], ov["list_scroll"]))
'
}

ovon() {
	v state | python3 -c '
import json, sys
print(1 if json.load(sys.stdin)["overview"]["active"] else 0)
'
}

wait_ready() {
	for _ in $(seq 1 60); do
		grep -aq "vidow ready" "$LOG" && return 0
		sleep 0.25
	done
	return 1
}

mark "起一个 headless 合成器"
XDG_RUNTIME_DIR="$RUNDIR/run" HOME="$CFGDIR" WLR_BACKENDS=headless \
	WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=gles2 VIDOW_LOG=info \
	setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
wait_ready || { echo "  vidow 没起来"; exit 1; }
export XDG_RUNTIME_DIR="$RUNDIR/run" HOME="$CFGDIR"
export WAYLAND_DISPLAY
WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
pass "合成器起来了（WAYLAND_DISPLAY=$WAYLAND_DISPLAY）"

mark "四块工作区各放一个窗口"
for i in 1 2 3 4; do
	if [ "$i" -gt 1 ]; then
		vmsg create-workspace "wsn$i" > /dev/null 2>&1
		sleep 0.4
		vmsg focus-workspace "wsn$i" > /dev/null 2>&1
		sleep 0.4
	fi
	./build/frames "ov$i" 30 > /dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 1.0
done
n_ws=$(v state | python3 -c '
import json, sys
d = json.load(sys.stdin)
print(len([w for w in d["workspaces"] if (w.get("windows") or [])]))
')
if [ "$n_ws" -ge 4 ]; then
	pass "有 $n_ws 块工作区带窗口"
else
	fail "只有 $n_ws 块工作区带窗口（要 ≥4，否则列表放得下、滚不动）"
fi

vmsg focus-workspace wsn1 > /dev/null 2>&1
sleep 0.6
vmsg move-cursor 640 360 > /dev/null 2>&1
sleep 0.3
vmsg toggle-overview > /dev/null 2>&1
sleep 1.2

read -r act0 rows0 scroll0 <<< "$(ovstate)"
if [ "$rows0" -ge 4 ]; then
	pass "进全景：$rows0 行（放不下一屏，能滚）"
else
	fail "进全景只有 $rows0 行（要 ≥4）"
fi

mark "滚一格换工作区：列表要跟着滚"
vmsg pointer-scroll 0 1 > /dev/null 2>&1
sleep 1.0
read -r act1 rows1 scroll1 <<< "$(ovstate)"
if [ "$act1" != "$act0" ]; then
	pass "工作区换到 $act1（原来是 $act0）"
else
	fail "滚一格工作区没换（还是 $act0）"
fi
if [ "$scroll1" -gt "$scroll0" ]; then
	pass "列表跟着滚：$scroll0 -> $scroll1"
else
	fail "列表没跟着滚：$scroll0 -> $scroll1（新换的那块在屏幕外，用户看到的就是「没反应」）"
fi

mark "滚回来：列表要滚回去"
vmsg pointer-scroll 0 -1 > /dev/null 2>&1
sleep 1.0
read -r act2 rows2 scroll2 <<< "$(ovstate)"
if [ "$act2" = "$act0" ]; then
	pass "工作区回到 $act0"
else
	fail "工作区没回到 $act0（现在是 $act2）"
fi
if [ "$scroll2" -lt "$scroll1" ]; then
	pass "列表滚回去：$scroll1 -> $scroll2"
else
	fail "列表没滚回去：$scroll1 -> $scroll2"
fi

if [ "$(ovon)" = "1" ]; then
	pass "全程全景还开着"
else
	fail "全景被关掉了（换工作区不该退出全景）"
fi

if grep -aq "ERROR" "$LOG"; then
	fail "日志里有 ERROR：$(grep -a ERROR "$LOG" | head -1)"
else
	pass "无 ERROR"
fi

echo
if [ "$FAILED" -eq 0 ]; then
	echo "test-overview-ws.sh: OK"
else
	echo "test-overview-ws.sh: 有失败项"
fi
exit "$FAILED"
