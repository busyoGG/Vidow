#!/bin/bash
# 普通 X11 窗口里的光标：set_cursor(NULL) 不能一律当成「游戏自己画光标」；
# 而且 X 光标图是按设备像素上屏的（2 倍屏上不能再乘一次，也不能被硬件光标
# 平面裁掉）。
#
# 背景（5ee22c0）：root 光标换成 1x1 透明，好让全屏游戏 SetCursor(NULL) 之后
# 那支系统箭头不再被继承上去。代价是**普通 X11 窗口没设光标**时，X 沿继承链
# 落到 root 的透明占位，Xwayland 报上来的也是 NULL —— 和游戏那条路一模一样。
# 旧代码一律按「不画」处理，普通 X11 窗口里光标就整块消失（用户 2026-11-02
# 报「xwayland 应用光标不再发生变化」）。
#
# 第二个坑：X 光标图**没有**自带倍率。Xwayland 不开 -hidpi 时 cursor surface
# 的 buffer_scale 恒为 1，而 X 像素就是这边的**设备像素**（X11 窗口的缓冲也是
# 这么声明的：500x400 的 X 缓冲 → 500x400 设备像素）。所以 X11 光标得按设备
# 像素上屏（wlr_cursor_set_buffer，密度 = xwayland_scale）。
#
# b623d68 曾把它改成交给 wlr_cursor_set_surface（按表面自己的 scale 当逻辑
# 尺寸），2 倍屏上整张图被乘 2：64px 的 X 光标变成 128 设备像素，比 Wayland
# 主题光标大一倍；更糟的是硬件光标平面只收得下 64x64，wlroots 把 128 的
# dst_box 画进 64 的缓冲 —— 用户 2026-11-03 报的「光标被裁切」就是这个。
#
# 判据：
#   1) 普通窗口、客户端没设光标 → 显示主题箭头（不是「不画」），日志有
#      「X11 窗口没设光标…→ 主题箭头」
#   2) 客户端换 hand2 → 光标图跟着换（普通窗口的光标变化照常生效）
#   3) 客户端再设 None（仍非全屏）→ 回到同一张主题箭头
#   4) 全屏窗口设 hand2 → 有光标；再设 None → image == 0x0（隐藏），而且
#      不再打「→ 主题箭头」那条日志（两条路真的分开了）
#   5) 2 倍屏上 X11 光标按设备像素上屏：同一支 16x16 的 hand2 还是 16x16
#      （不乘 2、也不再除成 8x8），热点按设备像素报
#   6) 大光标 / 连续换图：64 / 32 / 160 px 的方块光标都得原样上屏（image 和
#      texture 一样大、不被 ×2），而且**每一张**都要生效（Xwayland 等 frame
#      回调，漏发就只有第一张能上屏）
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

start_probe() { # 起窗口 + 等它上屏；结果留在 X Y W H
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
}

stop_probe() {
	[ -n "$PROBE_PID" ] && kill "$PROBE_PID" 2>/dev/null
	PROBE_PID=""
	sleep 0.2
}

start_vidow() { # <缩放>
	cat > "$VIDOW_CONFIG_LUA" <<CFG
scale = "$1"
xwayland = true
window_open_ms = 60
CFG
	rm -f "$LOG"
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
}

stop_vidow() {
	stop_probe
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
		VIDOW_PGID=""
	fi
}

probe_cmd() { # <hand|none>，等它把这一条处理完
	echo "$1" >&"${PROBE[1]}"
	read -r -t 5 -u "${PROBE[0]}" line || line=""
	[ "$line" = "$1" ] || fail "x11cursorwin 没回执：$1（拿到 '$line'）"
}

start_vidow 1
start_probe

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

# 2 倍屏：X 光标 pixmap 的像素就是设备像素（和 X11 窗口缓冲同一个约定）。
# 同一支 16x16 的 hand2，1 倍屏和 2 倍屏报出来的都该是 16x16 —— 不是 32x32
# （那是当逻辑尺寸又乘了一次），也不是 8x8（旧代码按密度除两次）。
mark "6) 2 倍屏：X11 光标按设备像素上屏（不乘 2）"
stop_vidow
start_vidow 2
start_probe
msg move-cursor $((X + W / 2)) $((Y + H / 2)) >/dev/null
sleep 0.6
probe_cmd hand
sleep 0.5
HAND2=$(image)
[ "$HAND2" = "$HAND" ] &&
	pass "2 倍屏上 hand2 还是 $HAND2（1 倍屏也是 $HAND；X 像素=设备像素）" ||
	fail "2 倍屏上 hand2 是 $HAND2（期望和 1 倍屏一样 $HAND；乘 2 或除 2 都不对）"
[ "$(arrow_lines)" -ge 1 ] &&
	pass "2 倍屏上「→ 主题箭头」那条照常走" ||
	fail "2 倍屏上没走「→ 主题箭头」"

# 大光标 + 连续换图：桌面光标的尺寸本来就是主题给的 32（这里用 64 的 X
# pixmap 模拟 Xft.dpi 已放大过的主题光标），上屏必须一张不多不少。热点
# 8,8 也要按设备像素落回 8,8（按密度除一次、上屏再乘回输出倍率）。
mark "7) 大光标 / 连续换图：原样上屏、每张都生效"
probe_cmd "box 64 8 8"
sleep 0.5
BOX64=$(image)
HOT=$(msg cursor | python3 -c '
import json,sys
d=json.load(sys.stdin)
if "result" in d: d=d["result"]
print("%s,%s" % (d.get("hotspot_x"), d.get("hotspot_y")))
')
[ "$BOX64" = "64x64" ] &&
	pass "64px 的 X 光标是 64x64（没被 ×2 成 128）" ||
	fail "64px 的 X 光标是 $BOX64（期望 64x64）"
[ "$HOT" = "8,8" ] &&
	pass "热点按设备像素落回 8,8" ||
	fail "热点是 $HOT（期望 8,8）"
probe_cmd "box 32 0 0"
sleep 0.5
BOX32=$(image)
[ "$BOX32" = "32x32" ] &&
	pass "换成 32px 立刻生效（frame 回调没断）" ||
	fail "换成 32px 没生效（拿到 $BOX32；只有第一张上屏？）"
probe_cmd "box 160 0 0"
sleep 0.5
BOX160=$(image)
TEX=$(msg cursor | python3 -c '
import json,sys
d=json.load(sys.stdin)
if "result" in d: d=d["result"]
print("%sx%s" % (d.get("texture_width"), d.get("texture_height")))
')
[ "$BOX160" = "160x160" ] && [ "$TEX" = "160x160" ] &&
	pass "160px 的方块整张上屏（image 和 texture 都是 160x160，不会被光标平面裁）" ||
	fail "160px 方块 image=$BOX160 texture=$TEX（期望都 160x160）"

if [ "$FAILED" -eq 0 ]; then
	echo
	echo "test-x11cursor-none.sh: 全过"
else
	echo
	echo "test-x11cursor-none.sh: 有失败"
fi
exit "$FAILED"
