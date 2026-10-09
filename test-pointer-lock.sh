#!/bin/bash
# 指针约束：zwp_pointer_constraints_v1 + zwp_relative_pointer_manager_v1。
#
# 这两个协议是「游戏锁鼠标」要的（用户报的「xwayland 缺了什么协议」就是它）：
# 缺了的话 Xwayland 的 confine_pointer / lock_pointer 请求发不出去（
# /usr/bin/Xwayland 里就这两个字符串），X11 游戏转视角只能靠绝对坐标凑。
#
# 判据（headless 1280x720 一块屏；客户端是 build/pointerlock 与 build/xgrab）：
#   A 协议就位：合成器日志报两个 global；客户端能 bind 到它们（bind 不到会自己报错退出）
#   B 锁定（Wayland）：locked 事件到达 → IPC input.pointer_constraint = locked/window；
#     相对位移**到客户端**（reltotal = 120,80）；光标被**钉住**（IPC 坐标不变、
#     客户端收到的绝对坐标也不变）—— 这三条一起才是「锁住了」
#   C 夹住（Wayland）：区域 100x100+50+50，把光标往区域外推 400,400 →
#     客户端的 motion 坐标仍在区域里，合成器光标也停在区域里（表面原点 6,6 →
#     布局 106,106）；相对位移照发（400,400）
#   D 收尾：客户端退出 → 约束解除、光标能动了
#   E X11（XWayland）：XGrabPointer 之后合成器里**真的**出现了一个约束
#     （window = xgrab 那个窗口，就是「Xwayland bind 上了协议」的直接证据）；
#     相对位移到 X 客户端（最后一条 motion = 钉住的坐标 + 位移）
#   F 全程没有 assert / abort / 段错误
#
# 用法：./test-pointer-lock.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-pointerlock-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
PASSED=0
VIDOW_PGID=""
CLIENT_PIDS=""

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

[ -x ./build/vidowmsg ] || { echo "test-pointer-lock.sh: 先构建"; exit 1; }
[ -x ./build/pointerlock ] || { echo "test-pointer-lock.sh: 先构建（缺 build/pointerlock）"; exit 1; }
[ -x ./build/xgrab ] || { echo "test-pointer-lock.sh: 先构建（缺 build/xgrab）"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
scale = "1"
cursor_shake = false
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

# IPC 里的约束状态（null / locked / confined / window 号）
constraint_type() { msg input | jq_py 'd.get("pointer_constraint") and d["pointer_constraint"]["type"] or "null"'; }
constraint_window() { msg input | jq_py 'd.get("pointer_constraint") and d["pointer_constraint"]["window"] or "null"'; }
cursor_pos() { msg input | jq_py '"%s %s" % (int(d["cursor"]["x"]), int(d["cursor"]["y"]))'; }

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

start_client() { # <日志文件> <命令…>
	local log="$1"; shift
	rm -f "$log"
	"$@" > "$log" 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
}

wait_for() { # <文件> <正则> <秒>
	local i
	for i in $(seq 1 $(( ${3:-10} * 10 ))); do
		grep -aq "$2" "$1" && return 0
		sleep 0.1
	done
	return 1
}

last_motion() { grep -a "^motion " "$1" | tail -1 | awk '{print $2, $3}'; }

mark "A 协议就位"
if grep -aq "指针约束: zwp_pointer_constraints_v1 + zwp_relative_pointer_manager_v1 已就位" "$LOG"; then
	pass "合成器日志：两个协议都建起来了"
else
	fail "合成器日志里没有协议就位的行"
fi

# ---------------------------------------------------------------- B 锁定
mark "B 锁定（Wayland 客户端锁鼠标）"
LOCKLOG="$RUNDIR/lock.log"
start_client "$LOCKLOG" ./build/pointerlock lock
sleep 1.2
read -r WX WY WW WH WID < <(win_box pointerlock)
if [ -z "${WID:-}" ]; then
	fail "没找到 pointerlock 的窗口"
else
	pass "客户端窗口起来了（#$WID 在 $WX,$WY ${WW}x${WH}）"
fi
msg move-cursor $((WX + 200)) $((WY + 150)) > /dev/null
if wait_for "$LOCKLOG" "^locked" 8; then
	pass "合成器发了 locked（客户端收到）"
else
	fail "客户端没收到 locked"
fi
[ "$(constraint_type)" = "locked" ] &&
	pass "IPC：pointer_constraint.type = locked" ||
	fail "IPC 里的约束不是 locked（$(constraint_type)）"
[ "$(constraint_window)" = "${WID:-x}" ] &&
	pass "IPC：约束挂在窗口 #$WID 上" ||
	fail "IPC 里的约束窗口不是 #${WID:-?}（$(constraint_window)）"
[ "$(msg input | jq_py 'd["pointer_constraint"]["lifetime"]')" = "persistent" ] &&
	pass "IPC：lifetime = persistent" ||
	fail "IPC 里的 lifetime 不对"

BEFORE="$(cursor_pos)"
# 客户端进窗口时收到的那一对坐标：锁住之后绝对坐标应该一直停在这儿
ENTER_MOTION="$(grep -a "^enter " "$LOCKLOG" | tail -1 | awk '{print $2, $3}')"
msg move-cursor 120 80 --relative > /dev/null
sleep 0.5
if grep -aq "^reltotal 120.0 80.0" "$LOCKLOG"; then
	pass "相对位移到客户端了（reltotal 120.0 80.0）"
else
	fail "客户端没收到相对位移（$(grep -a '^reltotal' "$LOCKLOG" | tail -1)）"
fi
[ "$(cursor_pos)" = "$BEFORE" ] &&
	pass "光标被钉住（还是 $BEFORE，位移没落到位置上）" ||
	fail "光标动了（$BEFORE -> $(cursor_pos)）"
AFTER_MOTION="$(last_motion "$LOCKLOG")"
AFTER_MOTION="${AFTER_MOTION:-$ENTER_MOTION}"
[ -n "$ENTER_MOTION" ] && [ "$AFTER_MOTION" = "$ENTER_MOTION" ] &&
	pass "客户端那边的指针位置没动（还是 $ENTER_MOTION）" ||
	fail "客户端那边的指针位置动了（$ENTER_MOTION -> $AFTER_MOTION）"
grep -aq "指针约束: 锁定 生效" "$LOG" &&
	pass "合成器日志：锁定 生效" ||
	fail "合成器日志里没有「锁定 生效」"

mark "C 夹住（Wayland 客户端把光标夹在区域里）"
stop_clients
sleep 0.6
CONFLOG="$RUNDIR/conf.log"
start_client "$CONFLOG" ./build/pointerlock confine 100x100+50+50
sleep 1.2
read -r CX CY CW CH CID < <(win_box pointerlock)
msg move-cursor $((CX + 200)) $((CY + 150)) > /dev/null
if wait_for "$CONFLOG" "^confined" 8; then
	pass "合成器发了 confined"
else
	fail "客户端没收到 confined"
fi
[ "$(constraint_type)" = "confined" ] &&
	pass "IPC：pointer_constraint.type = confined" ||
	fail "IPC 里的约束不是 confined（$(constraint_type)）"
# 先把光标摆进区域里（客户端区域是表面局部 100x100+50+50，表面原点 = 窗口原点）
CONF_CURSOR="$(cursor_pos)"
msg move-cursor 400 400 --relative > /dev/null
sleep 0.5
read -r MX MY <<< "$(last_motion "$CONFLOG")"
if [ -n "${MX:-}" ] && [ "$MX" -ge 50 ] && [ "$MX" -le 150 ] &&
		[ "$MY" -ge 50 ] && [ "$MY" -le 150 ]; then
	pass "往区域外推 400,400 之后客户端的坐标还在区域里（$MX,$MY）"
else
	fail "客户端的坐标跑到区域外了（${MX:-?},${MY:-?}）"
fi
read -r PX PY <<< "$(cursor_pos)"
if [ "$PX" -ge $((CX + 50)) ] && [ "$PX" -le $((CX + 150)) ] &&
		[ "$PY" -ge $((CY + 50)) ] && [ "$PY" -le $((CY + 150)) ]; then
	pass "合成器光标也在区域里（$PX,$PY，窗口原点 $CX,$CY）"
else
	fail "合成器光标在区域外（$PX,$PY，窗口原点 $CX,$CY）"
fi
grep -aq "^reltotal 400.0 400.0" "$CONFLOG" &&
	pass "夹住时相对位移照发（reltotal 400.0 400.0）" ||
	fail "夹住时相对位移没到（$(grep -a '^reltotal' "$CONFLOG" | tail -1)）"
stop_clients
sleep 0.6
[ "$(constraint_type)" = "null" ] &&
	pass "客户端退出后约束解除（IPC 里是 null）" ||
	fail "客户端退出后约束还在（$(constraint_type)）"
msg move-cursor 900 400 > /dev/null
[ "$(cursor_pos)" = "900 400" ] &&
	pass "解除之后光标能动了（900,400）" ||
	fail "解除之后光标还是动不了（$(cursor_pos)）"

mark "D 锁之前先给 cursor_hint"
HINTLOG="$RUNDIR/hint.log"
start_client "$HINTLOG" ./build/pointerlock lock --hint=40,60
sleep 1.2
read -r HX HY HW HH HID < <(win_box pointerlock)
msg move-cursor $((HX + 200)) $((HY + 150)) > /dev/null
if wait_for "$HINTLOG" "^locked" 8; then
	pass "带 hint 的锁定也生效了"
else
	fail "带 hint 的锁定没生效"
fi
HINT_CURSOR="$(cursor_pos)"
if [ "$HINT_CURSOR" = "$((HX + 40)) $((HY + 60))" ]; then
	pass "光标落在 hint 上（$HINT_CURSOR）"
else
	fail "光标没落在 hint 上（$HINT_CURSOR，期望 $((HX + 40)) $((HY + 60))）"
fi
stop_clients
sleep 0.6

# ---------------------------------------------------------------- E X11
mark "E X11（XWayland 的 XGrabPointer）"
XLOG="$RUNDIR/xgrab.log"
export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)
if [ -z "${DISPLAY:-}" ]; then
	fail "vidow 没给出 DISPLAY（XWayland 没起来？）"
else
	pass "XWayland 在 $DISPLAY"
	start_client "$XLOG" ./build/xgrab xgrab
	if wait_for "$XLOG" "^grabbed 0" 20; then
		pass "XGrabPointer 成功（grabbed 0）"
	else
		fail "XGrabPointer 没成功（$(head -1 "$XLOG" 2>/dev/null)）"
	fi
	sleep 1.0
	read -r XX XY XW XH XID < <(win_box xgrab)
	msg move-cursor $((XX + XW / 2)) $((XY + XH / 2)) > /dev/null
	sleep 0.6
	if [ "$(constraint_type)" = "locked" ] && [ "$(constraint_window)" = "${XID:-x}" ]; then
		pass "IPC：X11 窗口 #$XID 上真的出现了一个约束（locked）"
	else
		fail "X11 那条路没出现约束（type=$(constraint_type) window=$(constraint_window) 期望 #${XID:-?}）"
	fi
	XBEFORE="$(cursor_pos)"
	XMOTION="$(last_motion "$XLOG")"
	msg move-cursor 300 200 --relative > /dev/null
	sleep 0.6
	[ "$(cursor_pos)" = "$XBEFORE" ] &&
		pass "X11：光标被钉住（$XBEFORE）" ||
		fail "X11：光标动了（$XBEFORE -> $(cursor_pos)）"
	read -r NX NY <<< "$(last_motion "$XLOG")"
	read -r OX OY <<< "$XMOTION"
	if [ -n "${NX:-}" ] && [ -n "${OX:-}" ] &&
			[ "$NX" = "$((OX + 300))" ] && [ "$NY" = "$((OY + 200))" ]; then
		pass "X11：钉住的同时客户端照样收到位移（$OX,$OY -> $NX,$NY，相对指针那条路）"
	else
		fail "X11：客户端的位移对不上（$OX,$OY -> ${NX:-?},${NY:-?}，期望 +300,+200）"
	fi
fi
stop_clients
sleep 0.6

mark "F 收尾"
if grep -aiE "assert|abort|segmentation fault" "$LOG" | grep -av "assert.*不" | head -3 | grep -q .; then
	fail "日志里有 assert / abort / 段错误"
else
	pass "日志里没有 assert / abort / 段错误"
fi
msg version > /dev/null 2>&1 &&
	pass "合成器还活着（IPC 还通）" ||
	fail "合成器没反应了"
[ "$(constraint_type)" = "null" ] &&
	pass "收尾：没有残留的约束" ||
	fail "收尾时还有约束（$(constraint_type)）"

echo
echo "test-pointer-lock.sh: $PASSED 项通过$( [ "$FAILED" = 1 ] && echo "，有失败" || echo "，全部通过" )"
[ "$FAILED" = 0 ]
