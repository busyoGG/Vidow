#!/bin/bash
# portal 环境的两件事：启动时推进 systemd 用户管理器、**退出时放回去**。
#
# 为什么退出要还：后端（xdg-desktop-portal-gtk 等）是 dbus 按需激活的，环境取自
# systemd 用户管理器。vidow 启动时把 XDG_CURRENT_DESKTOP / WAYLAND_DISPLAY 换成
# 自己的，退出后不还的话，回到宿主合成器（Hyprland）以后新拉起来的后端连的是已经
# 消失的 vidow socket —— portal 全线失效。而且 Linger=yes 时登出也救不回来：用户
# 管理器不随会话退出，Hyprland 自己的启动逻辑也只 import 环境、不重启
# xdg-desktop-portal。
#
# 还原的是「vidow 进来之前用户管理器里是什么」，不是 vidow 继承到的环境（从 tty
# 起来时后者本来就是空的），所以快照得问 systemctl。这里用一个假 systemctl 让这件
# 事可断言：它把每次调用记进日志，show-environment 按给定内容回答。
#
# 判据：
#   A 环境齐全 + 首次（要写 portals.conf）
#     1) 启动前问了 show-environment（快照）
#     2) 启动 import 了三个变量
#     3) 启动重启了前端（配置变了）
#     4) 退出把三个值原样 set 回去
#     5) 退出重启了前端（一共 2 次 restart）
#     6) 还原排在 import 之后
#     7) vidow 日志里有快照 / 还原两条
#     8) 值都在时不该 unset
#   B 同一份配置（portals.conf 内容没变）
#     9) 启动不再重启前端
#    10) import 照做（环境归用户管理器管，跟文件变没变无关）
#    11) 日志写明「只 import 环境」
#    12) 退出照样还原
#   C 用户管理器里本来就没有 DESKTOP / WAYLAND_DISPLAY
#    13) 退出 unset 这两个
#    14) 退出只 set XDG_RUNTIME_DIR
#   D VIDOW_PORTAL_NO_RESTART=1
#    15) 一次 systemctl 都没调
#    16) 日志写明跳过
#   E 录屏 / 截图后端
#    17) ScreenCast / Screenshot 指向本机装了的那个（wlr > hyprland）
#    18) 一个都没装就一个字都不写（写了等于骗客户端）
#    19) 日志说明白
#
# 用法：./test-portalenv.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-portalenv-test
HOME_DIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAKE="$RUNDIR/bin"
FAKELOG="$RUNDIR/systemctl.log"
FAKE_ENV="$RUNDIR/fake-env"
FAILED=0
VIDOW_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -KILL "$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

rm -rf "$RUNDIR"
mkdir -p "$HOME_DIR" "$LOG" "$FAKE" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

# 假 systemctl：只干两件事 —— 记下被怎么调的；show-environment 按 FAKE_ENV 回答。
cat >"$FAKE/systemctl" <<'FAKE_SH'
#!/bin/sh
printf '%s\n' "$*" >>"${FAKE_LOG:?}"
case "$*" in
"--user show-environment")
	[ -f "${FAKE_ENV:-}" ] || exit 1
	cat "$FAKE_ENV"
	exit 0
	;;
esac
exit 0
FAKE_SH
chmod +x "$FAKE/systemctl"

# 起一轮 headless vidow，跑起来后把启动阶段的 systemctl 调用切出来（$1.start）。
# $2 是额外环境变量（比如 VIDOW_PORTAL_NO_RESTART=1）。
run_round() {
	local name="$1" extra="${2:-}"
	: >"$FAKELOG"
	env $extra \
		HOME="$HOME_DIR" XDG_RUNTIME_DIR="$RUNDIR/run" \
		PATH="$FAKE:$PATH" FAKE_LOG="$FAKELOG" FAKE_ENV="$FAKE_ENV" \
		WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
		VIDOW_LOG=debug \
		./build/vidow >"$LOG/$name.log" 2>&1 &
	VIDOW_PID=$!

	local i
	for i in $(seq 1 60); do
		grep -aq "vidow ready" "$LOG/$name.log" && break
		sleep 0.1
	done
	if ! grep -aq "vidow ready" "$LOG/$name.log"; then
		echo "  FAIL  vidow 起不来（$name）"
		sed 's/^/        /' "$LOG/$name.log" | tail -5
		FAILED=1
		return 1
	fi
	settle
	cp "$FAKELOG" "$LOG/$name.start"
	return 0
}

# 退出（SIGTERM → 正常退出路径 → 还原），等异步的 systemctl 落完盘。
stop_round() {
	local name="$1"
	kill -TERM "$VIDOW_PID" 2>/dev/null
	wait "$VIDOW_PID" 2>/dev/null
	VIDOW_PID=""
	settle
	tail -n "+$(( $(wc -l <"$LOG/$name.start") + 1 ))" "$FAKELOG" >"$LOG/$name.exit"
}

# 等日志不再长（还原是 fork 出去异步跑的，不能光靠 sleep 猜）。
settle() {
	local i last="" now
	for i in $(seq 1 20); do
		now=$(wc -l <"$FAKELOG" 2>/dev/null || echo 0)
		[ "$now" = "$last" ] && return 0
		last="$now"
		sleep 0.1
	done
}

has() { grep -aqF -- "$2" "$1"; }

# ---------------------------------------------------------------- A 环境齐全
mark "A 环境齐全 + 首次（要写 portals.conf）"
cat >"$FAKE_ENV" <<'EOF'
XDG_CURRENT_DESKTOP=Hyprland
WAYLAND_DISPLAY=wayland-1
XDG_RUNTIME_DIR=/run/user/1000
SOME_OTHER=1
EOF
run_round A || exit 1
stop_round A
cp "$HOME_DIR/.config/xdg-desktop-portal/vidow-portals.conf" "$LOG/A.conf"

if has "$LOG/A.start" "--user show-environment"; then
	pass "启动前问了 show-environment（记快照）"
else
	fail "启动前没记快照（用户管理器里原来是什么就不知道了）"
fi
if has "$LOG/A.start" "import-environment XDG_CURRENT_DESKTOP WAYLAND_DISPLAY XDG_RUNTIME_DIR"; then
	pass "启动 import 了三个变量"
else
	fail "启动没 import 环境 —— 后端会连到宿主合成器上"
fi
if has "$LOG/A.start" "restart xdg-desktop-portal"; then
	pass "启动重启了前端（portals.conf 变了）"
else
	fail "配置变了却没重启前端"
fi
if has "$LOG/A.exit" "set-environment XDG_CURRENT_DESKTOP=Hyprland WAYLAND_DISPLAY=wayland-1 XDG_RUNTIME_DIR=/run/user/1000"; then
	pass "退出把三个值原样 set 回用户管理器"
else
	fail "退出没还原环境：$(head -1 "$LOG/A.exit" 2>/dev/null)"
fi
RESTARTS=$(grep -ac "restart xdg-desktop-portal" "$FAKELOG")
if [ "$RESTARTS" = 2 ]; then
	pass "重启了 2 次前端（启动 1 次 + 退出 1 次）"
else
	fail "重启次数 $RESTARTS，期望 2"
fi
IMPORT_LINE=$(grep -an -- "import-environment" "$FAKELOG" | head -1 | cut -d: -f1)
SET_LINE=$(grep -an -- "^--user set-environment" "$FAKELOG" | head -1 | cut -d: -f1)
if [ -n "$IMPORT_LINE" ] && [ -n "$SET_LINE" ] && [ "$SET_LINE" -gt "$IMPORT_LINE" ]; then
	pass "还原排在 import 之后（$IMPORT_LINE → $SET_LINE）"
else
	fail "还原顺序不对（import=$IMPORT_LINE set=$SET_LINE）"
fi
if has "$LOG/A.log" "portal: 环境快照" && has "$LOG/A.log" "portal: 还原用户管理器环境"; then
	pass "vidow 日志里有快照 / 还原两条"
else
	fail "vidow 日志缺快照或还原记录"
fi
if has "$FAKELOG" "unset-environment"; then
	fail "三个值原来都在，不该 unset"
else
	pass "值都在时不 unset"
fi

# ------------------------------------------------------------ B 配置没变
mark "B 同一份配置（portals.conf 内容没变）"
run_round B || exit 1
stop_round B

if has "$LOG/B.start" "restart xdg-desktop-portal"; then
	fail "配置没变还重启了前端（会打断正在进行的 portal 请求）"
else
	pass "配置没变 → 启动不重启前端"
fi
if has "$LOG/B.start" "import-environment"; then
	pass "配置没变也 import 环境"
else
	fail "配置没变就不 import —— 用户管理器里留的还是上一个合成器的值"
fi
if has "$LOG/B.log" "内容没变，只 import 环境"; then
	pass "日志写明「只 import 环境」"
else
	fail "日志没说清这一轮干了什么"
fi
if has "$LOG/B.exit" "set-environment XDG_CURRENT_DESKTOP=Hyprland"; then
	pass "第二轮退出照样还原"
else
	fail "第二轮退出没还原"
fi

# -------------------------------------------------- C 原来就没有那两个变量
mark "C 用户管理器里本来没有 DESKTOP / WAYLAND_DISPLAY"
cat >"$FAKE_ENV" <<'EOF'
XDG_RUNTIME_DIR=/run/user/1000
EOF
run_round C || exit 1
stop_round C

if has "$LOG/C.exit" "unset-environment XDG_CURRENT_DESKTOP WAYLAND_DISPLAY"; then
	pass "退出 unset 那两个（原来就没有，别留个空的）"
else
	fail "该 unset 没 unset：$(head -1 "$LOG/C.exit" 2>/dev/null)"
fi
C_SET=$(grep -a -- "^--user set-environment" "$LOG/C.exit" | head -1)
if [ "$C_SET" = "--user set-environment XDG_RUNTIME_DIR=/run/user/1000" ]; then
	pass "退出只 set XDG_RUNTIME_DIR"
else
	fail "set 的内容不对：$C_SET"
fi

# ------------------------------------------------- D 不碰用户管理器（测试用）
mark "D VIDOW_PORTAL_NO_RESTART=1"
run_round D VIDOW_PORTAL_NO_RESTART=1 || exit 1
stop_round D

if [ ! -s "$FAKELOG" ]; then
	pass "一次 systemctl 都没调"
else
	fail "说好不碰用户管理器，却调了：$(head -3 "$FAKELOG" | tr '\n' '|')"
fi
if has "$LOG/D.log" "VIDOW_PORTAL_NO_RESTART 已设"; then
	pass "日志写明跳过"
else
	fail "日志没说明为什么不碰"
fi

# ------------------------------------------- E 录屏 / 截图该给哪个后端
# GTK 没有 ScreenCast / Screenshot 这两个接口，KDE 的实现走 KWin 私有协议
# （zkde_screencast_unstable_v1），在 vidow 上跑不通。能干活的只有 wlr 和
# hyprland 两个后端 —— 它们都走 zwlr_screencopy_manager_v1，而 vidow 自己实现了
# 这个协议（src/screencopy.c）。所以按「装了哪个用哪个」写进去；一个都没装就不写，
# 免得客户端以为支持、一调用就失败。
mark "E 录屏 / 截图后端"
DETECT=""
for c in wlr hyprland; do
	for d in $(echo "${XDG_DATA_DIRS:-/usr/local/share:/usr/share}" | tr ':' ' '); do
		if [ -f "$d/xdg-desktop-portal/portals/$c.portal" ]; then
			DETECT="$c"
			break 2
		fi
	done
done

if [ -n "$DETECT" ]; then
	if grep -aqx "org.freedesktop.impl.portal.ScreenCast=$DETECT" "$LOG/A.conf" \
			&& grep -aqx "org.freedesktop.impl.portal.Screenshot=$DETECT" "$LOG/A.conf"; then
		pass "ScreenCast / Screenshot 指向装了的那一个后端（$DETECT）"
	else
		fail "生成的 portals.conf 里录屏后端不对（本机装的是 $DETECT）：$(grep -a ScreenCast "$LOG/A.conf" | head -1)"
	fi
else
	fail "本机没装 wlr / hyprland 后端，这一节没得测"
fi

mkdir -p "$RUNDIR/nodata"
run_round E XDG_DATA_DIRS="$RUNDIR/nodata" || exit 1
stop_round E
if grep -aq "ScreenCast\|Screenshot" "$HOME_DIR/.config/xdg-desktop-portal/vidow-portals.conf"; then
	fail "一个录屏后端都没装却还是写了：$(grep -a "ScreenCast\|Screenshot" "$HOME_DIR/.config/xdg-desktop-portal/vidow-portals.conf" | head -1)"
else
	pass "一个录屏后端都没装 → 一个字都不写"
fi
if has "$LOG/E.log" "ScreenCast / Screenshot 不写"; then
	pass "日志说明白为什么不写"
else
	fail "日志没说明为什么不写"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "== test-portalenv: 全部通过 =="
else
	echo "== test-portalenv: 有失败 =="
fi
exit "$FAILED"
