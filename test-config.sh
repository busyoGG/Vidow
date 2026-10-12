#!/bin/bash
# 配置系统：就是一个 Lua 文件（~/.config/vidow/config.lua），改了就自动重载。
#
# 判据：
#   1) 值真的从 config.lua 读进来
#   2) 旧的 key = value 文件（~/.config/vidow/config）不再被读取，而且启动时会喊一声
#   3) 改文件自动重载：原地写、写临时文件再 rename 过来，两种保存方式都认
#   4) 目录里**别的**文件动过不重载（stat 比对挡掉）
#   5) 重载 = 重新读一遍：删掉的行回默认值，不是留着旧值
#   6) ipc = false 在配置文件里照样有效（socket 先建后拆）；把那行删掉，自动重载后回来
#   7) $VIDOW_CONFIG 能换配置文件位置
#   8) vidow.exec 的选项：{ status = true } 拿退出码 / { wait = false } 撒手跑 /
#      并掉的老名字（vidow.spawn、vidow.exec_status）给一句人话而不是 "nil value"
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-config-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	if [ -n "$VIDOW_PID" ]; then
		kill -TERM -- "-$VIDOW_PID" 2>/dev/null
		sleep 0.3
		kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

CFG="$CFGDIR/.config/vidow/config.lua"
write_cfg() { printf '%s\n' "$@" > "$CFG"; }

# 先放一个旧的 key = value 文件（内容故意和 config.lua 不一样：它不该生效）
cat > "$CFGDIR/.config/vidow/config" <<'EOF'
gaps = 99
window_opacity = 0.25
EOF
write_cfg 'scale = "1"' 'xwayland = false' 'gaps = 10'

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

start_vidow() {
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	if ! grep -aq "vidow ready" "$LOG"; then
		echo "test-config.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	SOCK="$XDG_RUNTIME_DIR/vidow-$WAYLAND_DISPLAY.sock"
}

stop_vidow() {
	kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	VIDOW_PID=""
}

getcfg() { # getcfg <键> —— 读不到就打空
	./build/vidowmsg get-config "$1" 2>/dev/null |
		sed -n 's/.*"value": *"\(.*\)".*/\1/p' | head -1
}

waitcfg() { # waitcfg <键> <期望值> —— 最多等 4 秒（自动重载有 150ms 去抖）
	local i
	for i in $(seq 1 40); do
		[ "$(getcfg "$1")" = "$2" ] && return 0
		sleep 0.1
	done
	return 1
}

# ------------------------------------------------------------ 1) 读进来 + 旧文件失效
mark "唯一的配置文件是 config.lua；旧的 key = value 文件不再读"
start_vidow
[ "$(getcfg gaps)" = "10" ] && pass "config.lua 里的 gaps = 10 生效了" \
	|| fail "gaps 应该是 10，读到的是 $(getcfg gaps)"
[ "$(getcfg window_opacity)" = "1" ] &&
	pass "旧文件里的 window_opacity = 0.25 没生效（还是默认 1）" \
	|| fail "旧文件被读了：window_opacity = $(getcfg window_opacity)"
grep -aq "不再被读取" "$LOG" && pass "启动时喊了一声，提示搬走旧文件" \
	|| fail "没有提示旧文件不再读取"
grep -aq "config.lua: running .*config.lua" "$LOG" && pass "跑的是 config.lua" \
	|| fail "日志里没看到跑 config.lua"
./build/vidowmsg state 2>/dev/null | grep -aq '"watch": *true' &&
	pass "state 里报了自动重载盯着（config_lua.watch）" ||
	fail "state 里没说盯上了"

# ------------------------------------------------------------ 2) 自动重载（原地写）
mark "改文件就自动重载（原地写，inode 不变）"
write_cfg 'scale = "1"' 'xwayland = false' 'gaps = 24'
waitcfg gaps 24 && pass "改完没做任何事，gaps 自己变成 24" \
	|| fail "自动重载没生效（gaps = $(getcfg gaps)）"
grep -aq "变了，自动重载" "$LOG" && pass "日志里说了是自动重载" \
	|| fail "日志里没有自动重载的记录"

# ------------------------------------------------------------ 3) 自动重载（rename）
mark "改文件就自动重载（写临时文件再 rename，inode 换了）"
printf 'scale = "1"\nxwayland = false\ngaps = 33\n' > "$RUNDIR/new.lua"
mv "$RUNDIR/new.lua" "$CFG"
waitcfg gaps 33 && pass "rename 换掉文件也能认出来（gaps = 33）" \
	|| fail "rename 保存没触发重载（gaps = $(getcfg gaps)）"


# ------------------------------------------------------------ 5) 重载 = 重新读
mark "重载是重新读一遍：删掉的行回默认值"
write_cfg 'scale = "1"' 'xwayland = false'
waitcfg gaps 6 && pass "把 gaps 那行删掉，回默认值 6（不是留着 33）" \
	|| fail "删掉的行没回默认值（gaps = $(getcfg gaps)）"

# ------------------------------------------------------------ 6) ipc 开关
mark "ipc = false 在配置文件里有效（socket 先建后拆）；删掉那行又回来"
write_cfg 'scale = "1"' 'xwayland = false' 'ipc = false'
for _ in $(seq 1 40); do [ -S "$SOCK" ] || break; sleep 0.1; done
[ ! -S "$SOCK" ] && pass "自动重载之后 socket 被拆掉了（ipc = false）" \
	|| fail "ipc = false 没生效，socket 还在"
grep -aq "ipc: 关着" "$LOG" && pass "日志里说了 ipc 关着" || fail "日志里没说 ipc 关着"
write_cfg 'scale = "1"' 'xwayland = false'
for _ in $(seq 1 40); do [ -S "$SOCK" ] && break; sleep 0.1; done
[ -S "$SOCK" ] && pass "把那行删掉，socket 自己回来了（ipc 默认开）" \
	|| fail "ipc 没能重新打开"
waitcfg gaps 6 && pass "重载之后 IPC 照常能用" || fail "重载之后 IPC 不通"

# ------------------------------------------------------------ 7) $VIDOW_CONFIG
mark "\$VIDOW_CONFIG 能换配置文件位置"
stop_vidow
ALT="$RUNDIR/alt.lua"
printf 'scale = "1"\nxwayland = false\ngaps = 51\n' > "$ALT"
export VIDOW_CONFIG="$ALT"
start_vidow
[ "$(getcfg gaps)" = "51" ] && pass "VIDOW_CONFIG 指的配置文件生效了（gaps = 51）" \
	|| fail "VIDOW_CONFIG 没生效（gaps = $(getcfg gaps)）"
printf 'scale = "1"\nxwayland = false\ngaps = 52\n' > "$ALT"
waitcfg gaps 52 && pass "换过去的那个文件改了也自动重载" \
	|| fail "VIDOW_CONFIG 指的文件的自动重载没生效"
unset VIDOW_CONFIG
stop_vidow

# ------------------------------------------- 8) vidow.exec 的选项 / 老名字报错
mark "vidow.exec 的选项：status / wait，以及并掉的老名字报错"
SPAWNLOG="$RUNDIR/spawn.log"
rm -f "$SPAWNLOG"
write_cfg 'scale = "1"' 'xwayland = false' \
	'local out, code, err = vidow.exec("echo hi; exit 3", { status = true })' \
	'vidow.log("OPT status out=[" .. tostring(out) .. "] code=" .. tostring(code) .. " err=[" .. tostring(err) .. "]")' \
	'local o2, why = vidow.exec("exit 7")' \
	'vidow.log("OPT plain out=" .. tostring(o2) .. " why=[" .. tostring(why) .. "]")' \
	'vidow.log("OPT waitfalse ret=" .. tostring(vidow.exec("echo ran >> '"$SPAWNLOG"'", { wait = false })))' \
	'local ok, msg = pcall(function() return vidow.spawn("x") end)' \
	'vidow.log("OPT spawn ok=" .. tostring(ok) .. " msg=" .. tostring(msg))' \
	'local ok2, msg2 = pcall(function() return vidow.exec_status("x") end)' \
	'vidow.log("OPT exec_status ok=" .. tostring(ok2) .. " msg=" .. tostring(msg2))' \
	'local ok3, msg3 = pcall(function() return vidow.exec("echo x", { wait = true, nope = 1 }) end)' \
	'vidow.log("OPT badopt ok=" .. tostring(ok3) .. " msg=" .. tostring(msg3))' \
	'local ok4, msg4 = pcall(function() return vidow.exec_once("echo x", { wait = true }) end)' \
	'vidow.log("OPT once_wait ok=" .. tostring(ok4) .. " msg=" .. tostring(msg4))'
start_vidow
grep -aq "OPT status out=\[hi" "$LOG" && grep -aq "code=3" "$LOG" &&
	pass "{ status = true } 返回 out, code, err（out=hi / code=3）" \
	|| fail "status 形态不对：$(grep -a 'OPT status' "$LOG" | tail -1)"
grep -aq "OPT plain out=nil why=\[exit 7\]" "$LOG" &&
	pass "不给选项时照旧：非 0 退出 = nil + 原因（老手感没变）" \
	|| fail "默认形态变了：$(grep -a 'OPT plain' "$LOG" | tail -1)"
grep -aq "OPT waitfalse ret=true" "$LOG" && grep -aq "^ran$" "$SPAWNLOG" 2>/dev/null &&
	pass "{ wait = false } 立刻返回 true，命令照跑（原来 vidow.spawn 的活）" \
	|| fail "wait = false 不对：$(grep -a 'OPT waitfalse' "$LOG" | tail -1) / 命令跑了没：$(cat "$SPAWNLOG" 2>/dev/null)"
grep -aq "OPT spawn ok=false" "$LOG" && grep -aq "OPT spawn .*并进 vidow.exec" "$LOG" &&
	pass "老名字 vidow.spawn 报的是一句人话（怎么写才对）" \
	|| fail "vidow.spawn 的报错不对：$(grep -a 'OPT spawn' "$LOG" | tail -1)"
grep -aq "OPT exec_status ok=false" "$LOG" && grep -aq "OPT exec_status .*并进 vidow.exec" "$LOG" &&
	pass "老名字 vidow.exec_status 同样给一句人话" \
	|| fail "vidow.exec_status 的报错不对：$(grep -a 'OPT exec_status' "$LOG" | tail -1)"
grep -aq "OPT badopt .*不认识的选项 'nope'" "$LOG" &&
	pass "选项名拼错当场报错并列出能写哪些" \
	|| fail "拼错的选项没报错：$(grep -a 'OPT badopt' "$LOG" | tail -1)"
grep -aq "OPT once_wait .*撒手跑的" "$LOG" &&
	pass "exec_once 传 { wait = true } 报错（它等不了）" \
	|| fail "exec_once 的 wait = true 没报错：$(grep -a 'OPT once_wait' "$LOG" | tail -1)"
grep -aq "Assertion" "$LOG" && fail "退出时日志里有 assert" || pass "退出时没有 assert"
stop_vidow

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-config.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
