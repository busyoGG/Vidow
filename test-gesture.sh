#!/bin/bash
# 触控板手势（vidow.gesture{}）：三指横扫跟手拖视口、四指竖扫换工作区、捏合、按住。
#
# 这台机器上**没有触控板**，所以测试走注入那条路（`vidowmsg gesture …`）—— 注入直接
# 调 gesture.c 的同一批函数，和 libinput 送进来的手势走的是同一条代码。
#
# 判据：
#   1) 没有配置 = 一个手势都没有：注入手势什么都不发生（连日志里的 action 都没有）
#   2) 离散方向：四指上扫（够远）触发动作；方向没绑 / 扫得太短 都不触发
#   3) 捏合：scale 过了阈值才触发；按住：hold-begin 当场触发
#   4) 连续（三指横扫）：手指拖动时视口跟手（焦点不动），松手吸附到相邻那条列；
#      拖得太短弹回原位；cancelled 的手势不算数
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-gesture-test
CLIENT_PIDS="" 
CFGDIR="$RUNDIR/home"
CFGDIRV="$CFGDIR/.config/vidow"
LOG="$RUNDIR/log"
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

trap 'exit 143' TERM INT
trap cleanup EXIT

our_vidow_pids() {
	local p
	for p in /proc/[0-9]*; do
		[ -r "$p/cmdline" ] || continue
		grep -aq "build/vidow" "$p/cmdline" 2>/dev/null || continue
		[ -r "$p/environ" ] || continue
		grep -aqz "$RUNDIR" "$p/environ" 2>/dev/null || continue
		echo "${p#/proc/}"
	done
}

cleanup() {
	local pid
	our_vidow_pids | while read -r pid; do kill -KILL "$pid" 2>/dev/null; done
	for pid in ${CLIENT_PIDS:-}; do kill -KILL "$pid" 2>/dev/null; done
}

rm -rf "$RUNDIR"
mkdir -p "$CFGDIRV" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=info
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=pixman
export DISPLAY= WAYLAND_DISPLAY=

write_config() { # <额外配置行>
	cat > "$CFGDIRV/config.lua" <<CFG
scale = "1"
gaps = 10
window_open_ms = 0
window_move_ms = 0
$1
CFG
}

start_vidow() {
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	for _ in $(seq 1 60); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		fail "vidow 没起来"
		grep -a "ERROR" "$LOG" | head -5
		exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
}

stop_vidow() {
	local pid
	our_vidow_pids | while read -r pid; do kill -TERM "$pid" 2>/dev/null; done
	for _ in $(seq 1 40); do
		[ -z "$(our_vidow_pids)" ] && return 0
		sleep 0.2
	done
	return 1
}

VM="./build/vidowmsg"
st() { $VM state 2>/dev/null; }
q() { # q '<用 st 表示 state 的 python 表达式>'
	local out
	out=$(st)
	printf '%s' "$out" | python3 -c "
import json, sys
st = json.load(sys.stdin)
print($1)
" 2>/dev/null || echo "?"
}
ngestures() { q "len(st['gestures'])"; }
nwin() { q "st['counts']['windows']"; }
floats() { q "[w['floating'] for w in st['windows']]"; }
minimized() { q "[w['minimized'] for w in st['windows']]"; }
overview() { q "st['overview'].get('active')"; }
focused_title() { q "[w['title'] for w in st['windows'] if w.get('focused')]"; }
viewport() { q "round(st['outputs'][0]['viewport_x'],1)"; }

openwin() { # openwin <标题>
	python3 tools/pattern.py "$1" >/dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 1.6
}
gesture() { $VM gesture "$@" >/dev/null 2>&1; }

# ---------------------------------------------------- 1) 没有配置 = 没有手势
mark "1) 没有配置文件：一个手势都没有（注入了也不发生什么）"
write_config ""
start_vidow
[ "$(ngestures)" = "0" ] && pass "gestures 是空的（$(ngestures) 条）" \
	|| fail "居然有 $(ngestures) 条内置手势"
openwin g-none
gesture swipe-begin 4
gesture swipe-update 4 0 -400
gesture swipe-end
sleep 0.4
[ "$(floats)" = "[False]" ] && pass "四指上扫没动任何东西（没有内置手势）" \
	|| fail "没绑的手势居然生效了：$(floats)"
grep -aq "action:" "$LOG" && fail "没绑的手势却跑出了动作" \
	|| pass "没绑的手势一个动作都没跑"
stop_vidow

# ---------------------------------------------------- 2) 离散方向
mark "2) 离散方向：四指上扫 = 浮动切换（够远才算）"
write_config '
vidow.gesture{ fingers = 4, direction = "up", action = "toggle-floating" }
'
start_vidow
[ "$(ngestures)" = "1" ] && pass "手势表里有一条（$(ngestures)）" \
	|| fail "手势表条数不对：$(ngestures)"
grep -aq "gesture: 4 指竖扫 -> toggle-floating" "$LOG" \
	&& pass "日志里点名了这条手势" || fail "日志里没有这条手势"
openwin g-swipe
[ "$(floats)" = "[False]" ] && pass "起点：不浮动" || fail "起点就不对：$(floats)"
# 够远：累计 400（阈值 300）
gesture swipe-begin 4
gesture swipe-update 4 0 -200
gesture swipe-update 4 0 -200
gesture swipe-end
sleep 0.4
[ "$(floats)" = "[True]" ] && pass "四指上扫 400 触发（浮动=True）" \
	|| fail "够远的上扫没触发：$(floats)"
$VM set-floating false >/dev/null 2>&1
sleep 0.3
# 方向没绑：下扫
gesture swipe-begin 4
gesture swipe-update 4 0 400
gesture swipe-end
sleep 0.4
[ "$(floats)" = "[False]" ] && pass "四指下扫没绑 → 不触发" \
	|| fail "没绑的方向居然触发了：$(floats)"
# 太短：累计 100（不到阈值 300）
gesture swipe-begin 4
gesture swipe-update 4 0 -100
gesture swipe-end
sleep 0.4
[ "$(floats)" = "[False]" ] && pass "只扫 100（不到 300 阈值）→ 不触发" \
	|| fail "太短的手势也触发了：$(floats)"
# cancelled：扫得够远但被取消
gesture swipe-begin 4
gesture swipe-update 4 0 -400
gesture swipe-end --cancel
sleep 0.4
[ "$(floats)" = "[False]" ] && pass "cancelled 的手势不算数" \
	|| fail "取消掉的手势还是触发了：$(floats)"
stop_vidow

# ---------------------------------------------------- 3) 捏合 / 按住
mark "3) 捏合（scale 过阈值才触发）与按住"
write_config '
vidow.gesture{ fingers = 2, pinch = "out", action = "minimize-window" }
vidow.gesture{ fingers = 3, hold = true, action = "toggle-overview" }
'
start_vidow
openwin g-pinch
[ "$(minimized)" = "[False]" ] && pass "起点：没最小化" || fail "起点不对：$(minimized)"
gesture pinch-begin 2
gesture pinch-update 2 1.02
gesture pinch-end
sleep 0.4
[ "$(minimized)" = "[False]" ] && pass "捏合 1.02（不到 0.1 阈值）→ 不触发" \
	|| fail "太小的捏合也触发了：$(minimized)"
gesture pinch-begin 2
gesture pinch-update 2 1.4
gesture pinch-end
sleep 0.4
[ "$(minimized)" = "[True]" ] && pass "捏合张开到 1.4 → 触发（最小化）" \
	|| fail "捏合没触发：$(minimized)"
[ "$(overview)" = "False" ] && pass "按住之前：全景是关的" || fail "全景状态不对"
gesture hold-begin 3
sleep 0.4
[ "$(overview)" = "True" ] && pass "三指按住 → 当场触发（全景开了）" \
	|| fail "按住没触发：$(overview)"
gesture hold-end
$VM action "toggle-overview" >/dev/null 2>&1
sleep 0.5
[ "$(overview)" = "False" ] && pass "Esc 退出全景" || fail "全景没退出去"
stop_vidow

# ---------------------------------------------------- 4) 连续：跟手拖视口
mark "4) 三指横扫 = 跟手拖视口（松手吸附 / 太短弹回 / 取消不算）"
write_config '
vidow.gesture{ fingers = 3, swipe = "view" }
-- 两条列都占满一整屏：不然两列并排就装下了，视口没得滚（max_viewport = 0）
vidow.rule{ title = "^g-view", column_width = 1.0 }
'
start_vidow
openwin g-view-a
openwin g-view-b
[ "$(nwin)" = "2" ] && pass "两个窗口（两条列）" || fail "窗口数不对：$(nwin)"
BASE=$(viewport)
FOCUS0=$(focused_title)
pass "起点：视口 $BASE，焦点 $FOCUS0"
# 手指往**右**划 1200 单位（×0.3 = 360px，往左划会撞到条带末端被 clamp 卡住）：
# 视口跟手变小，焦点**不动**（还没松手）
gesture swipe-begin 3
gesture swipe-update 3 600 0
gesture swipe-update 3 600 0
MID=$(viewport)
if python3 -c "import sys; sys.exit(0 if abs(float('$MID') - float('$BASE')) > 100 else 1)"; then
	pass "拖动中：视口跟手（$BASE → $MID）"
else
	fail "拖动中视口没动（$BASE → $MID）"
fi
[ "$(focused_title)" = "$FOCUS0" ] && pass "拖动中焦点不动（松手才吸附）" \
	|| fail "拖动中焦点就变了：$(focused_title)"
gesture swipe-end
sleep 0.6
[ "$(focused_title)" != "$FOCUS0" ] \
	&& pass "松手吸附到相邻那条列（焦点 $FOCUS0 → $(focused_title)）" \
	|| fail "松手没吸附（焦点还是 $(focused_title)）"
SNAPPED=$(viewport)
# 吸附的目标 = 新焦点那条列的左边缘（这两条列都占满整屏，所以是 0）
python3 -c "import sys; sys.exit(0 if abs(float('$SNAPPED')) < 1 else 1)" \
	&& pass "吸附到位：视口停在 0（新焦点那条列占满整屏，左边缘就是 0；拖到的是 $MID）" \
	|| fail "吸附之后视口不对：$SNAPPED（拖到 $MID）"
# 太短：只划 100 单位（30px，不到 100px 阈值）→ 弹回原位
FOCUS1=$(focused_title)
BASE1=$(viewport)
gesture swipe-begin 3
gesture swipe-update 3 100 0
gesture swipe-end
sleep 0.6
[ "$(focused_title)" = "$FOCUS1" ] && pass "拖得太短：焦点不变" \
	|| fail "太短也换了列：$(focused_title)"
V1=$(viewport)
python3 -c "import sys; sys.exit(0 if abs(float('$V1') - float('$BASE1')) < 1 else 1)" \
	&& pass "拖得太短：视口弹回原位（$V1）" \
	|| fail "视口没弹回去：$V1（起点 $BASE1）"
# cancelled：拖得够远但取消 → 弹回原位
gesture swipe-begin 3
gesture swipe-update 3 1200 0
gesture swipe-end --cancel
sleep 0.6
[ "$(focused_title)" = "$FOCUS1" ] && pass "cancelled：焦点不变" \
	|| fail "取消掉的手势也换列了：$(focused_title)"
V2=$(viewport)
python3 -c "import sys; sys.exit(0 if abs(float('$V2') - float('$BASE1')) < 1 else 1)" \
	&& pass "cancelled：视口弹回原位（$V2）" \
	|| fail "取消之后视口不对：$V2（起点 $BASE1）"
stop_vidow

echo
if [ "$FAILED" = "0" ]; then
	echo "全部通过 / test-gesture.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
