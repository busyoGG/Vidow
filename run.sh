#!/bin/bash
# run.sh —— 起 vidow（tty 真机 / 嵌套窗口），顺手把 portal 也验一遍，退出时清干净。
#
#   ./run.sh                    真机会话（DRM 后端；WAYLAND_DISPLAY 没设时的默认）
#   ./run.sh --nested           嵌在当前 Wayland 会话里（一个窗口）
#   ./run.sh --portal           起来之后跑 ./portaltest.sh（体检 + 一个真请求）
#   ./run.sh --portal-check     只体检（./portaltest.sh --check，不动屏幕）
#   ./run.sh --debug            VIDOW_LOG=debug（日志没过滤，会很大；要过滤用 ./dbg.sh）
#   ./run.sh --no-portal-env    用一份临时配置起：不 import 环境、不重启 portal、不写
#                               portals.conf。代价：你那份 config.lua 也不生效
#                               （vidow 没有 --config，只能换 XDG_CONFIG_HOME）
#   ./run.sh --print            只打印要跑的命令，不真起
#   ./run.sh -h|--help
#
# 环境变量：VIDOW_BIN（默认 ./build/vidow）、VIDOW_RUN_LOG（默认 /tmp/vidow-run.log）、
#           VIDOW_RENDER_DRM_DEVICE（--nested 用的渲染节点，默认 /dev/dri/renderD128）
#
# 退出时（Ctrl+C、vidow 自己退、脚本被杀）做三件事：
#   ① 收 vidow：SIGTERM → 等 5 秒 → SIGKILL，顺带收它那个进程组里的子进程。
#   ② 扫**残留客户端**：环境里 WAYLAND_DISPLAY 正好是我们这个 socket 的进程。vidow 一死
#      它们本该跟着走，但有些会活下来空转 —— 实测 wev 两个进程各 100% CPU 烧了 25 分钟，
#      另一台会话里游戏掉帧、加载慢就是它（README 变更记录里那条测试收尾漏杀）。这些
#      孤儿会被点名收掉。
#   ③ 看一眼有没有新 coredump：vidow 退出时段错误是老 bug，脚本把栈的前几帧打出来，
#      免得它悄悄崩（一次 2MB，coredumpctl 里已经堆了一百多个）。
#
# 为什么 --nested 要自己补 WLR_RENDER_DRM_DEVICE：wlroots 0.20.2 的 GLES2 渲染器要一个
# DRM fd，而 wayland 后端不提供 —— 不补就是 "Cannot create GLES2 renderer: no DRM FD
# available"，起不来（README 那条 `WLR_BACKENDS=wayland ./build/vidow` 少了这一半）。
set -u
cd "$(dirname "$0")"

BIN=${VIDOW_BIN:-./build/vidow}
LOG=${VIDOW_RUN_LOG:-/tmp/vidow-run.log}
DRMNODE=${VIDOW_RENDER_DRM_DEVICE:-/dev/dri/renderD128}

MODE=""
DEBUG=0
PORTAL=0
PORTAL_CHECK=0
NO_PORTAL_ENV=0
PRINT=0

usage() {
	sed -n '2,25p' "$0" | sed 's/^# \?//'
}

die() { echo "run.sh: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
	--tty) MODE=tty ;;
	--nested) MODE=nested ;;
	--debug) DEBUG=1 ;;
	--portal) PORTAL=1 ;;
	--portal-check) PORTAL_CHECK=1 ;;
	--no-portal-env) NO_PORTAL_ENV=1 ;;
	--print) PRINT=1 ;;
	-h|--help) usage; exit 0 ;;
	*) die "不认识的参数 $1（--help 看用法）" ;;
	esac
	shift
done

[ -x "$BIN" ] || die "没有 $BIN（先 meson setup build && ninja -C build）"

# 后端：没指定就看当前环境 —— 有 WAYLAND_DISPLAY 说明人在图形会话里，默认嵌套；
# 在裸 tty 上（WAYLAND_DISPLAY 为空）默认真机。
if [ -z "$MODE" ]; then
	if [ -n "${WAYLAND_DISPLAY:-}" ]; then
		MODE=nested
	else
		MODE=tty
	fi
fi
if [ "$MODE" = tty ] && [ -n "${WAYLAND_DISPLAY:-}" ]; then
	die "WAYLAND_DISPLAY=${WAYLAND_DISPLAY} 还设着，却在跑真机会话模式 ——
     从图形会话里的终端起 vidow 请用 --nested；真机请在裸 tty 上跑"
fi
if [ "$MODE" = nested ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
	die "--nested 但 WAYLAND_DISPLAY 没设（从裸 tty 起不来，得有个宿主合成器）"
fi

# 同一个用户只有一条 session bus，portal 只能指向一个合成器 —— 两个 vidow 一起跑，
# portaltest.sh 会判「有不止一个 vidow 在跑」。
if pgrep -x vidow >/dev/null 2>&1; then
	die "已经有 vidow 在跑（pid $(pgrep -x vidow | tr '\n' ' ')）—— 先退掉它再起"
fi

[ -n "${XDG_RUNTIME_DIR:-}" ] || export XDG_RUNTIME_DIR="/run/user/$(id -u)"

ENVS=()
[ "$DEBUG" = 1 ] && ENVS+=(VIDOW_LOG=debug)
if [ "$MODE" = nested ]; then
	ENVS+=(WLR_BACKENDS=wayland)
	# 见文件头：wlroots 0.20.2 的 GLES2 渲染器要 DRM fd，wayland 后端不给。
	if [ -z "${WLR_RENDER_DRM_DEVICE:-}" ]; then
		ENVS+=(WLR_RENDER_DRM_DEVICE="$DRMNODE")
	fi
fi

# --no-portal-env：临时配置目录。vidow 只认 $XDG_CONFIG_HOME/vidow/config.lua，
# 所以只能整个换掉配置目录 —— 临时那份只写 portal 两行，其余全默认。
CLEANCFG=""
if [ "$NO_PORTAL_ENV" = 1 ]; then
	CLEANCFG=$(mktemp -d /tmp/vidow-noportal-XXXXXX) || die "建临时配置目录失败"
	mkdir -p "$CLEANCFG/vidow"
	cat > "$CLEANCFG/vidow/config.lua" <<'LUA'
-- run.sh --no-portal-env 的临时配置：只关 portal 那一摊，其余全默认。
-- 代价：你那份 ~/.config/vidow/config.lua 也不生效（vidow 没有 --config 参数，
-- 只能靠 XDG_CONFIG_HOME 换目录），键位那些也是默认的。
portal = { conf_sync = false, desktop = "" }
LUA
	ENVS+=(XDG_CONFIG_HOME="$CLEANCFG")
fi

if [ "$PRINT" = 1 ]; then
	echo "模式：$MODE"
	echo "命令：setsid env ${ENVS[*]:-} $BIN >> $LOG 2>&1 </dev/null &"
	[ "$PORTAL" = 1 ] && echo "起来之后：./portaltest.sh"
	[ "$PORTAL_CHECK" = 1 ] && echo "起来之后：./portaltest.sh --check"
	[ -n "$CLEANCFG" ] && echo "临时配置：$CLEANCFG/vidow/config.lua"
	exit 0
fi

V=""
SOCK=""
START_TS=$(date +%s)
CLEANED=0

# ---------------------------------------------------------------- 收尾

# 某个 pid 的环境变量（一行一个 KEY=VALUE）。只扫自己名下的进程：/proc/<pid>/environ
# 读别人的会报「权限不够」，而那条报错来自**重定向**（不是 tr），压不掉 —— 所以先按
# owner 过一遍，再把重定向一起塞进 stderr 黑洞。
own_env() {
	[ "$(stat -c %u "/proc/$1" 2>/dev/null)" = "$(id -u)" ] || return 1
	{ tr '\0' '\n' < "/proc/$1/environ"; } 2>/dev/null
}

# 残留客户端：环境里 WAYLAND_DISPLAY 正好是我们这个 socket 的进程。vidow 自己也有这个
# 环境变量（它自己 setenv 的），按 pid 排除。
reap_leftovers() {
	local pid comm killed=0 p
	[ -n "$SOCK" ] || return 0
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$V" ] && continue
		own_env "$pid" | grep -qx "WAYLAND_DISPLAY=$SOCK" || continue
		comm=$(cat "/proc/$pid/comm" 2>/dev/null || echo "?")
		echo "  收残留客户端：pid $pid $comm（WAYLAND_DISPLAY=$SOCK）"
		kill -TERM "$pid" 2>/dev/null
		killed=$((killed + 1))
	done
	if [ "$killed" = 0 ]; then
		echo "  没有残留客户端"
		return 0
	fi
	sleep 1
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$V" ] && continue
		own_env "$pid" | grep -qx "WAYLAND_DISPLAY=$SOCK" || continue
		echo "  还不走，SIGKILL：pid $pid $(cat "/proc/$pid/comm" 2>/dev/null)"
		kill -KILL "$pid" 2>/dev/null
	done
	echo "  共收了 $killed 个（vidow 死了它们本该自己走，没走就是 bug）"
}

check_coredump() {
	command -v coredumpctl >/dev/null 2>&1 || return 0
	local n
	n=$(coredumpctl list --since "@$START_TS" --no-legend 2>/dev/null |
		grep -c vidow)
	if [ "${n:-0}" = 0 ]; then
		echo "  没有新 coredump（这次是正常退出）"
		return 0
	fi
	echo "  ⚠ 有 $n 个新 coredump：vidow 是崩着退的（不是正常退出）"
	coredumpctl list --since "@$START_TS" --no-legend 2>/dev/null |
		grep vidow | tail -3 | sed 's/^/    /'
	coredumpctl info --since "@$START_TS" 2>/dev/null |
		grep -A5 "Stack trace of thread" | head -8 | sed 's/^/    /'
}

cleanup() {
	[ "$CLEANED" = 1 ] && return 0
	CLEANED=1
	trap - EXIT INT TERM
	echo
	echo "== 收尾 =="
	if [ -n "$V" ] && kill -0 "$V" 2>/dev/null; then
		pgid=$(ps -o pgid= -p "$V" 2>/dev/null | tr -d ' ')
		if [ -n "$pgid" ]; then
			kill -TERM -- "-$pgid" 2>/dev/null || kill -TERM "$V" 2>/dev/null
		else
			kill -TERM "$V" 2>/dev/null
		fi
		for _ in $(seq 1 25); do
			kill -0 "$V" 2>/dev/null || break
			sleep 0.2
		done
		if kill -0 "$V" 2>/dev/null; then
			echo "  vidow 没理 SIGTERM，SIGKILL"
			[ -n "$pgid" ] && kill -KILL -- "-$pgid" 2>/dev/null
			kill -KILL "$V" 2>/dev/null
			sleep 0.5
		fi
		echo "  vidow（pid $V）收掉了"
	else
		echo "  vidow 已经不在（自己退的？）"
	fi
	reap_leftovers
	check_coredump
	[ -n "$CLEANCFG" ] && rm -rf "$CLEANCFG"
	echo "日志：$LOG（$(wc -l < "$LOG" 2>/dev/null || echo 0) 行）"
	echo "== 收尾结束 =="
}
trap cleanup EXIT INT TERM

# ---------------------------------------------------------------- 起

echo "== 起 vidow（$MODE）=="
if [ "$NO_PORTAL_ENV" = 1 ]; then
	echo "  --no-portal-env：用临时配置 $CLEANCFG（不碰 portal，你的 config.lua 也不生效）"
else
	echo "  portal：vidow 启动会 import-environment + 必要时重启 xdg-desktop-portal，"
	echo "          并同步 portals.conf；退出时它自己放回去（src/portalconf.c）"
fi
UENV=$(systemctl --user show-environment 2>/dev/null |
	grep -E '^(XDG_CURRENT_DESKTOP|WAYLAND_DISPLAY)=' | tr '\n' ' ')
echo "  前置：${UENV:-（没有 systemd 用户管理器）}"

: > "$LOG"
setsid env ${ENVS[@]:-} "$BIN" >>"$LOG" 2>&1 </dev/null &

for _ in $(seq 1 150); do
	SOCK=$(sed -n 's/.*vidow ready on WAYLAND_DISPLAY=//p' "$LOG" | tail -1)
	[ -n "$SOCK" ] && break
	sleep 0.2
done
if [ -z "$SOCK" ]; then
	echo "vidow 30 秒没起来。日志尾部：" >&2
	tail -20 "$LOG" >&2
	exit 1
fi

V=$(pgrep -x "$(basename "$BIN")" 2>/dev/null | head -1)
[ -n "$V" ] || V=$(pgrep -f "$BIN" 2>/dev/null | head -1)
XDISPLAY=""
if [ -n "$V" ] && [ -r "/proc/$V/environ" ]; then
	XDISPLAY=$(tr '\0' '\n' < "/proc/$V/environ" |
		sed -n 's/^DISPLAY=//p' | head -1)
fi
IPC="$XDG_RUNTIME_DIR/vidow-$SOCK.sock"

echo
echo "vidow pid=$V  WAYLAND_DISPLAY=$SOCK  DISPLAY=${XDISPLAY:-（Xwayland 还没起，懒启动）}"
echo "IPC  $IPC"
echo "日志 tail -f $LOG"

# ---------------------------------------------------------------- portal

if [ "$PORTAL" = 1 ] || [ "$PORTAL_CHECK" = 1 ]; then
	echo
	if [ "$NO_PORTAL_ENV" = 1 ]; then
		echo "注意：--no-portal-env 下 vidow 不碰 portal 环境，portaltest.sh 的"
		echo "      ②③④（用户管理器环境 / portal 进程指向）必然判 FAIL —— 那是在测"
		echo "      「vidow 会不会接管 portal」，这次要看的只有 ①⑤ 和 vidow 自己的日志。"
	fi
	if [ "$PORTAL_CHECK" = 1 ]; then
		./portaltest.sh --check
	else
		echo "== 跑 ./portaltest.sh（会弹一个真的文件选择器对话框）=="
		./portaltest.sh
	fi
	echo
	echo "（portal 那几项要连着测，就跑 ./portaltest.sh / ./test-portal.sh / ./test-screencast.sh）"
fi

if [ -t 0 ]; then
	echo
	echo "== 现在这块 tty 归 vidow 用；Ctrl+C 退出（脚本会收尾）=="
	# 前台等 vidow：它自己退出（键位 / IPC quit）也走同一条收尾
	while [ -n "$V" ] && kill -0 "$V" 2>/dev/null; do
		sleep 1
	done
	echo "vidow 退出了"
else
	echo "（stdin 不是终端，不等交互；要前台守着就别重定向输入）"
fi
