#!/bin/bash
# 事件丰富度：除了窗口 / 工作区 / 输出 / layer / 剪贴板 / mru / 全景那几类（见
# test-hooks.sh 与 README 的事件表），这些也发事件：
#
#   keybind                  按下了绑好的键（trigger + action）
#   ipc-action               从 IPC 来的动作（action + source=ipc）
#   window-activated         应用喊「看我」（xdg-activation / _NET_ACTIVE_WINDOW /
#                            IPC 的 activate-window）
#   drag-started / drag-ended 拖窗口起 / 停（载荷是那个窗口）
#   output-power-changed     屏开 / 关（name + enabled）
#   xwayland-ready           X 服务真的起来了（display + scale）
#   input-device-added / input-device-removed  输入设备插拔（含虚拟键盘）
#   config-failed            配置没跑成（error）
#   screencast-started / screencast-stopped    录屏起止（要真录屏才能触发，见下）
#
# 判据：每条事件真的发出来、字段对得上；事件名单里列得全。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-events-test
CFGDIR="$RUNDIR/home"
CFGDIRV="$CFGDIR/.config/vidow"
EVLOG="$RUNDIR/events.log"
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
		[ -r "$p/cmdline" ] 2>/dev/null || continue
		grep -aq "build/vidow" "$p/cmdline" 2>/dev/null || continue
		[ -r "$p/environ" ] 2>/dev/null || continue
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

waitgone() {
	local i
	for i in $(seq 1 40); do
		[ -z "$(our_vidow_pids)" ] && return 0
		sleep 0.2
	done
	return 1
}

stop_clients() {
	local p pid cmd
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*xeyes*) kill "$pid" 2>/dev/null ;;
		esac
	done
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

command -v wtype >/dev/null || { echo "test-events.sh: 需要 wtype"; exit 1; }
command -v xeyes >/dev/null || { echo "test-events.sh: 需要 xeyes（x11-apps）"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-events.sh: 需要 python3-gobject"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIRV" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

# 每条事件一行：
#   事件名|trigger|action|name|enabled|output|app_id|error|display|type|kind
write_config() {
	# 注意 echo 后面那一段要**引号包住**：裸着写的话 | 会被 sh 当管道，
	# 整条命令就变成一串「找不到的命令」了。
	cat > "$CFGDIRV/config.lua" <<EOF
-- 指针手势（拖动 / 改列宽 / 滚轮换列）现在是键位：合成器不内置，测试也得自己绑。
vidow.bind("Mod+mouse-left", "interactive-move")
vidow.bind("Mod+mouse-right", "interactive-resize")
vidow.bind("Mod+wheel-up", "focus-column left")
vidow.bind("Mod+wheel-down", "focus-column right")
vidow.bind("Mod+T", "spawn touch $RUNDIR/keybind-fired")
vidow.on("*", "echo \"\$VIDOW_EVENT|\$VIDOW_TRIGGER|\$VIDOW_ACTION|\$VIDOW_NAME|\$VIDOW_ENABLED|\$VIDOW_OUTPUT|\$VIDOW_APP_ID|\$VIDOW_ERROR|\$VIDOW_DISPLAY|\$VIDOW_TYPE|\$VIDOW_KIND\" >> $EVLOG")
EOF
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


openwin() { python3 tools/pattern.py "$1" >/dev/null 2>&1 & sleep 1.6; }
# 等日志里出现某一行（最多 4 秒）
waitln() { local i; for i in $(seq 1 40); do grep -aq "$1" "$EVLOG" && return 0; sleep 0.1; done; return 1; }
state_expr() { ./build/vidowmsg state 2>/dev/null | python3 -c "
import json, sys
st = json.load(sys.stdin)
print($1)
" 2>/dev/null; }

# ------------------------------------------------------------ 1) 名单
mark "1) 事件名单列全（vidowmsg events）"
write_config
start_vidow
EVENTS=$(./build/vidowmsg events 2>/dev/null)
MISSING=$(printf '%s' "$EVENTS" | python3 -c "
import json, sys
d = json.load(sys.stdin)
names = set(d['events'])
want = ['keybind', 'ipc-action', 'window-activated', 'drag-started', 'drag-ended',
        'output-power-changed', 'xwayland-ready', 'input-device-added',
        'input-device-removed', 'screencast-started', 'screencast-stopped',
        'config-failed', 'window-opened', 'window-closed', 'workspace-activated',
        'column-maximize-changed', 'clipboard-changed', 'primary-changed',
        'clipboard-history-changed',
        'mru-committed', 'overview-toggled', 'config-reloaded', 'layer-mapped']
print(','.join(sorted(n for n in want if n not in names)) or '-')
print(d['count'])
" 2>/dev/null)
MISS=${MISSING%$'\n'*}; N=${MISSING##*$'\n'}
[ "$MISS" = "-" ] && pass "23 个抽样事件名一个不缺（一共 $N 个）" || fail "少了：$MISS"
[ "$N" -ge 40 ] && pass "事件总数 $N（>=40）" || fail "事件总数才 $N"

# ------------------------------------------------------------ 2) 键位 / 动作
mark "2) keybind 与 ipc-action"
wtype -M logo -k t >/dev/null 2>&1
[ -f "$RUNDIR/keybind-fired" ] && pass "Mod+T 的绑定真的跑了" || fail "Mod+T 没生效"
waitln "^keybind|" && pass "keybind 事件发出来了" || fail "keybind 事件没发"
grep -aq "^keybind|Mod+T|spawn touch $RUNDIR/keybind-fired|" "$EVLOG" &&
	pass "载荷对：trigger = Mod+T、action = 原样的动作串" \
	|| fail "载荷不对：$(grep -a '^keybind' "$EVLOG")"
./build/vidowmsg action cycle-column-width >/dev/null 2>&1
waitln "^ipc-action|" && pass "ipc-action 事件发出来了" || fail "ipc-action 没发"
grep -aq "^ipc-action||cycle-column-width|" "$EVLOG" &&
	pass "载荷对：action = cycle-column-width（trigger 空着，不是按出来的）" \
	|| fail "载荷不对：$(grep -a '^ipc-action' "$EVLOG")"

# ------------------------------------------------------------ 3) 窗口
mark "3) window-activated 与拖拽"
openwin ev-1
waitln "^window-opened|" && pass "开窗照旧发 window-opened" || fail "window-opened 丢了"
./build/vidowmsg activate-window --title ev-1 >/dev/null 2>&1
waitln "^window-activated|" && pass "activate-window 发了 window-activated" \
	|| fail "window-activated 没发"
grep -aq "^window-activated|.*|pattern.py|" "$EVLOG" &&
	pass "载荷是那个窗口（app_id = pattern.py）" \
	|| fail "载荷不对：$(grep -a '^window-activated' "$EVLOG")"
# 拖窗口：Mod+左键**按在窗口上** → 挪 → 松手（和 test-drag.sh 同一套注入）
GEO=$(state_expr "[(w['geometry']['x'] + w['geometry']['width'] // 2,
                   w['geometry']['y'] + w['geometry']['height'] // 2)
                  for w in st['windows'] if w.get('app_id') == 'pattern.py'][0]")
X0=$(printf '%s' "$GEO" | tr -d '(),' | awk '{print $1}')
Y0=$(printf '%s' "$GEO" | tr -d '(),' | awk '{print $2}')
[ -n "$X0" ] && pass "拿到窗口中心（$X0,$Y0）再按" || fail "读不到窗口几何"
./build/vidowmsg move-cursor "$X0" "$Y0" >/dev/null 2>&1
sleep 0.3
./build/vidowmsg action "pointer-button left press mod" >/dev/null 2>&1
sleep 0.3
./build/vidowmsg move-cursor "$((X0 + 150))" "$((Y0 + 20))" >/dev/null 2>&1
sleep 0.3
./build/vidowmsg action "pointer-button left release mod" >/dev/null 2>&1
waitln "^drag-started|" && pass "起拖发了 drag-started" || fail "drag-started 没发"
waitln "^drag-ended|" && pass "松手发了 drag-ended" || fail "drag-ended 没发"
grep -aq "^drag-started|.*|pattern.py|" "$EVLOG" &&
	pass "拖拽事件的载荷是那个窗口" || fail "拖拽载荷不对"

# ------------------------------------------------------------ 4) 输出
mark "4) output-power-changed"
./build/vidowmsg output-power HEADLESS-1 off >/dev/null 2>&1
sleep 0.5
./build/vidowmsg output-power HEADLESS-1 on >/dev/null 2>&1
sleep 0.5
waitln "^output-power-changed|||HEADLESS-1|false|" &&
	pass "关屏：name = HEADLESS-1、enabled = false" || fail "关屏事件不对：$(grep -a '^output-power' "$EVLOG")"
grep -aq "^output-power-changed|||HEADLESS-1|true|" "$EVLOG" &&
	pass "开屏：enabled = true" || fail "开屏事件不对"

# ------------------------------------------------------------ 5) X11
mark "5) xwayland-ready（懒启动：第一个 X 客户端连上来）"
XDISPLAY=$(state_expr "st['xwayland']['display']")
[ -n "$XDISPLAY" ] && pass "先知道 X 的 DISPLAY（$XDISPLAY）" || fail "state 里没拿到 DISPLAY"
DISPLAY="$XDISPLAY" xeyes >/dev/null 2>&1 &
sleep 3
waitln "^xwayland-ready|" && pass "X 服务起来时发了 xwayland-ready" \
	|| fail "xwayland-ready 没发（X 客户端起没起来？）"
grep -aq "^xwayland-ready|\(.*|\)\{7\}$XDISPLAY|" "$EVLOG" &&
	pass "载荷里带 display（$XDISPLAY）" \
	|| fail "载荷不对：$(grep -a '^xwayland-ready' "$EVLOG")"
waitln "^window-opened|.*|[xX]E[yY]es" && pass "X11 窗口也照常发 window-opened" \
	|| fail "X11 窗口没发 window-opened：$(grep -ai '^window-opened' "$EVLOG" | tail -2)"

# ------------------------------------------------------------ 6) 输入设备
mark "6) input-device-added / removed（wtype 用虚拟键盘）"
waitln "^input-device-added|.*|keyboard|" &&
	pass "虚拟键盘连上来时发了 input-device-added（type = keyboard）" \
	|| fail "input-device-added 没发：$(grep -a '^input-device' "$EVLOG" | head -2)"
grep -aq "^input-device-added|||wlr_virtual_keyboard_v1|.*|keyboard|" "$EVLOG" &&
	pass "载荷对：name = wlr_virtual_keyboard_v1、type = keyboard" \
	|| fail "载荷不对：$(grep -a '^input-device-added' "$EVLOG")"
waitln "^input-device-removed|" &&
	pass "它断了之后发了 input-device-removed" \
	|| fail "input-device-removed 没发"

# ------------------------------------------------------------ 7) 配置出错
mark "7) config-failed（钩子这时已经作废，只有订阅者收得到）"
( ./build/vidowmsg --raw watch config-failed > "$RUNDIR/watch.log" 2>&1 &
	echo $! > "$RUNDIR/watch.pid" )
sleep 0.8
printf 'error("配置故意写错")\n' > "$CFGDIRV/config.lua"
sleep 2.5
grep -aq '"event":"config-failed"' "$RUNDIR/watch.log" &&
	pass "订阅者收到 config-failed" || fail "config-failed 没发：$(tr '\n' ' ' < "$RUNDIR/watch.log" | head -c 200)"
grep -aq "故意写错" "$RUNDIR/watch.log" &&
	pass "载荷里带出错原因" || fail "载荷里没有原因"
grep -aq "钩子\|#0" "$RUNDIR/watch.log" && fail "载荷里混进了别的东西" || true
./build/vidowmsg state >/dev/null 2>&1 && pass "配置写错之后合成器还在（照旧用上一份）" \
	|| fail "配置写错把合成器带下去了"
kill "$(cat "$RUNDIR/watch.pid")" 2>/dev/null

# ------------------------------------------------------------ 8) 收尾
mark "8) 收尾"
stop_clients
./build/vidowmsg action quit >/dev/null 2>&1
waitgone && pass "退出干净" || { fail "进程还在"; our_vidow_pids; }
grep -aq "Assertion" "$LOG" && fail "退出时日志里有 assert" || pass "退出时没有 assert"

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-events.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
