#!/bin/bash
# 键位全是配置给的（合成器一个都不内置）+ 默认那套真的能用。
#
# 判据：
#   1) 没有配置文件 = **一个键位都没有**：按 Mod+Q / Mod+V 什么都不发生（键还给客户端）
#   2) --write-config 写出来那套（configs/14-keys.lua）真的生效：Mod+Q 关窗、Mod+V 浮动、
#      Mod+R 列宽、Mod+Shift+L 挪列、Mod+O 全景、Mod+M 最小化 / Mod+Shift+M 放回
#   3) 键位表里的键名解析：Ctrl+Alt+F1..F12（切 tty 那组）也绑得上
#   4) 动作字符串：vidowmsg action 逐条跑（窗口 / 列 / 浮动 / 全屏 / 全景 / spawn /
#      quit / switch-vt），参数写错和没听过的动作都当场报错
#   5) 改键位就是改配置：把 Mod+Q 换成 Mod+W，重载后新的生效、旧的还给客户端
#   6) spawn 真的起了命令
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-binds-test
CFGDIR="$RUNDIR/home"
CFGDIRV="$CFGDIR/.config/vidow"
LOG="$RUNDIR/log"
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

trap 'exit 143' TERM INT
trap cleanup EXIT

# 本测试起的 vidow：cmdline 里有 build/vidow、且 environ 里带着本测试的 RUNDIR。
# 只认这两条，绝不碰用户自己的会话（它的 environ 里没有这个 RUNDIR）。
# 为什么不按 pgid 杀：`setsid ./build/vidow &` 里的 $! 不一定就是 vidow 自己
# （setsid 可能先 fork 一下），按 pgid 杀会漏。
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

kill_stray_vidows() {
	local pid
	our_vidow_pids | while read -r pid; do
		kill -KILL "$pid" 2>/dev/null
	done
}

# 等本测试的 vidow 全退干净（最多 8 秒）
waitgone() {
	local i
	for i in $(seq 1 40); do
		[ -z "$(our_vidow_pids)" ] && return 0
		sleep 0.2
	done
	return 1
}

cleanup() {
	stop_clients
	local pid
	our_vidow_pids | while read -r pid; do
		kill -TERM "$pid" 2>/dev/null
	done
	sleep 0.4
	kill_stray_vidows
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}

stop_clients() {
	local p pid cmd
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*bluewin.py*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

command -v wtype >/dev/null || { echo "test-binds.sh: 需要 wtype"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-binds.sh: 需要 python3-gobject"; exit 1; }

mkdir -p "$CFGDIR/.config" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

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
	our_vidow_pids | while read -r pid; do
		kill -TERM "$pid" 2>/dev/null
	done
	waitgone || kill_stray_vidows
}

# 状态查询的小工具：解析不出来就重试（顺手把原文留下来，真出问题好查）
st_expr() { # st_expr '<用 st 表示 state 的 python 表达式>'
	local i out
	for i in 1 2 3; do
		out=$(./build/vidowmsg state 2>/dev/null)
		if printf '%s' "$out" | python3 -c "
import json, sys
st = json.load(sys.stdin)
print($1)
" 2>/dev/null; then
			return 0
		fi
		sleep 0.2
	done
	printf '%s' "$out" > "$RUNDIR/state.bad.json" 2>/dev/null
	return 1
}
nbinds() { st_expr "len(st['keybinds'])"; }
nwin() { st_expr "st['counts']['windows']"; }
floats() { st_expr "[w['floating'] for w in st['windows']]"; }
widths() { st_expr "[round(w['column']['width_fraction'],2) for w in st['windows'] if w.get('column')]"; }
overview() { st_expr "st['overview'].get('active')"; }
minimized() { st_expr "[w['minimized'] for w in st['windows']]"; }
openwin() { # openwin <标题>
	python3 tools/pattern.py "$1" >/dev/null 2>&1 &
	sleep 1.6
}
waitnwin() { # waitnwin <期望窗口数> —— 最多等 3 秒
	local i
	for i in $(seq 1 30); do
		[ "$(nwin)" = "$1" ] && return 0
		sleep 0.1
	done
	return 1
}

# ------------------------------------------------------------ 1) 没有配置 = 没有键位
mark "1) 没有配置文件：一个键位都没有（键还给客户端）"
start_vidow
[ "$(nbinds)" = "0" ] && pass "keybinds 是空的（$(nbinds) 条）" \
	|| fail "居然有 $(nbinds) 条内置键位"
openwin kb-none
wtype -M logo -k q >/dev/null 2>&1
sleep 0.8
[ "$(nwin)" = "1" ] && pass "按 Mod+Q 窗口还在（没有内置的关窗键）" \
	|| fail "Mod+Q 把窗口关了 —— 还有内置键位？"
wtype -M logo -k v >/dev/null 2>&1
sleep 0.8
[ "$(floats)" = "[False]" ] && pass "按 Mod+V 也没浮动（没有内置的浮动键）" \
	|| fail "Mod+V 居然生效了：$(floats)"
wtype -M logo -M shift -k l >/dev/null 2>&1
sleep 0.6
grep -aq "action:" "$LOG" && fail "没绑的键却跑出了动作（日志里有 action:）" \
	|| pass "没绑的键一个动作都没跑"

# ------------------------------------------------------------ 2) 默认配置那套
mark "2) --write-config 写出来的默认键位：真的能用"
stop_clients
stop_vidow
rm -f "$CFGDIRV/config.lua"
HOME="$CFGDIR" ./build/vidow --write-config >/dev/null 2>&1
[ -s "$CFGDIRV/configs/14-keys.lua" ] && pass "默认配置里有一份键位（14-keys.lua）" \
	|| fail "没写出 14-keys.lua"
start_vidow
[ "$(nbinds)" -ge 40 ] && pass "键位表有了（$(nbinds) 条）" \
	|| fail "键位只有 $(nbinds) 条"
grep -aq 'Ctrl+Alt+F12 -> switch-vt 12' "$LOG" &&
	pass "Ctrl+Alt+F1..F12 也绑上了（键名 F1..F12 认得）" \
	|| fail "Ctrl+Alt+F12 没绑上"
openwin kb-1
wtype -M logo -k r >/dev/null 2>&1
sleep 0.5
[ "$(widths)" = "[0.67]" ] && pass "Mod+R 把列宽切到 2/3（$(widths)）" \
	|| fail "Mod+R 没切列宽：$(widths)"
wtype -M logo -k v >/dev/null 2>&1
sleep 0.6
[ "$(floats)" = "[True]" ] && pass "Mod+V 浮动" || fail "Mod+V 没浮动：$(floats)"
wtype -M logo -k v >/dev/null 2>&1
sleep 0.6
[ "$(floats)" = "[False]" ] && pass "再按一次回平铺" || fail "回不去：$(floats)"
openwin kb-2
[ "$(nwin)" = "2" ] || fail "第二个窗口没开起来（$(nwin)）"
wtype -M logo -M shift -k l >/dev/null 2>&1
sleep 0.6
grep -aq "action: move-column right ->" "$LOG" && pass "Mod+Shift+L 走到 move-column right" \
	|| fail "Mod+Shift+L 没生效"
wtype -M logo -k o >/dev/null 2>&1
sleep 0.6
[ "$(overview)" = "True" ] && pass "Mod+O 进全景" || fail "Mod+O 没进全景"
wtype -M logo -k o >/dev/null 2>&1
sleep 0.6
[ "$(overview)" = "False" ] && pass "再按一次退出全景" || fail "全景没退出"
wtype -M logo -k m >/dev/null 2>&1
sleep 0.6
case "$(minimized)" in
*True*) pass "Mod+M 最小化当前窗口（$(minimized)）" ;;
*) fail "Mod+M 没最小化：$(minimized)" ;;
esac
wtype -M logo -M shift -k m >/dev/null 2>&1
sleep 0.8
case "$(minimized)" in
*True*) fail "Mod+Shift+M 没放回来：$(minimized)" ;;
*) pass "Mod+Shift+M 放回最近收起来的那个" ;;
esac
wtype -M logo -k q >/dev/null 2>&1
waitnwin 1 && pass "Mod+Q 关掉一个窗口（剩 $(nwin) 个）" \
	|| fail "Mod+Q 没关窗（剩 $(nwin) 个）"

# ------------------------------------------------------------ 3) 动作字符串
mark "3) 动作字符串（vidowmsg action，逐条）"
A() { ./build/vidowmsg action "$1" >/dev/null 2>&1; }
A "cycle-column-width" && pass "cycle-column-width" || fail "cycle-column-width 失败"
A "focus-column left" && pass "focus-column left" || fail "focus-column left 失败"
A "focus-column 1" && pass "focus-column 1（数字方向也认）" || fail "focus-column 1 失败"
A "move-column right" && pass "move-column right" || fail "move-column right 失败"
A "focus-window down" && pass "focus-window down" || fail "focus-window down 失败"
A "move-window up" && pass "move-window up" || fail "move-window up 失败"
A "toggle-fullscreen" && pass "toggle-fullscreen" || fail "toggle-fullscreen 失败"
sleep 0.5
A "toggle-fullscreen" || fail "全屏关不回去"
A "spawn touch $RUNDIR/spawned" && pass "spawn 起命令" || fail "spawn 失败"
sleep 0.6
[ -f "$RUNDIR/spawned" ] && pass "spawn 真的跑了（文件出来了）" || fail "spawn 没跑"
A "focus-column bogus" && fail "方向写错居然成功" || pass "方向写错当场失败"
A "not-an-action" && fail "没听过的动作居然成功" || pass "没听过的动作当场失败"
grep -aq "不认识的动作 'not-an-action'" "$LOG" &&
	pass "日志里点名了不认识的动作" || fail "日志里没点名"
grep -aq "focus-column 要一个方向" "$LOG" &&
	pass "日志里说了方向怎么写" || fail "日志里没说方向怎么写"

# ------------------------------------------------------------ 4) 改键位
mark "4) 换键位就是改配置：Mod+Q → Mod+W"
sed -i 's|vidow.bind("Mod+Q", "close-window")|vidow.bind("Mod+W", "close-window")|' \
	"$CFGDIRV/configs/14-keys.lua"
sleep 1
grep -aq "Mod+W -> close-window" "$LOG" && pass "改完自动重载，新键绑上了" \
	|| fail "改了 14-keys.lua 没重载"
BEFORE=$(nwin)
wtype -M logo -k q >/dev/null 2>&1
sleep 0.8
[ "$(nwin)" = "$BEFORE" ] && pass "旧键 Mod+Q 还给客户端了（窗口没关）" \
	|| fail "旧键还在生效"
wtype -M logo -k w >/dev/null 2>&1
waitnwin $((BEFORE - 1)) && pass "新键 Mod+W 关窗" \
	|| fail "新键没生效（剩 $(nwin) 个）"

# ------------------------------------------------------------ 5) 收尾
mark "5) quit 动作：正常退出"
stop_clients
A "quit" && pass "quit 动作报成功" || fail "quit 报失败"
waitgone && pass "进程退干净了（本测试的 vidow 一个不剩）" || {
	fail "quit 之后进程还在"
	our_vidow_pids
}
grep -aq "Assertion" "$LOG" && fail "退出时日志里有 assert" || pass "退出时没有 assert"

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-binds.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
