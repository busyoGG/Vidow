#!/bin/bash
# 事件钩子（config.lua 的 vidow.on，见 src/hooks.c）：命令钩子 / Lua 钩子 /
# 全部事件 / 自定义事件 / 摘钩子 / 出错不连累别人。
#
# 判据：
#   1) vidowmsg hooks：谁在听哪个事件、跑过几次；events：事件名单
#   2) 命令钩子：事件字段变成环境变量（$VIDOW_EVENT / $VIDOW_APP_ID / $VIDOW_TITLE）
#   3) "*" 钩子收到所有事件
#   4) Lua 钩子：载荷是张表（e.event / e.data.app_id），钩子里能 await vidow.exec()
#   5) 钩子出错只打日志，合成器不倒、后面的钩子照跑
#   6) vidow.off() 摘掉自己 → 只跑一次
#   7) IPC 的 emit：自己发事件，订阅者和钩子都收到；data 展开成 VIDOW_<字段>
#   8) 配置重载之后钩子不重复累积（旧的一批作废，新的重新声明）
#   9) **没有 IPC 订阅者**也一样跑（钩子不是「订阅者的副作用」）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-hooks-test
CFGDIR="$RUNDIR/home"
CFGDIRV="$CFGDIR/.config/vidow"
HKDIR="$RUNDIR/hooks"
LOG="$RUNDIR/log"
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

trap 'exit 143' TERM INT
trap cleanup EXIT

# 本测试起的 vidow：cmdline 里有 build/vidow、且 environ 里带着本测试的 RUNDIR
# （绝不碰用户自己的会话）。
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
		*pattern.py*) kill "$pid" 2>/dev/null ;;
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

python3 -c "import gi" 2>/dev/null || { echo "test-hooks.sh: 需要 python3-gobject"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIRV" "$RUNDIR/run" "$HKDIR"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

write_config() {
	cat > "$CFGDIRV/config.lua" <<EOF
-- 命令钩子：事件字段变成环境变量
vidow.on("window-opened", "echo open:\$VIDOW_EVENT:\$VIDOW_APP_ID:\$VIDOW_TITLE >> $HKDIR/open.log")
-- 全部事件
vidow.on("*", "echo any:\$VIDOW_EVENT >> $HKDIR/any.log")
-- Lua 钩子：载荷是张表，钩子里能 await vidow.exec()
vidow.on("window-opened", function(e)
	vidow.exec("echo lua:" .. tostring(e.data.app_id) .. ":" .. e.event .. " >> $HKDIR/lua.log", { wait = false })
	local out = vidow.exec("echo from-exec")
	vidow.exec("echo exec:" .. tostring(out) .. " >> $HKDIR/lua.log", { wait = false })
end)
-- 出错的钩子：只该打日志，后面的钩子照跑
vidow.on("window-opened", function(e) error("故意炸一个") end)
-- 跑一次就摘掉自己
local once
once = vidow.on("window-opened", function(e)
	vidow.off(once)
	vidow.exec("echo once >> $HKDIR/once.log", { wait = false })
end)
-- 自定义事件：data 里的字段展开成 \$VIDOW_<字段名大写>
vidow.on("test-ping", "echo ping:\$VIDOW_N >> $HKDIR/ping.log")
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

stop_vidow() {
	local pid
	our_vidow_pids | while read -r pid; do
		kill -TERM "$pid" 2>/dev/null
	done
	waitgone || kill_stray_vidows
}

openwin() { # openwin <标题>
	python3 tools/pattern.py "$1" >/dev/null 2>&1 &
	sleep 1.6
}
# 钩子最晚在下一个 idle 里跑，命令本身又是 detached 的：等文件出现（最多 4 秒）
waitfor() { # waitfor <文件> [想看到的内容]
	local i
	for i in $(seq 1 40); do
		if [ -s "$1" ] && { [ $# -lt 2 ] || grep -aq "$2" "$1"; }; then
			return 0
		fi
		sleep 0.1
	done
	return 1
}
# 一次性把应答抓下来，用 python 解析（比在 shell 里塞表达式清楚，失败时
# 原文就在 $RUNDIR/hooks.json 里）。
snap_hooks() {
	./build/vidowmsg --raw hooks > "$RUNDIR/hooks.json" 2>&1
}
hexpr() { # hexpr '<用 h 表示 result 的 python 表达式>'
	python3 -c "
import json, sys
h = json.load(open('$RUNDIR/hooks.json'))['result']
print($1)
" 2>/dev/null
}
nhooks() { snap_hooks; hexpr "h['count']"; }

# ------------------------------------------------------------ 1) 挂上了
mark "1) 配置里的钩子挂上了：vidowmsg hooks 报得出来"
write_config
start_vidow
# 配置里挂了 6 条：window-opened 一条命令 + 三条 Lua（含那条故意出错的）、
# "*" 一条、test-ping 一条
[ "$(nhooks)" = "6" ] && pass "6 条钩子都挂上了" \
	|| fail "钩子数不对：$(nhooks)（期望 6）"
snap_hooks
EV=$(hexpr "'/'.join(sorted({x['event'] for x in h['hooks']}))")
[ "$EV" = "*/test-ping/window-opened" ] && pass "事件名对：$EV" ||
	fail "事件名不对：'$EV'"
hexpr "sorted({x['kind'] for x in h['hooks']})" | grep -q "cmd" &&
	pass "命令钩子和 Lua 钩子都在（kind = cmd / lua）" || fail "kind 不对"
hexpr "[x['cmd'] for x in h['hooks'] if x['event']=='window-opened' and x['kind']=='cmd'][0]" |
	grep -q "VIDOW_APP_ID" && pass "命令钩子的原文报得出来（含 \$VIDOW_APP_ID）" \
	|| fail "命令原文没报出来"
hexpr "'window-opened' in h['event_names'] and 'config-failed' in h['event_names']" |
	grep -q True && pass "事件名单里有 window-opened / config-failed" \
	|| fail "事件名单不对"
NEVN=$(./build/vidowmsg events 2>/dev/null |
	python3 -c "import json,sys; d=json.load(sys.stdin); print(d['count'], len(d['events']))")
[ "$NEVN" = "$(echo "$NEVN" | awk '{print $2" "$2}')" ] && [ "${NEVN% *}" -ge 25 ] &&
	pass "vidowmsg events：$NEVN（count 和名单长度一致）" ||
	fail "vidowmsg events 不一致：$NEVN"

# ------------------------------------------------------------ 2) 命令钩子
mark "2) 命令钩子：事件字段变成环境变量"
openwin th-1
waitfor "$HKDIR/open.log" && pass "window-opened 把命令钩子叫起来了" || fail "命令钩子没跑"
grep -aq "^open:window-opened:pattern.py:th-1$" "$HKDIR/open.log" &&
	pass "\$VIDOW_EVENT / \$VIDOW_APP_ID / \$VIDOW_TITLE 都对（$(cat "$HKDIR/open.log")）" \
	|| fail "环境变量不对：$(cat "$HKDIR/open.log")"
# 没有 IPC 订阅者（本测试全程不订阅）也能跑 —— 上面这两条就是证据
pass "（本测试一个订阅者都没有，钩子照样收到事件）"

# ------------------------------------------------------------ 3) "*" 与 Lua
mark "3) '*' 收全部事件 / Lua 钩子的载荷"
waitfor "$HKDIR/any.log" "window-opened" && pass "'*' 钩子收到 window-opened" \
	|| fail "'*' 钩子没收到 window-opened"
grep -aq "window-title-changed" "$HKDIR/any.log" &&
	pass "'*' 钩子收到 window-title-changed（开窗带的那一串都算）" \
	|| fail "'*' 钩子漏了 window-title-changed"
grep -aq "workspace-created" "$HKDIR/any.log" &&
	pass "'*' 钩子收到 workspace-created" || fail "'*' 钩子漏了 workspace-created"
waitfor "$HKDIR/lua.log" "from-exec" && pass "Lua 钩子跑到底（钩子里 await vidow.exec）" \
	|| fail "Lua 钩子没跑完：$(cat "$HKDIR/lua.log" 2>/dev/null)"
grep -aq "^lua:pattern.py:window-opened$" "$HKDIR/lua.log" &&
	pass "Lua 钩子拿到表：e.data.app_id / e.event" \
	|| fail "Lua 钩子的载荷不对：$(cat "$HKDIR/lua.log")"
grep -aq "^exec:from-exec$" "$HKDIR/lua.log" &&
	pass "vidow.exec() 在钩子里返回输出（$(grep -a '^exec:' "$HKDIR/lua.log")）" \
	|| fail "vidow.exec 在钩子里的返回值不对"
grep -aq "钩子 #4 出错：.*故意炸一个" "$LOG" &&
	pass "出错的钩子点了名（哪条、什么错）" || fail "出错的钩子日志里没点名"
./build/vidowmsg state >/dev/null 2>&1 &&
	pass "有钩子炸了之后合成器还活着（state 答得上）" || fail "有钩子炸了之后合成器倒了"

# ------------------------------------------------------------ 4) 摘钩子
mark "4) vidow.off()：跑一次就摘掉自己"
waitfor "$HKDIR/once.log" && [ "$(wc -l < "$HKDIR/once.log" 2>/dev/null)" = "1" ] &&
	pass "once 钩子只跑了一次" || fail "once 钩子跑了 $(wc -l < "$HKDIR/once.log" 2>/dev/null) 次"
openwin th-2
sleep 1
[ "$(wc -l < "$HKDIR/once.log")" = "1" ] &&
	pass "第二个窗口不再触发它（vidow.off 真的摘掉了）" \
	|| fail "摘掉的钩子还在跑（$(wc -l < "$HKDIR/once.log") 行）"
[ "$(wc -l < "$HKDIR/open.log")" = "2" ] &&
	pass "没摘的那个钩子两次都跑（open.log 两行）" \
	|| fail "open.log 行数不对：$(wc -l < "$HKDIR/open.log")"

# ------------------------------------------------------------ 5) 自定义事件
mark "5) IPC 的 emit：自己发事件，订阅者和钩子都收到"
OUT=$(./build/vidowmsg --raw emit test-ping --data '{"n":7}' 2>&1)
case "$OUT" in
*'"hooks":2'*)
	pass "emit 报出 2 条钩子会收到（test-ping 那条 + '*'）" ;;
*)
	fail "emit 的应答不对：$OUT" ;;
esac
waitfor "$HKDIR/ping.log" && grep -aq "^ping:7$" "$HKDIR/ping.log" &&
	pass "data 里的字段展开成 \$VIDOW_N（ping:7）" \
	|| fail "自定义事件的 data 没展开：$(cat "$HKDIR/ping.log" 2>/dev/null)"
OUT=$(./build/vidowmsg --raw emit nobody-listens --data '{"x":1}' 2>&1)
case "$OUT" in
*'"hooks":1'*)
	pass "没人专门听的自定义事件也照样成功（只有 '*' 接住）" ;;
*)
	fail "没人的 emit 失败了：$OUT" ;;
esac
# 订阅者那一侧：订阅 + emit，两个都得收到
( ./build/vidowmsg --raw watch test-ping > "$RUNDIR/watch.log" 2>&1 &
	echo $! > "$RUNDIR/watch.pid" )
sleep 0.8
./build/vidowmsg --raw emit test-ping --data '{"n":9}' >/dev/null 2>&1
sleep 0.8
grep -aq '"event":"test-ping"' "$RUNDIR/watch.log" &&
	pass "订阅者也收到同一个自定义事件（watch 里有一条）" \
	|| fail "订阅者没收到自定义事件：$(tr '\n' ' ' < "$RUNDIR/watch.log" | head -c 200)"
kill "$(cat "$RUNDIR/watch.pid")" 2>/dev/null

# ------------------------------------------------------------ 6) 重载
mark "6) 配置重载：钩子重新声明，不重复累积"
BEFORE=$(nhooks)
sed -i 's/vidow.on("\*", /vidow.on("*", /' "$CFGDIRV/config.lua"
kill -HUP "$(our_vidow_pids | head -1)" 2>/dev/null
sleep 1.5
grep -aq "config-reloaded" "$HKDIR/any.log" &&
	pass "重载本身也是事件（* 钩子收到 config-reloaded）" \
	|| fail "重载事件没到"
# 重载前少一条是正常的（once 那条把自己摘了）；重载 = 配置里声明的 6 条重来
[ "$(nhooks)" = "6" ] && pass "重载之后钩子回到配置里声明的 6 条（旧的一批作废、once 那条也回来了）" \
	|| fail "重载之后钩子变成 $(nhooks) 条（期望 6；重载前是 $BEFORE）"
# 重载之后新开的窗照样走钩子（每开一窗一条）
openwin th-3
sleep 1
[ "$(wc -l < "$HKDIR/open.log")" = "3" ] &&
	pass "重载后新窗口的还是走钩子（open.log 3 行）" \
	|| fail "重载后钩子不出活了：$(wc -l < "$HKDIR/open.log") 行"
snap_hooks
FIRED=$(hexpr "[x['fired'] for x in h['hooks'] if x['event']=='window-opened' and x['kind']=='cmd'][0]")
case "$FIRED" in
''|0) fail "fired 计数没动（$FIRED）" ;;
*)	pass "fired 计数在涨（那条命令钩子跑了 $FIRED 次）" ;;
esac

# ------------------------------------------------------------ 7) 收尾
mark "7) 收尾"
stop_clients
./build/vidowmsg action quit >/dev/null 2>&1
waitgone && pass "退出干净" || { fail "进程还在"; our_vidow_pids; }
grep -aq "Assertion" "$LOG" && fail "退出时日志里有 assert" || pass "退出时没有 assert"

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-hooks.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
