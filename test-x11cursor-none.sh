#!/bin/bash
# 普通 X11 窗口里的光标：set_cursor(NULL) 不能一律当成「游戏自己画光标」。
#
# 背景（5ee22c0）：root 光标换成 1x1 透明，好让全屏游戏 SetCursor(NULL) 之后
# 那支系统箭头不再被继承上去。代价是**普通 X11 窗口没设光标**时，X 沿继承链
# 落到 root 的透明占位，Xwayland 报上来的也是 NULL —— 和游戏那条路一模一样。
# 旧代码一律按「不画」处理，普通 X11 窗口里光标就整块消失（用户 2026-11-02
# 报「xwayland 应用光标不再发生变化」）。
#
# 判据（headless 1280x720、一块 1x 屏）：
#   1) 普通窗口、客户端没设光标 → 显示主题箭头（不是「不画」），日志有
#      「X11 窗口没设光标…→ 主题箭头」
#   2) 客户端换 hand2 → 光标图跟着换（普通窗口的光标变化照常生效）
#   3) 客户端再设 None（仍非全屏）→ 回到同一张主题箭头
#   4) 全屏窗口设 hand2 → 有光标；再设 None → image == 0x0（隐藏），而且
#      不再打「→ 主题箭头」那条日志（两条路真的分开了）
#
# 用法：./test-x11cursor-none.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-x11cursor-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
PROBE_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1" >&2; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$PROBE_PID" ] && kill "$PROBE_PID" 2>/dev/null
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	rm -f /tmp/vidow-cursor-trace
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/vidowmsg ] || { echo "先构建（ninja -C build）"; exit 1; }
[ -x ./build/x11cursorwin ] || { echo "先构建（ninja -C build）"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
xwayland = true
window_open_ms = 60
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run" HOME="$CFGDIR" VIDOW_LOG=debug
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

msg() { ./build/vidowmsg "$@" 2>&1; }

image() { # 当前光标图（"0x0" = 没画）
	msg cursor | python3 -c '
import json,sys
d=json.loads(sys.stdin.read())
if "result" in d: d=d["result"]
print(d.get("image"))
'
}

geo() { # "x y w h"（找不到就是 -1 -1 0 0）
	msg windows | python3 -c '
import json,sys
ws=json.load(sys.stdin)
if isinstance(ws,dict): ws=ws.get("windows",[])
for w in ws:
    if (w.get("title") or "")=="x11cursorwin":
        g=w.get("geometry") or {}
        print(g.get("x"),g.get("y"),g.get("width"),g.get("height"))
        break
else:
    print("-1 -1 0 0")
'
}

arrow_lines() { grep -ac "X11 窗口没设光标" "$LOG" 2>/dev/null || true; }

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
sleep 0.6

export DISPLAY=$(grep -ao "xwayland: DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)
[ -n "$DISPLAY" ] || { echo "拿不到 Xwayland 的 DISPLAY"; exit 1; }

# 光标链路探针（日志里才有 set_cursor 的账）
touch /tmp/vidow-cursor-trace

coproc PROBE { ./build/x11cursorwin 2>"$RUNDIR/probe.err"; }
read -r -t 5 -u "${PROBE[0]}" line || true
[ "$line" = "mapped" ] || { echo "x11cursorwin 没起来"; exit 1; }

for _ in $(seq 1 80); do
	[ "$(geo)" != "-1 -1 0 0" ] && break
	sleep 0.25
done
sleep 0.6
read -r X Y W H <<<"$(geo)"
[ "$W" -gt 0 ] || { echo "x11cursorwin 窗口没上屏"; tail -5 "$LOG"; exit 1; }
echo "  窗口在 $X,$Y ${W}x${H}"

probe_cmd() { # <hand|none>，等它把这一条处理完
	echo "$1" >&"${PROBE[1]}"
	read -r -t 5 -u "${PROBE[0]}" line || line=""
	[ "$line" = "$1" ] || fail "x11cursorwin 没回执：$1（拿到 '$line'）"
}

mark "1) 普通窗口、客户端没设光标 → 主题箭头"
msg move-cursor $((X + W / 2)) $((Y + H / 2)) >/dev/null
sleep 0.6
THEME=$(image)
[ "$THEME" != "0x0" ] && [ -n "$THEME" ] &&
	pass "光标没消失（主题箭头 $THEME）" ||
	fail "光标被藏起来了（image=$THEME）"
[ "$(arrow_lines)" -ge 1 ] &&
	pass "日志走了「X11 窗口没设光标…→ 主题箭头」那条" ||
	fail "日志里没有「→ 主题箭头」那条"

mark "2) 客户端换成 hand2 → 光标跟着换"
probe_cmd hand
sleep 0.5
HAND=$(image)
[ "$HAND" != "0x0" ] && [ "$HAND" != "$THEME" ] &&
	pass "换成了客户端给的光标（$THEME → $HAND）" ||
	fail "光标没跟着换（$THEME → $HAND）"

mark "3) 非全屏再设 None → 回到同一张主题箭头"
probe_cmd none
sleep 0.5
BACK=$(image)
[ "$BACK" = "$THEME" ] &&
	pass "回到主题箭头（$BACK）" ||
	fail "没回到主题箭头（期望 $THEME，拿到 $BACK）"

mark "4) 全屏窗口：hand2 有图，None 才隐藏"
msg focus-window --title x11cursorwin >/dev/null
msg set-fullscreen true >/dev/null
sleep 0.8
read -r FX FY FW FH <<<"$(geo)"
[ "$FW" -eq 1280 ] && [ "$FH" -eq 720 ] &&
	pass "窗口全屏了（$FX,$FY ${FW}x${FH}）" ||
	fail "窗口没全屏（$FX,$FY ${FW}x${FH}）"
probe_cmd hand
sleep 0.4
FULL_HAND=$(image)
[ "$FULL_HAND" != "0x0" ] &&
	pass "全屏下客户端的光标照常显示（$FULL_HAND）" ||
	fail "全屏下客户端的光标没显示（image=$FULL_HAND）"
BEFORE=$(arrow_lines)
probe_cmd none
sleep 0.5
FULL_NONE=$(image)
[ "$FULL_NONE" = "0x0" ] &&
	pass "全屏下设 None 才隐藏（image=0x0）" ||
	fail "全屏下没隐藏（image=$FULL_NONE）"
[ "$(arrow_lines)" -eq "$BEFORE" ] &&
	pass "隐藏那条没走「→ 主题箭头」（两条路分开了）" ||
	fail "隐藏时又打了「→ 主题箭头」"

mark "5) 日志干净"
grep -aqE "assert|abort" "$LOG" &&
	fail "日志里有 assert / abort" ||
	pass "日志里没有 assert / abort"

if [ "$FAILED" -eq 0 ]; then
	echo
	echo "test-x11cursor-none.sh: 全过"
else
	echo
	echo "test-x11cursor-none.sh: 有失败"
fi
exit "$FAILED"
