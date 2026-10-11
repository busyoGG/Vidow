#!/bin/bash
# 输入注入：IPC 能合成鼠标和键盘事件（move-cursor / pointer-button /
# pointer-scroll / send-key / type-text），等于「谁连上 socket 谁就能操控整个桌面」。
#
# 判据（headless 1280x720 一块屏 + wev 当观察者 —— 它是第三方客户端，
# 「事件真的到客户端了」由它说了算，不是合成器自己说）：
#   1) 注入键盘在 input 里：name=vidow-inject、inject=true；headless 下座位有
#      KEYBOARD 能力，客户端启动就能拿到 keymap + enter
#   2) move-cursor：指针焦点跟着走，wev 收到 enter/motion 的**窗口内坐标**
#      （窗口在 6,6，光标到 300,200 → 局部 294,194）
#   3) --relative：在当前位置上加
#   4) pointer-button：click / press / release 分别到位，按钮名和数字都认，连点
#   5) pointer-scroll：滚轮模式给 axis_value120 = 格数×120、delta = 格数×15；
#      连续模式不给 value120
#   6) send-key：Return / ctrl+shift+t 的**顺序**（修饰键先按、后松），
#      大写和符号自己按 Shift（"Hi!" 里 ! 是 Shift+1）
#   7) 注入的按键**不触发 vidow 自己的快捷键**（绑定 super+t 之后注入它，
#      overview 不开，而 wev 照样收到）
#   8) 注入的修饰键真的改了座位状态：按住 super 再滚轮 = 换列（不是滚客户端）
#   9) type-text 打中文给出「要走输入法」的明确错误
#  10) input_injection = false 之后全部拒绝，理由写清楚
#  11) 日志里没有 assert / abort
#
# 用法：./test-input-inject.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py / frames / wev。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-inject-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
WEVLOG="$RUNDIR/wev.log"
FAILED=0
VIDOW_PGID=""
VIDOW_PIDS=""
WEV_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*build/frames*|*build/xpopup*|*/wev*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

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

command -v wev >/dev/null || { echo "test-input-inject.sh: 需要 wev（wayland event viewer）"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-input-inject.sh: 先构建"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

write_config() { # <额外配置行>
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
scale = "1"
window_open_ms = 60
window_close_ms = 60
window_move_ms = 60
$1
-- 指针手势是键位（合成器不内置）：第 7 轮「按住 super 滚轮换列」那条断言要它
vidow.bind("Mod+wheel-up", "focus-column left")
vidow.bind("Mod+wheel-down", "focus-column right")
CFG
}

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

start_vidow() { # <额外配置行>
	export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
	write_config "$1"
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

start_wev() {
	rm -f "$WEVLOG"
	# stdbuf：wev 的输出重定向到文件时是块缓冲的，事件会攒一大块才落盘，
	# 断言就会「明明到了却说没收到」。行缓冲之后每条事件都能立刻读到。
	stdbuf -o0 -e0 wev > "$WEVLOG" 2>&1 &
	WEV_PID=$!
	sleep 1.2
}

msg() { ./build/vidowmsg --raw "$@" 2>&1; }

is_ok() { # stdin: JSON
	python3 -c 'import json,sys
try:
    print("true" if json.loads(sys.stdin.read()).get("ok") else "false")
except Exception:
    print("false")'
}

# wev 日志里某类事件的行（只留事件那一行，方便肉眼核对顺序）
wev_lines() { # <正则>
	grep -aE 'wl_(pointer|keyboard)' "$WEVLOG" | grep -aE "$1"
}
wev_count() { wev_lines "$1" | wc -l | tr -d ' '; }
wev_seq() { # 键盘事件的键码序列（按下/松开都算），一行一个
	wev_lines 'wl_keyboard\] key:' | sed -E 's/.*key: ([0-9]+); state: ([0-9]).*/\1:\2/'
}

# vidowmsg 成功退出 0，失败非 0（原因已经打到 stderr）—— 直接看退出码最省事
expect_ok() { # <说明> <命令…>
	desc="$1"; shift
	if out=$(./build/vidowmsg "$@" 2>&1); then
		pass "$desc"
	else
		fail "$desc —— $(printf '%s' "$out" | tr -d '\n' | head -c 160)"
	fi
}

expect_err() { # <说明> <命令…>
	desc="$1"; shift
	if out=$(./build/vidowmsg "$@" 2>&1); then
		fail "$desc（本该拒绝，却成功了）"
	else
		pass "$desc"
	fi
}

expect_wev() { # <说明> <正则> <最少次数>
	desc="$1"; pat="$2"; want="$3"
	n=$(wev_count "$pat")
	if [ "$n" -ge "$want" ]; then
		pass "$desc"
	else
		fail "$desc（wev 只收到 $n 条，期望 ≥$want）"
	fi
}

echo "=== 输入注入 ==="

mark "1. 启动：注入键盘 + 座位的键盘能力"
start_vidow ""
kb=$(msg input)
printf '%s' "$kb" | python3 -c '
import json,sys
d=json.load(sys.stdin)["result"]["keyboards"]
hit=[k for k in d if k["name"]=="vidow-inject"]
print("hit=%d inject=%s virtual=%s total=%d" % (
    len(hit), hit[0]["inject"] if hit else "-",
    hit[0]["virtual"] if hit else "-", len(d)))' > "$RUNDIR/kb.txt"
grep -q 'hit=1 inject=True virtual=False' "$RUNDIR/kb.txt" \
	&& pass "注入键盘在 input 里（name=vidow-inject / inject=true）" \
	|| fail "注入键盘没出现：$(cat "$RUNDIR/kb.txt")"
start_wev
grep -aq 'wl_keyboard\] keymap' "$WEVLOG" && pass "wev 拿到了 keymap（座位有 KEYBOARD 能力）" \
	|| fail "wev 没拿到 keymap（headless 下座位没有键盘能力？）"
grep -aq 'wl_keyboard\] enter' "$WEVLOG" && pass "wev 拿到键盘焦点" \
	|| fail "wev 没拿到键盘焦点"

mark "2. move-cursor：指针焦点跟着走（窗口内坐标）"
expect_ok "move-cursor 300 200" move-cursor 300 200
sleep 0.3
wev_lines 'wl_pointer\] enter' | grep -aq '294.000000, 194.000000' \
	&& pass "wev 收到 enter 在局部 294,194（窗口原点 6,6 + 光标 300,200）" \
	|| fail "enter 坐标不对：$(wev_lines 'wl_pointer\] enter' | tail -1)"
expect_ok "move-cursor 100 100 --relative" move-cursor 100 100 --relative
sleep 0.3
cur=$(msg cursor | python3 -c 'import json,sys;d=json.load(sys.stdin)["result"];print("%g,%g"%(d["x"],d["y"]))')
[ "$cur" = "400,300" ] && pass "相对移动后光标在 400,300（IPC 回读）" \
	|| fail "相对移动后光标是 $cur，期望 400,300"
wev_lines 'wl_pointer\] motion' | grep -aq '394.000000, 294.000000' \
	&& pass "wev 收到 motion 到局部 394,294" \
	|| fail "motion 坐标不对：$(wev_lines 'wl_pointer\] motion' | tail -1)"
expect_err "布局外的坐标要拒绝" move-cursor -50 -50

mark "3. pointer-button：click / press / release / 连点"
# 光标得在 wev 窗口里（窗口 6,6 起 634x708）—— 上一节结束时它在窗外
expect_ok "把光标挪回窗口里" move-cursor 300 200
expect_ok "pointer-button click" pointer-button click
sleep 0.3
expect_wev "wev 收到左键按下" 'button: 272 \(left\), state: 1' 1
expect_wev "wev 收到左键松开" 'button: 272 \(left\), state: 0' 1
expect_ok "pointer-button press right" pointer-button press right
expect_ok "pointer-button release right" pointer-button release right
sleep 0.3
expect_wev "右键按下" 'button: 273 \(right\), state: 1' 1
expect_wev "右键松开" 'button: 273 \(right\), state: 0' 1
before=$(wev_count 'button: 272 \(left\), state: 1')
expect_ok "pointer-button click left 2（连点）" pointer-button click left 2
sleep 0.4
after=$(wev_count 'button: 272 \(left\), state: 1')
[ "$((after - before))" -eq 2 ] && pass "连点两次都到了客户端" \
	|| fail "连点次数不对（新增 $((after - before)) 次按下，期望 2）"
expect_err "不认识的按钮名要拒绝" pointer-button click 侧键

mark "4. pointer-scroll：滚轮 / 连续"
expect_ok "pointer-scroll 0 -1（滚轮一格）" pointer-scroll 0 -1
sleep 0.3
expect_wev "滚轮事件带 axis_source=wheel" 'axis_source: 0 \(wheel\)' 1
wev_lines 'axis_value120' | grep -aq 'value120: -120' \
	&& pass "一格 = value120 -120" || fail "value120 不对：$(wev_lines axis_value120 | tail -1)"
wev_lines 'wl_pointer\] axis:' | grep -aq 'value: -15.000000' \
	&& pass "一格 = delta -15" || fail "delta 不对：$(wev_lines 'wl_pointer\] axis:' | tail -1)"
v120_before=$(wev_count 'axis_value120')
expect_ok "pointer-scroll 0 -3 --discrete false（连续）" pointer-scroll 0 -3 --discrete false
sleep 0.3
v120_after=$(wev_count 'axis_value120')
[ "$v120_after" -eq "$v120_before" ] \
	&& pass "连续滚动不带 value120（那是滚轮才有的）" \
	|| fail "连续滚动也发了 value120"
wev_lines 'wl_pointer\] axis:' | grep -aq 'value: -3.000000' \
	&& pass "连续滚动按原值给（-3）" || fail "连续滚动的值不对"
expect_err "dx/dy 全 0 要拒绝" pointer-scroll 0 0

mark "5. send-key：顺序、修饰键、大写与符号"
expect_ok "send-key tap Return" send-key tap Return
sleep 0.3
expect_wev "Return 到了（key 36 = 28+8）" 'key: 36; state: 1' 1
base=$(wev_seq | wc -l | tr -d ' ')
expect_ok "send-key tap ctrl+shift+t" send-key tap ctrl+shift+t
sleep 0.3
seq=$(wev_seq | tail -n +$((base + 1)) | tr '\n' ' ')
# 37=Control_L 50=Shift_L 28=t（都是 xkb 键码 = evdev + 8）
[ "$seq" = "37:1 50:1 28:1 28:0 50:0 37:0 " ] \
	&& pass "ctrl+shift+t 的顺序对（修饰键先按、后松）" \
	|| fail "ctrl+shift+t 的顺序是：$seq"
base=$(wev_seq | wc -l | tr -d ' ')
expect_ok 'type-text "Hi!"' type-text 'Hi!'
sleep 0.4
seq=$(wev_seq | tail -n +$((base + 1)) | tr '\n' ' ')
# H = Shift+43，i = 31，! = Shift+10（1 键）
[ "$seq" = "50:1 43:1 43:0 50:0 31:1 31:0 50:1 10:1 10:0 50:0 " ] \
	&& pass "Hi! 每个字符的 Shift 都自己按了" \
	|| fail "Hi! 的按键序列是：$seq"
expect_err "type-text 中文要给明确错误" type-text 中文
msg type-text 中文 | grep -aq '输入法' && pass "错误里说了要走输入法" \
	|| fail "中文的错误信息没说清楚"

mark "6. 注入的按键不触发 vidow 自己的快捷键"
stop_vidow
start_vidow 'vidow.bind("Mod+t", "toggle-overview")'
start_wev
ov=$(msg overview | python3 -c 'import json,sys;print(json.load(sys.stdin)["result"]["target"])')
[ "$ov" = "False" ] && pass "注入前 overview 是关的" || fail "注入前 overview 已经是开的"
base=$(wev_seq | wc -l | tr -d ' ')
expect_ok "send-key tap super+t" send-key tap super+t
sleep 0.4
ov=$(msg overview | python3 -c 'import json,sys;print(json.load(sys.stdin)["result"]["target"])')
[ "$ov" = "False" ] && pass "注入 super+t 没触发绑定（overview 还是关的）" \
	|| fail "注入的按键把 vidow 自己的快捷键触发了"
seq=$(wev_seq | tail -n +$((base + 1)) | tr '\n' ' ')
[ "$seq" = "133:1 28:1 28:0 133:0 " ] \
	&& pass "但客户端照样收到 super+t（133 = Super_L）" \
	|| fail "客户端收到的序列是：$seq"

mark "7. 注入的修饰键改了座位状态（按住 super 滚轮 = 换列）"
# 再来一个窗口，凑出两条列（换列才有意义）
./build/frames 注入二 20 >/dev/null 2>&1 &
sleep 1.5
# 光标回到 wev 窗口里：不然滚轮落到别的窗口上，「wev 没收到」就成了一句废话
expect_ok "把光标挪回 wev 窗口里" move-cursor 300 200
expect_ok "按住 super" send-key press super
v120=$(wev_count 'axis_value120')
expect_ok "super 按着时滚轮" pointer-scroll 0 -1
sleep 0.4
v120_after=$(wev_count 'axis_value120')
[ "$v120_after" -eq "$v120" ] \
	&& pass "滚轮没转发给客户端（被 Mod+滚轮换列吃掉了）" \
	|| fail "按住 super 滚轮还是转发给客户端了（座位上的修饰键没生效）"
expect_ok "松开 super" send-key release super
v120=$(wev_count 'axis_value120')
expect_ok "松开之后再滚一次" pointer-scroll 0 -1
sleep 0.4
v120_after=$(wev_count 'axis_value120')
[ "$v120_after" -gt "$v120" ] \
	&& pass "松开 super 之后滚轮又转发给客户端了" \
	|| fail "松开 super 之后滚轮还是没到客户端"

mark "8. input_injection = false"
stop_vidow
start_vidow "input_injection = false"
for cmd in "move-cursor 100 100" "pointer-button click" "pointer-scroll 0 -1" \
		"send-key tap Return" "type-text hi"; do
	expect_err "关掉之后 $cmd 被拒" $cmd
done
msg move-cursor 100 100 | grep -aq 'input_injection' \
	&& pass "拒绝理由里点了 input_injection" || fail "拒绝理由没说清楚"

mark "9. 日志干净"
grep -aqE 'assert|abort|Assertion' "$LOG" && fail "日志里有 assert/abort" \
	|| pass "日志里没有 assert / abort"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
