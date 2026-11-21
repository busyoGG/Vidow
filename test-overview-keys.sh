#!/bin/bash
# 两屏全景：快捷键作用在**鼠标所在那块屏**的窗口焦点上。
#
# 现场（用户报的「overview只能在进入overview的工作区使用super+滚轮快捷键，
# 我想要鼠标在哪个工作区就能在哪个工作区使用」/「overview 情况下确实是不能
# 改变工作区焦点，这是确切的 bug」）：进来那一刻 focused_output 定在当时的屏
# 上，之后 Super+滚轮（默认配置里是 focus-column left/right）全都作用在**那块
# 屏的窗口**上，鼠标挪到另一块屏按就没有反应。
#
# 根因不是 focused_output —— pointer motion 早就把它换成光标那块屏了；真正不
# 跟的是「键盘焦点在哪个工作区」：`server->focused->ws`。`vidow_focus_column()`
# 拿的是 `server->focused->column`，所以在 overview 里永远是进来那块工作区的列。
# 修法：`vidow_overview_follow_pointer()`（src/overview.c）在按键 / 滚轮进来之
# 前把键盘焦点交给光标那块屏现在亮着的工作区（vidow_workspace_focus_output）。
#
# 判据：
#   1) 进全景前焦点窗在左屏；光标挪到右屏（不按键）焦点窗不动
#   2) 右屏上滚一格：焦点窗变成**右屏**的窗口（老代码留在左屏的 wL1 上）
#   3) 光标挪回左屏再滚一格：焦点窗回到左屏
#   4) 全程无 ERROR
#
# 注：测试里的绑定写成不带 Mod 的 `wheel-down`，因为注入的键盘不被
# pointer_modifiers() 认（它只看真键盘，见 src/input.c），按 Mod 注入的组合键
# 到不了绑定表。滚轮方向与动作的对应关系和用户配置一致（滚一格 = 向右/左换列）。
#
# 用法：./test-overview-keys.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-overview-keys-test
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
vidow.output{ name = "HEADLESS-2", scale = 1, mode = "1280x720" }
window_open_ms = 0
window_move_ms = 0
vidow.bind("Mod+O", "toggle-overview")
-- 注入的键盘不被 pointer_modifiers() 认，所以测试里不按 Mod（见文件头注释）
vidow.bind("wheel-down", "focus-column left")
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
sleep 0.5

msg() { ./build/vidowmsg "$@" >/dev/null 2>&1; }
focused() {
	./build/vidowmsg windows 2>/dev/null | python3 -c \
'import json,sys;
w=[x for x in json.load(sys.stdin) if x.get("focused")];
print("%s@%s" % (w[0]["title"], w[0]["output"]) if w else "none")'
}
win_on() {   # win_on 标题 屏名 → 开一个窗口并挪到那块屏
	./build/frames "$1" 30 > /dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 1.0
	msg move-window-to-output "$2"
	sleep 0.4
}

mark "1. 两块屏各摆两列窗口"
win_on wL1 HEADLESS-2
win_on wL2 HEADLESS-2
win_on wR1 HEADLESS-1
win_on wR2 HEADLESS-1
sleep 0.5
LEFT=$(./build/vidowmsg windows 2>/dev/null | python3 -c 'import json,sys;
print(sorted({x["output"] for x in json.load(sys.stdin) if x["title"].startswith("wL")})[0])')
[ "$LEFT" = "HEADLESS-2" ] && pass "左屏的窗口在 HEADLESS-2 上" \
	|| fail "左屏窗口落在 $LEFT 上（期望 HEADLESS-2）"

mark "2. 进全景，光标在左屏 → 焦点窗在左屏"
msg move-cursor 640 360
sleep 0.3
msg toggle-overview
sleep 1.0
F0=$(focused)
case "$F0" in
	wL*@HEADLESS-2) pass "进全景时焦点窗是 $F0" ;;
	*) fail "进全景时焦点窗是 $F0（期望 wL*@HEADLESS-2）" ;;
esac

mark "3. 光标挪到右屏（不按键）：焦点窗不许跟着跑"
msg move-cursor 1900 500
sleep 0.5
F1=$(focused)
[ "$F1" = "$F0" ] && pass "光挪不改窗口焦点（仍是 $F1）" \
	|| fail "光挪指针就换了焦点：$F0 -> $F1"

mark "4. 右屏上滚一格（wheel-down = focus-column left）：焦点要落到右屏"
msg pointer-scroll 0 1
sleep 0.8
F2=$(focused)
case "$F2" in
	wR*@HEADLESS-1) pass "快捷键作用在鼠标那块屏：$F2" ;;
	*) fail "快捷键还是作用在进来那块屏：$F2（期望 wR*@HEADLESS-1）" ;;
esac

mark "5. 光标挪回左屏再滚一格：焦点回到左屏"
msg move-cursor 300 500
sleep 0.4
msg pointer-scroll 0 1
sleep 0.8
F3=$(focused)
case "$F3" in
	wL*@HEADLESS-2) pass "挪回左屏后作用在左屏：$F3" ;;
	*) fail "挪回左屏后焦点是 $F3（期望 wL*@HEADLESS-2）" ;;
esac

mark "6. 全程没有 ERROR"
if grep -aq "\[ERROR\]" "$LOG"; then
	fail "日志里有 ERROR：$(grep -a "\[ERROR\]" "$LOG" | head -2 | cut -c1-90)"
else
	pass "没有 ERROR"
fi

echo
if [ "$FAILED" -ne 0 ]; then
	echo "test-overview-keys.sh: 有失败项"
	exit 1
fi
echo "test-overview-keys.sh: OK"
