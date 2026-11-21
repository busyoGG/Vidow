#!/bin/bash
# 全景里「换工作区」和「换列」都要作用在**鼠标指着的那块工作区**上。
#
# 现场（用户 2026-11-22 报的两句）：
#   1) 「能换屏幕了 但同一个屏幕的工作区还是不能换」—— 换块的动作其实生效了
#      （state 里 active 变了），但行表只在进全景那一刻滚过一次
#      （rows_scroll_to_active 只在 vidow_overview_toggle 里调用），列表还停在原来
#      那几行，用户看到的就是「没反应」。
#   2) 「DP-3 两个工作区，只有一个工作区能够 super+滚轮」—— 快捷键（默认配置里
#      Alt+wheel = workspace up/down，Mod+wheel = focus-column）以前一律作用在
#      **该屏当前亮着的那块**工作区上，鼠标指着另一块的行也不理。
#
# 修法：
#   - vidow_overview_workspace_changed()（src/overview.c）：全景期间切工作区时重建
#     行表 + 滚到新块，由 vidow_workspace_activate() 调用。
#   - vidow_overview_follow_pointer() 里加 overview_row_ws_at()：光标压在哪一行，就先
#     把那一行的工作区端出来（等价于点它那一行里的窗口），快捷键再作用上去。
#
# 判据：
#   A1) 两行放得下时：光标在第一行、滚一格 → 亮着的工作区换成第一行那块，焦点落到
#       第一行的窗口上（老代码会留在第二行那块里，只在它内部换列）
#   A2) 光标挪到第二行再滚 → 回到第二行那块
#   B1) 四块工作区各一个窗口（行数超一屏）：进全景行数 ≥4
#   B2) 滚一格换块：active 变、list_scroll 变大（列表跟着滚）
#   B3) 再滚一格：又换一块、list_scroll 不回退
#   C1) 全程全景还开着
#   C2) 无 ERROR
#
# 注：绑定写成不带 Mod 的 wheel-up/down —— 注入的键盘不被 pointer_modifiers() 认
# （见 src/input.c），带 Mod 的注入组合键到不了绑定表。滚一格 = 换一列/换一块。
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
vidow.bind("wheel-up", "focus-column left")
vidow.bind("wheel-down", "workspace down")
CFG

v() { ./build/vidowmsg "$@" 2>&1; }
vmsg() { ./build/vidowmsg "$@" > /dev/null 2>&1; }
spawn() { ./build/frames "$1" 30 > /dev/null 2>&1 & CLIENT_PIDS="$CLIENT_PIDS $!"; sleep 1.0; }
focus_ws() { vmsg focus-workspace "$1"; sleep 0.5; }

# 当前亮着的工作区名 / 焦点窗口标题
active_ws() {
	v state | python3 -c '
import json, sys
d = json.load(sys.stdin)
a = [(w.get("name") or w["id"]) for w in d["workspaces"] if w.get("active")]
print(a[0] if a else "-")
'
}
focused_title() {
	v state | python3 -c '
import json, sys
d = json.load(sys.stdin)
w = (d.get("focused") or {}).get("window") or {}
print(w.get("title") or "-")
'
}
ov_field() {
	v state | python3 -c '
import json, sys
d = json.load(sys.stdin)["overview"]
print(int(d["rows"]), int(d["list_scroll"]), 1 if d["active"] else 0)
'
}

mark "起一个 headless 合成器"
XDG_RUNTIME_DIR="$RUNDIR/run" HOME="$CFGDIR" WLR_BACKENDS=headless \
	WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=gles2 VIDOW_LOG=info \
	setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
ready=0
for _ in $(seq 1 60); do
	grep -aq "vidow ready" "$LOG" && { ready=1; break; }
	sleep 0.25
done
[ "$ready" = 1 ] || { echo "  vidow 没起来"; exit 1; }
export XDG_RUNTIME_DIR="$RUNDIR/run" HOME="$CFGDIR"
export WAYLAND_DISPLAY
WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
pass "合成器起来了（WAYLAND_DISPLAY=$WAYLAND_DISPLAY）"

mark "两块工作区、各两个窗口（两行放得下一屏）"
spawn s1
spawn s2
vmsg create-workspace ws2
sleep 0.5
focus_ws ws2
spawn t1
spawn t2
first_ws=$(v state | python3 -c '
import json, sys
d = json.load(sys.stdin)
print([(w.get("name") or w["id"]) for w in d["workspaces"]][0])
')
if [ "$(active_ws)" = "ws2" ]; then
	pass "现在亮着的是 ws2（第一块是 $first_ws）"
else
	fail "现在亮着的不是 ws2（是 $(active_ws)）"
fi

vmsg move-cursor 640 360
sleep 0.3
vmsg toggle-overview
sleep 1.3
rows=$(ov_field | cut -d' ' -f1)
if [ "$rows" -ge 2 ]; then
	pass "全景里 $rows 行（每块工作区一行）"
else
	fail "全景里只有 $rows 行（要 ≥2）"
fi

mark "光标压在第一行（另一块工作区）上滚：要切到那一块"
vmsg move-cursor 640 120
sleep 0.5
vmsg pointer-scroll 0 -1
sleep 1.0
if [ "$(active_ws)" = "$first_ws" ]; then
	pass "亮着的换成第一行那块（$first_ws）"
else
	fail "亮着的还是 $(active_ws)，没换到第一行那块（$first_ws）"
fi
title=$(focused_title)
case "$title" in
s*) pass "焦点落在第一行的窗口上（$title）" ;;
*) fail "焦点没落在第一行的窗口上（$title）" ;;
esac

mark "光标挪到第二行再滚：要回第二行那块"
vmsg move-cursor 640 560
sleep 0.5
vmsg pointer-scroll 0 -1
sleep 1.0
if [ "$(active_ws)" = "ws2" ]; then
	pass "亮着的回到第二行那块（ws2）"
else
	fail "亮着的是 $(active_ws)，没回第二行那块（ws2）"
fi
title=$(focused_title)
case "$title" in
t*) pass "焦点落在第二行的窗口上（$title）" ;;
*) fail "焦点没落在第二行的窗口上（$title）" ;;
esac

mark "列表超一屏时：换块要跟着滚"
vmsg move-cursor 640 360
sleep 0.3
vmsg toggle-overview
sleep 1.0
for i in 3 4 5 6; do
	vmsg create-workspace "wsn$i"
	sleep 0.4
	focus_ws "wsn$i"
	spawn "ov$i"
done
focus_ws wsn3
vmsg toggle-overview
sleep 1.3
read -r rows0 scroll0 _ <<< "$(ov_field)"
if [ "$rows0" -ge 4 ]; then
	pass "进全景：$rows0 行（一屏放不下，能滚）"
else
	fail "进全景只有 $rows0 行（要 ≥4，否则行放得下、滚不动）"
fi
vmsg move-cursor 640 360
sleep 0.3
vmsg pointer-scroll 0 1
sleep 1.2
read -r rows1 scroll1 _ <<< "$(ov_field)"
act1=$(active_ws)
if [ "$act1" != "wsn3" ]; then
	pass "换到下一块（$act1）"
else
	fail "滚一格工作区没换（还是 wsn3）"
fi
if [ "$scroll1" -gt "$scroll0" ]; then
	pass "列表跟着滚：$scroll0 -> $scroll1"
else
	fail "列表没跟着滚：$scroll0 -> $scroll1（新块在屏幕外，看着像「没反应」）"
fi
vmsg pointer-scroll 0 1
sleep 1.2
read -r rows2 scroll2 _ <<< "$(ov_field)"
act2=$(active_ws)
if [ "$act2" != "$act1" ]; then
	pass "再滚一格又换了一块（$act2）"
else
	fail "再滚一格没换块（还是 $act1）"
fi
if [ "$scroll2" -ge "$scroll1" ]; then
	pass "列表一直跟着往下滚：$scroll0 -> $scroll1 -> $scroll2"
else
	fail "列表回退了：$scroll1 -> $scroll2"
fi

mark "收尾检查"
read -r _ _ ovon <<< "$(ov_field)"
if [ "$ovon" = 1 ]; then
	pass "全程全景还开着"
else
	fail "全景被关掉了（换工作区 / 换列都不该退出全景）"
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
