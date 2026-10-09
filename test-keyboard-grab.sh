#!/bin/bash
# 键盘那两个协议：zwp_keyboard_shortcuts_inhibit_manager_v1（客户端说「别吃我的
# 快捷键」）+ zwp_xwayland_keyboard_grab_manager_v1（Xwayland 替 X11 的
# XGrabKeyboard 要键盘）。
#
# 为什么是这两个：`strings /usr/bin/Xwayland` 里就有这两个接口名，缺了它们
#   - 虚拟机的窗口 / 远程桌面客户端按不了 Super、Alt+Tab（被合成器的键位吃掉）；
#   - X11 的 XGrabKeyboard 传不到合成器，焦点一换按键就跑到别处去。
#
# 判据（headless 1280x720 一块屏；客户端是 build/keyinhibit 与 build/xkeygrab，
# 真按键用 wtype 走虚拟键盘造 —— 注入的键不触发键位，量不了抑制）：
#   A 协议就位：合成器日志报两个协议；客户端能 bind 到 manager（bind 不到会自己退出）
#   B 对照：同一个窗口、同样的按键，**没有抑制**时 Mod+F 的键位生效（窗口全屏）
#   C 抑制：焦点那个窗口要了抑制 → IPC input.shortcuts_inhibited = true；
#     Mod+F 不再生效（还是不全屏），而按键**到了客户端**（keycount 涨）
#   D 逃生口：配置里写了 { inhibited = true } 的那条绑定在抑制期间照样生效
#   E 撤销：客户端把 inhibitor 销毁 → IPC 回到 false，Mod+F 又生效
#   F X11（XWayland）：XGrabKeyboard → IPC keyboard_grab 报出那个窗口；
#     把焦点点回 Wayland 窗口之后，按键**还是**送给抓着键盘的 X 客户端
#     （X11 的语义：谁抓谁收）；撤销抓取 → 焦点回到焦点窗口，按键也回去了
#   G 全程没有 assert / abort / 段错误，收尾没有残留的抓取
#
# 用法：./test-keyboard-grab.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-kbgrab-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
PASSED=0
VIDOW_PGID=""
CLIENT_PIDS=""
LAST_CLIENT_PID=""   # start_client 起完就把 pid 放这儿（pgrep 会认错人）
KIPID=""
XPID=""

pass() { PASSED=$((PASSED + 1)); echo "  PASS  $1"; }
fail() { FAILED=1; echo "  FAIL  $1"; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for pid in $CLIENT_PIDS; do
		[ -n "$pid" ] || continue
		kill "$pid" 2>/dev/null
	done
	sleep 0.4
	for pid in $CLIENT_PIDS; do
		[ -n "$pid" ] || continue
		kill -KILL "$pid" 2>/dev/null
	done
	CLIENT_PIDS=""
}

cleanup() {
	stop_clients
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.5
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/vidowmsg ] || { echo "test-keyboard-grab.sh: 先构建"; exit 1; }
[ -x ./build/keyinhibit ] || { echo "test-keyboard-grab.sh: 先构建（缺 build/keyinhibit）"; exit 1; }
[ -x ./build/xkeygrab ] || { echo "test-keyboard-grab.sh: 先构建（缺 build/xkeygrab）"; exit 1; }
command -v wtype >/dev/null || { echo "test-keyboard-grab.sh: 需要 wtype（真按键）"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
scale = "1"
cursor_shake = false
vidow.bind("Mod+F", "toggle-fullscreen")
-- 逃生口：抑制期间这条照样生效（不然抑制着的窗口能把用户锁死）
vidow.bind("Mod+O", "toggle-floating", { inhibited = true })
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run" HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export WLR_HEADLESS_OUTPUT_SIZE=1280x720

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "vidow 没起来"; tail -5 "$LOG"; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

msg() { ./build/vidowmsg "$@" 2>&1; }

# JSON 里取一个表达式（stdin 是 JSON）
jq_py() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)" 2>/dev/null; }

# 窗口几何：<app_id 或 title> -> "x y w h id"
win_box() {
	msg windows | python3 -c '
import json,sys
d=json.load(sys.stdin)
key="'"$1"'"
w=[x for x in d if x.get("app_id")==key or x.get("title")==key]
if not w:
    sys.exit(1)
g=w[0]["geometry"]
print(g["x"],g["y"],g["width"],g["height"],w[0]["id"])' 2>/dev/null
}

# 焦点窗口的 id（没焦点 = 空）
focused_id() {
	msg windows | python3 -c '
import json,sys
d=json.load(sys.stdin)
w=[x for x in d if x.get("focused")]
print(w[0]["id"] if w else "")' 2>/dev/null
}

fullscreen_of() { # <窗口 id>
	msg windows | python3 -c '
import json,sys
d=json.load(sys.stdin)
w=[x for x in d if str(x["id"])=="'"$1"'"]
print(w[0].get("fullscreen") if w else "?")' 2>/dev/null
}

floating_of() { # <窗口 id>
	msg windows | python3 -c '
import json,sys
d=json.load(sys.stdin)
w=[x for x in d if str(x["id"])=="'"$1"'"]
print(w[0].get("floating") if w else "?")' 2>/dev/null
}

inhibited() { msg input | jq_py 'd["shortcuts_inhibited"]'; }
grab_window() { msg input | jq_py 'd.get("keyboard_grab") and d["keyboard_grab"]["window"] or "null"'; }
grab_type() { msg input | jq_py 'd.get("keyboard_grab") and d["keyboard_grab"]["type"] or "null"'; }

# 客户端收到过几个按键事件（keyinhibit / xkeygrab 都打 keycount）
keycount() {
	[ -f "$1" ] || { echo 0; return; }
	local n
	n=$(grep -a "^keycount " "$1" | tail -1 | awk '{print $2}')
	echo "${n:-0}"
}

start_client() { # <日志文件> <命令…>
	local log="$1"; shift
	rm -f "$log"
	"$@" > "$log" 2>&1 &
	LAST_CLIENT_PID=$!
	CLIENT_PIDS="$CLIENT_PIDS $LAST_CLIENT_PID"
}

wait_for() { # <文件> <正则> <秒>
	local i
	for i in $(seq 1 $(( ${3:-10} * 10 ))); do
		grep -aq "$2" "$1" && return 0
		sleep 0.1
	done
	return 1
}

# 某个窗口的几何：<窗口 id> -> "x y w h"
win_geom() {
	msg windows | python3 -c '
import json,sys
d=json.load(sys.stdin)
w=[x for x in d if str(x["id"])=="'"$1"'"]
if not w:
    sys.exit(1)
g=w[0]["geometry"]
print(g["x"],g["y"],g["width"],g["height"])' 2>/dev/null
}

# 这一点底下是哪个窗口（命中测试；没有窗口 = 空）
pick_id() { msg pick-window "$1" "$2" | jq_py 'd.get("window") and d["window"]["id"] or ""'; }

# 把光标点进某个窗口里（焦点跟着走）。
# 注意：窗口盒子（geometry）不等于客户端表面的大小 —— 表面是客户端自己 commit
# 的尺寸（keyinhibit 是 400x300），贴在盒子左上角，盒子剩下的地方命中测试是
# 「没有窗口」。所以先用 pick-window 试几个点，命中了才点。
click_window() { # <窗口 id>
	local id="$1" off px py gx gy gw gh
	read -r gx gy gw gh < <(win_geom "$id")
	for off in 40 100 160 220; do
		px=$((gx + off)); py=$((gy + off))
		if [ "$(pick_id "$px" "$py")" = "$id" ]; then
			msg move-cursor "$px" "$py" > /dev/null
			msg pointer-button click > /dev/null
			sleep 0.5
			return 0
		fi
	done
	echo "   （点不进去：#$id 在 $gx,$gy ${gw}x${gh}，试过的点都没命中它）"
	return 1
}

mark "A 协议就位"
if grep -aq "键盘协议: zwp_keyboard_shortcuts_inhibit_manager_v1 + zwp_xwayland_keyboard_grab_manager_v1 已就位" "$LOG"; then
	pass "合成器日志：两个协议都建起来了"
else
	fail "合成器日志里没有协议就位的行"
fi
grep -aq "keybind: Mod+O -> toggle-floating（抑制期间也生效）" "$LOG" &&
	pass "配置里的 { inhibited = true } 认出来了（日志里有）" ||
	fail "配置里的 { inhibited = true } 没生效"

# ---------------------------------------------------------------- B 对照
mark "B 对照：没有抑制时键位照常生效"
KILOG="$RUNDIR/noinhibit.log"
start_client "$KILOG" ./build/keyinhibit noinhibit kbtest
sleep 1.2
read -r WX WY WW WH WID < <(win_box kbtest)
if [ -z "${WID:-}" ]; then
	fail "没找到 keyinhibit 的窗口"
else
	pass "客户端窗口起来了（#$WID 在 $WX,$WY ${WW}x${WH}）"
fi
[ "$(inhibited)" = "False" ] &&
	pass "IPC：没有抑制时 shortcuts_inhibited = false" ||
	fail "IPC：没抑制也报了 true（$(inhibited)）"
wtype -M logo -k f
sleep 0.6
[ "$(fullscreen_of "${WID:-x}")" = "True" ] &&
	pass "Mod+F 生效了（窗口全屏）—— 对照组成立" ||
	fail "Mod+F 没生效（fullscreen=$(fullscreen_of "${WID:-x}")）"
wtype -M logo -k f
sleep 0.6
[ "$(fullscreen_of "${WID:-x}")" = "False" ] &&
	pass "再按一次退回来（全屏开关是双向的）" ||
	fail "第二次 Mod+F 没退回来（fullscreen=$(fullscreen_of "${WID:-x}")）"
stop_clients
sleep 0.6

# ---------------------------------------------------------------- C 抑制
mark "C 抑制：客户端说「别吃我的快捷键」"
HLDLOG="$RUNDIR/hold.log"
start_client "$HLDLOG" ./build/keyinhibit hold kbtest
KIPID=$LAST_CLIENT_PID
sleep 1.2
read -r HX HY HW HH HID < <(win_box kbtest)
if wait_for "$HLDLOG" "^active 1" 5; then
	pass "客户端收到 active（合成器认了抑制）"
else
	fail "客户端没收到 active（合成器没认？）"
fi
[ "$(inhibited)" = "True" ] &&
	pass "IPC：shortcuts_inhibited = true（焦点在它身上）" ||
	fail "IPC：shortcuts_inhibited 不是 true（$(inhibited)）"
KC_BEFORE=$(keycount "$HLDLOG")
wtype -M logo -k f
sleep 0.6
[ "$(fullscreen_of "${HID:-x}")" = "False" ] &&
	pass "抑制期间 Mod+F **不生效**（窗口没全屏）" ||
	fail "抑制期间 Mod+F 还是生效了（fullscreen=$(fullscreen_of "${HID:-x}")）"
KC_AFTER=$(keycount "$HLDLOG")
[ "$KC_AFTER" -gt "$KC_BEFORE" ] &&
	pass "但按键到了客户端（keycount $KC_BEFORE -> $KC_AFTER）" ||
	fail "按键没到客户端（keycount 还是 $KC_AFTER）"

mark "D 逃生口：{ inhibited = true } 的绑定照常生效"
FLOAT_BEFORE=$(floating_of "${HID:-x}")
wtype -M logo -k o
sleep 0.6
FLOAT_AFTER=$(floating_of "${HID:-x}")
if [ "$FLOAT_BEFORE" != "$FLOAT_AFTER" ]; then
	pass "抑制期间 Mod+O 照样生效（floating $FLOAT_BEFORE -> $FLOAT_AFTER）"
else
	fail "逃生口没生效（floating 还是 $FLOAT_AFTER）"
fi

mark "E 撤销抑制：键位又生效"
if [ -n "$KIPID" ]; then
	kill -USR1 "$KIPID"
	if wait_for "$HLDLOG" "^dropped" 5; then
		pass "客户端把 inhibitor 销毁了（dropped）"
	else
		fail "客户端没销毁 inhibitor"
	fi
else
	fail "找不到 keyinhibit 进程"
fi
sleep 0.5
[ "$(inhibited)" = "False" ] &&
	pass "IPC：shortcuts_inhibited 回到 false" ||
	fail "IPC：销毁之后还是 true（$(inhibited)）"
# 光标点回窗口里（焦点确认在它身上，不然量的是别的东西）
click_window "${HID:-0}" &&
	pass "焦点点回 Wayland 窗口 #$HID" ||
	fail "点不回 Wayland 窗口 #${HID:-?}（focused=$(focused_id)）"
wtype -M logo -k f
sleep 0.6
[ "$(fullscreen_of "${HID:-x}")" = "True" ] &&
	pass "抑制没了 Mod+F 又生效（窗口全屏）" ||
	fail "抑制没了 Mod+F 还是没生效（fullscreen=$(fullscreen_of "${HID:-x}")）"
wtype -M logo -k f
sleep 0.5
stop_clients
sleep 0.6

# ---------------------------------------------------------------- F X11
mark "F X11（XWayland 的 XGrabKeyboard）"
WLOG="$RUNDIR/wayland.log"
start_client "$WLOG" ./build/keyinhibit hold kbtest
sleep 1.2
read -r XWX XWY XWW XWH XWID < <(win_box kbtest)
export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)
if [ -z "${DISPLAY:-}" ]; then
	fail "vidow 没给出 DISPLAY（XWayland 没起来？）"
else
	pass "XWayland 在 $DISPLAY"
	XLOG="$RUNDIR/xkeygrab.log"
	start_client "$XLOG" ./build/xkeygrab xkg
	XPID=$LAST_CLIENT_PID
	if wait_for "$XLOG" "^grabkey 0" 20; then
		pass "XGrabKeyboard 成功（grabkey 0）"
	else
		fail "XGrabKeyboard 没成功（$(head -1 "$XLOG" 2>/dev/null)）"
	fi
	sleep 1.0
	read -r XX XY XW XH XID < <(win_box xkg)
	# 几何要**现在**重新量：X11 窗口一出来列宽会重排，之前量的坐标点下去可能
	# 落到隔壁窗口上（第一次跑就是这么点到 X11 窗口去的）。
	read -r XWX XWY XWW XWH XWID < <(win_box kbtest)
	echo "   （Wayland #${XWID:-?} 在 $XWX,$XWY ${XWW}x${XWH}；X11 #${XID:-?} 在 $XX,$XY ${XW}x${XH}）"
	if [ "$(grab_type)" = "xwayland" ] && [ "$(grab_window)" = "${XID:-x}" ]; then
		pass "IPC：X11 窗口 #$XID 上真的出现了一个键盘抓取（type=xwayland）"
	else
		fail "X11 那条路没出现抓取（type=$(grab_type) window=$(grab_window) 期望 #${XID:-?}）"
	fi
	# 焦点点回 Wayland 窗口：X11 的语义是「谁抓谁收」，按键该照样给 X 客户端
	click_window "${XWID:-0}"
	[ "$(focused_id)" = "${XWID:-x}" ] &&
		pass "焦点已经点回 Wayland 窗口 #$XWID" ||
		fail "焦点没点到 Wayland 窗口上（focused=$(focused_id) 期望 #${XWID:-?}）"
	WK_BEFORE=$(keycount "$WLOG")
	XK_BEFORE=$(keycount "$XLOG")
	msg send-key tap Return > /dev/null
	sleep 0.6
	XK_AFTER=$(keycount "$XLOG")
	WK_AFTER=$(keycount "$WLOG")
	[ "$XK_AFTER" -gt "$XK_BEFORE" ] &&
		pass "按键还是送给抓着键盘的 X 客户端（keycount $XK_BEFORE -> $XK_AFTER）" ||
		fail "抓着键盘的 X 客户端没收到按键（keycount 还是 $XK_AFTER）"
	[ "$WK_AFTER" = "$WK_BEFORE" ] &&
		pass "焦点那个 Wayland 窗口没收到（keycount 还是 $WK_AFTER）" ||
		fail "按键跑到焦点窗口去了（keycount $WK_BEFORE -> $WK_AFTER）"
	grep -aq "^key 36 pressed" "$XLOG" &&
		pass "X 客户端收到的是 Return（X 键码 36 = evdev 28 + 8）" ||
		fail "X 客户端收到的键码不对（$(grep -a '^key ' "$XLOG" | tail -1)）"
	if [ -n "$XPID" ]; then
		kill -USR1 "$XPID"
		if wait_for "$XLOG" "^ungrabbed" 5; then
			pass "XUngrabKeyboard 之后客户端报了 ungrabbed"
		else
			fail "撤销抓取之后客户端没报 ungrabbed"
		fi
	else
		fail "找不到 xkeygrab 进程"
	fi
	sleep 0.5
	[ "$(grab_window)" = "null" ] &&
		pass "IPC：抓取撤销了（keyboard_grab = null）" ||
		fail "IPC：抓取还在（$(grab_window)）"
	[ "$(focused_id)" = "${XWID:-x}" ] &&
		pass "焦点回到焦点窗口 #$XWID（抓取期间按过去的焦点还回去了）" ||
		fail "焦点没还给焦点窗口（focused=$(focused_id) 期望 #${XWID:-?}）"
	WK_BEFORE=$(keycount "$WLOG")
	msg send-key tap Return > /dev/null
	sleep 0.6
	WK_AFTER=$(keycount "$WLOG")
	[ "$WK_AFTER" -gt "$WK_BEFORE" ] &&
		pass "撤销之后按键回到焦点窗口（keycount $WK_BEFORE -> $WK_AFTER）" ||
		fail "撤销之后按键还没回到焦点窗口（keycount 还是 $WK_AFTER）"
fi
stop_clients
sleep 0.6

mark "G 收尾"
if grep -aiE "assert|abort|segmentation fault" "$LOG" | grep -av "assert.*不" | head -3 | grep -q .; then
	fail "日志里有 assert / abort / 段错误"
else
	pass "日志里没有 assert / abort / 段错误"
fi
msg version > /dev/null 2>&1 &&
	pass "合成器还活着（IPC 还通）" ||
	fail "合成器没反应了"
[ "$(grab_window)" = "null" ] &&
	pass "收尾：没有残留的键盘抓取" ||
	fail "收尾时还有抓取（$(grab_window)）"
[ "$(inhibited)" = "False" ] &&
	pass "收尾：没有残留的抑制" ||
	fail "收尾时还有抑制"

echo
echo "test-keyboard-grab.sh: $PASSED 项通过$( [ "$FAILED" = 1 ] && echo "，有失败" || echo "，全部通过" )"
[ "$FAILED" = 0 ]
