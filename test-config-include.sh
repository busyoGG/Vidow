#!/bin/bash
# 配置拆成好几份：vidow.include()（入口 config.lua + configs/ 下的每一类）。
#
# 判据：
#   1) include 进来的文件里的键真的生效；日志里点名了每一份
#   2) 相对路径按**正在 include 的那个文件所在目录**解析（configs 里互相 include
#      也照样好使）；绝对路径照用
#   3) include 的先后 = 键生效的先后（同一个键只有第一次写的算数）
#   4) include 进来的文件里能调 vidow.exec()（yield 要能穿过 include 这一层）
#   5) 改 / 删 / 补一份 include 进来的文件都自动重载，日志里点名是哪一份
#   6) 报错（文件不在 / 语法错 / 自己 include 自己）当场喊，只带走后面那些键，
#      进程活着、IPC 照常
#   7) 重载之后不再 include 的那份文件，改了也不重载
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-config-include-test
CFGDIR="$RUNDIR/home"
CFGDIRV="$CFGDIR/.config/vidow"
CONFIGS="$CFGDIRV/configs"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

# 被 timeout / Ctrl-C 干掉时也要收干净（EXIT 陷阱在 SIGTERM 下不会跑）
trap 'exit 143' TERM INT
trap cleanup EXIT

# 兜底：按 $RUNDIR 认领漏网的 vidow（environ 里有这个 RUNDIR 的才是本次的）
kill_stray_vidows() {
	local p
	for p in /proc/[0-9]*; do
		[ -r "$p/cmdline" ] || continue
		grep -aq "build/vidow" "$p/cmdline" 2>/dev/null || continue
		[ -r "$p/environ" ] || continue
		grep -aqz "$RUNDIR" "$p/environ" 2>/dev/null || continue
		kill -KILL "${p#/proc/}" 2>/dev/null
	done
}

cleanup() {
	if [ -n "$VIDOW_PID" ]; then
		kill -TERM -- "-$VIDOW_PID" 2>/dev/null
		sleep 0.3
		kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	fi
	kill_stray_vidows
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}

mkdir -p "$CONFIGS" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

CFG="$CFGDIRV/config.lua"
write_entry() { printf '%s\n' "$@" > "$CFG"; }

# 一开始的树：
#   01-a  基础（headless 要的 scale / xwayland）+ gaps = 15
#   02-b  也写 gaps = 99（先写的算数，这个不该生效）+ decor_enabled = false
#   03-c  include 同一目录下的 04-d.lua（相对路径按自己的目录解析）
#   04-d  corner_radius = 3
#   05-e  include 配置目录**外面**的文件（绝对路径）
#   06-f  用 vidow.exec() 拿值（yield 要能穿过 include）
write_entry \
	'vidow.include("configs/01-a.lua")' \
	'vidow.include("configs/02-b.lua")' \
	'vidow.include("configs/03-c.lua")' \
	'vidow.include("configs/05-e.lua")' \
	'vidow.include("configs/06-f.lua")'
cat > "$CONFIGS/01-a.lua" <<'EOF'
scale = "1"
xwayland = false
gaps = 15
EOF
cat > "$CONFIGS/02-b.lua" <<'EOF'
gaps = 99
decor_enabled = false
window_opacity = 0.5
EOF
cat > "$CONFIGS/03-c.lua" <<'EOF'
vidow.include("04-d.lua")
EOF
cat > "$CONFIGS/04-d.lua" <<'EOF'
corner_radius = 3
EOF
printf 'force_render_hz = 11\n' > "$RUNDIR/outside.lua"
cat > "$CONFIGS/05-e.lua" <<EOF
vidow.include("$RUNDIR/outside.lua")
EOF
cat > "$CONFIGS/06-f.lua" <<'EOF'
cursor_shake_scale = tonumber(vidow.exec("echo 3"))
EOF

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
		echo "test-config-include.sh: vidow 没起来"
		grep -a "ERROR" "$LOG" | head -5
		exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
}

stop_vidow() {
	[ -n "$VIDOW_PID" ] || return
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

logcount() { grep -ac "$1" "$LOG"; }

alive() { # IPC 还能用 = 进程还活着
	./build/vidowmsg get-config gaps >/dev/null 2>&1
}

# ------------------------------------------------------------ 1) 读进来
mark "入口 + configs/：include 进来的键生效，日志点名每一份"
start_vidow
[ "$(getcfg gaps)" = "15" ] && pass "configs/01-a.lua 里的 gaps = 15 生效" \
	|| fail "gaps 应该是 15，读到 $(getcfg gaps)"
[ "$(getcfg decor_enabled)" = "false" ] &&
	pass "configs/02-b.lua 里的 decor_enabled = false 生效" \
	|| fail "decor_enabled 应该是 false，读到 $(getcfg decor_enabled)"
grep -aq "include .*configs/01-a.lua" "$LOG" && pass "日志里点名了 01-a.lua" \
	|| fail "日志里没有 include 01-a.lua 的记录"
grep -aq "include .*configs/06-f.lua" "$LOG" && pass "日志里点名了 06-f.lua" \
	|| fail "日志里没有 include 06-f.lua 的记录"


mark "相对路径按「正在 include 的那个文件」所在目录解析；绝对路径照用"
[ "$(getcfg corner_radius)" = "3" ] &&
	pass "configs/03-c.lua 里 include(\"04-d.lua\") 找到的是 configs/04-d.lua" \
	|| fail "相对路径没按文件所在目录解析（corner_radius = $(getcfg corner_radius)）"
[ "$(getcfg force_render_hz)" = "11" ] &&
	pass "绝对路径 include 配置目录外面的文件也生效（force_render_hz = 11）" \
	|| fail "绝对路径 include 没生效（force_render_hz = $(getcfg force_render_hz)）"

mark "include 进来的文件里能调 vidow.exec()（yield 穿过 include 这一层）"
waitcfg cursor_shake_scale 3 && pass "vidow.exec(\"echo 3\") 拿到的值落地了" \
	|| fail "exec 的值没落地（cursor_shake_scale = $(getcfg cursor_shake_scale)）"

# ------------------------------------------------------------ 2) 自动重载
mark "改一份 include 进来的文件：自动重载，日志点名是哪一份"
BEFORE=$(logcount "02-b.lua（include 进来的） 变了")
sed -i 's/window_opacity = 0.5/window_opacity = 0.4/' "$CONFIGS/02-b.lua"
waitcfg window_opacity 0.4 && pass "改 configs/02-b.lua，window_opacity 自己变成 0.4" \
	|| fail "include 进来的文件改了没自动重载（window_opacity = $(getcfg window_opacity)）"
[ "$(logcount "02-b.lua（include 进来的） 变了")" -gt "$BEFORE" ] &&
	pass "日志里点名了是哪一份文件变了" || fail "日志里没点名是哪一份变了"

mark "删掉一份 include 进来的文件：当场报错，只带走后面那些键"
sed -i 's/gaps = 15/gaps = 21/' "$CONFIGS/01-a.lua"
waitcfg gaps 21 && pass "先确认重载链路是通的（gaps = 21）" \
	|| fail "gaps 没跟上（$(getcfg gaps)）"
rm -f "$CONFIGS/04-d.lua"
sleep 0.6
grep -aq "vidow.include: 打不开 .*04-d.lua" "$LOG" &&
	pass "日志里报的是「打不开 04-d.lua」" || fail "没报「打不开 04-d.lua」"
[ "$(getcfg gaps)" = "21" ] &&
	pass "报错之前的键照样生效（gaps = 21）" || fail "报错把前面的也带走了（gaps = $(getcfg gaps)）"
[ "$(getcfg corner_radius)" = "12" ] &&
	pass "报错之后的键回到默认值（corner_radius = 12）" \
	|| fail "corner_radius 应该是默认值 12，读到 $(getcfg corner_radius)"
alive && pass "报错之后进程还活着，IPC 照常" || fail "报错之后 IPC 不通了"

mark "把那份文件补回来：自动重载（盯的是目录，文件出现也算）"
cat > "$CONFIGS/04-d.lua" <<'EOF'
corner_radius = 3
EOF
waitcfg corner_radius 3 && pass "补上 04-d.lua，corner_radius 自己回到 3" \
	|| fail "补回文件没触发重载（corner_radius = $(getcfg corner_radius)）"

# ------------------------------------------------------------ 3) 报错
mark "include 的文件语法写错：报错里带 文件:行"
printf 'corner_radius = = 5\n' > "$CONFIGS/04-d.lua"
sleep 0.8
grep -aq "04-d.lua:1: " "$LOG" && pass "报错里有 04-d.lua:1" \
	|| fail "报错里没有文件名和行号：$(grep -a 'ERROR' "$LOG" | tail -1)"
alive && pass "语法错误之后进程还活着" || fail "语法错误之后 IPC 不通了"
cat > "$CONFIGS/04-d.lua" <<'EOF'
corner_radius = 3
EOF
waitcfg corner_radius 3 && pass "改回来就好了（corner_radius = 3）" \
	|| fail "改回来没生效"

mark "自己 include 自己：报错，不转死"
cat > "$CONFIGS/07-loop.lua" <<'EOF'
vidow.include("07-loop.lua")
EOF
write_entry \
	'vidow.include("configs/01-a.lua")' \
	'vidow.include("configs/02-b.lua")' \
	'vidow.include("configs/03-c.lua")' \
	'vidow.include("configs/05-e.lua")' \
	'vidow.include("configs/06-f.lua")' \
	'vidow.include("configs/07-loop.lua")'
sleep 1
grep -aq "include 套得太深了" "$LOG" && pass "报了「include 套得太深了」" \
	|| fail "自己 include 自己没报错"
alive && pass "转了一圈之后进程还活着" || fail "自己 include 自己把进程搞死了"

mark "入口里 include 一个不存在的文件：报错里带入口的行号和路径"
sed -i 's|vidow.include("configs/07-loop.lua")|vidow.include("configs/99-nope.lua")|' "$CFG"
sleep 0.8
grep -aq "config.lua:6: vidow.include: 打不开 .*99-nope.lua" "$LOG" &&
	pass "报错带上了入口的行号（config.lua:6）" \
	|| fail "报错没带入口行号：$(grep -a 'ERROR' "$LOG" | tail -1)"

# ------------------------------------------------------------ 4) 重载后不再 include 的那份
mark "重载之后不再 include 的那份文件：改了也不重载"
write_entry \
	'vidow.include("configs/01-a.lua")' \
	'vidow.include("configs/03-c.lua")'
waitcfg decor_enabled true && pass "把 02-b 从入口里去掉，它的键回默认值了" \
	|| fail "去掉 include 之后键没回默认值（decor_enabled = $(getcfg decor_enabled)）"
BEFORE=$(logcount "02-b.lua（include 进来的） 变了")
sed -i 's/window_opacity = 0.4/window_opacity = 0.3/' "$CONFIGS/02-b.lua"
sleep 1
[ "$(logcount "02-b.lua（include 进来的） 变了")" = "$BEFORE" ] &&
	pass "不再 include 的 02-b.lua 改了，没有重载" || fail "不 include 的文件也触发了重载"
[ "$(getcfg window_opacity)" = "1" ] &&
	pass "它的键也确实没进来（window_opacity = 1）" \
	|| fail "不 include 的文件居然生效了（window_opacity = $(getcfg window_opacity)）"

stop_vidow
echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-config-include.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
