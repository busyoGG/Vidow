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
mkdir -p "$HOME_DIR/.config/vidow" "$LOG" "$FAKE" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

# 只改 portal_capture 的一份配置（空 = 没有配置文件）
set_capture() {
	if [ -z "${1:-}" ]; then
		rm -f "$HOME_DIR/.config/vidow/config.lua"
		return 0
	fi
	printf 'portal_capture = "%s"\n' "$1" >"$HOME_DIR/.config/vidow/config.lua"
}

# 把这一轮生成的 portals.conf / vidow.portal 留一份给断言用
snap_conf() {
	cp "$HOME_DIR/.config/xdg-desktop-portal/vidow-portals.conf" "$LOG/$1.conf" 2>/dev/null
	cp "$HOME_DIR/.local/share/xdg-desktop-portal/portals/vidow.portal" "$LOG/$1.portal" 2>/dev/null
}

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
"--user show -p MainPID --value xdg-desktop-portal.service")
	# 「前端现在跑着吗、是哪个进程」。FAKE_MAINPID 空 = 没跑（0 也当没跑）。
	[ -n "${FAKE_MAINPID:-}" ] || exit 0
	echo "$FAKE_MAINPID"
	exit 0
	;;
esac
exit 0
FAKE_SH
chmod +x "$FAKE/systemctl"

# 会话环境（src/env.c）除了 systemctl 还有两条路能改用户管理器的环境：busctl 和
# dbus-update-activation-environment（都走会话总线，不看 XDG_RUNTIME_DIR）。这里
# 也得假掉 —— 不然这一轮测试会去改**真**用户管理器：推的是测试里那个
# WAYLAND_DISPLAY，退出还原还会把宿主会话本来就有的变量 unset 掉。
cat >"$FAKE/busctl" <<'FAKE_SH'
#!/bin/sh
printf 'busctl %s\n' "$*" >>"${FAKE_LOG:?}"
exit 0
FAKE_SH
cat >"$FAKE/dbus-update-activation-environment" <<'FAKE_SH'
#!/bin/sh
printf 'dbus-update-activation-environment %s\n' "$*" >>"${FAKE_LOG:?}"
exit 0
FAKE_SH
chmod +x "$FAKE/busctl" "$FAKE/dbus-update-activation-environment"

# 起一轮 headless vidow，跑起来后把启动阶段的 systemctl 调用切出来（$1.start）。
# $2 是额外环境变量（比如 VIDOW_PORTAL_NO_RESTART=1）。
run_round() {
	local name="$1" extra="${2:-}"
	: >"$FAKELOG"
	env $extra \
		HOME="$HOME_DIR" XDG_RUNTIME_DIR="$RUNDIR/run" \
		PATH="$FAKE:$PATH" FAKE_LOG="$FAKELOG" FAKE_ENV="$FAKE_ENV" \
		FAKE_MAINPID="${FAKE_MAINPID:-}" \
		WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
		VIDOW_LOG=debug \
		"${VIDOW_BIN:-./build/vidow}" >"$LOG/$name.log" 2>&1 &
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
cp "$HOME_DIR/.local/share/xdg-desktop-portal/portals/vidow.portal" "$LOG/A.portal" 2>/dev/null

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
# 只看**我们那三个**：vidow 的会话环境导出（见 src/env.c）退出时会把「我们推上去、
# 用户管理器里本来没有的」变量（VIDOW_SOCKET 这类）unset 掉 —— 那是它自己的账，不是
# 这一条要管的。这里要盯的是：三个值原来都在，就一个都不许 unset。
if grep -a -- "--user unset-environment" "$FAKELOG" 2>/dev/null |
	grep -qE "XDG_CURRENT_DESKTOP|WAYLAND_DISPLAY|XDG_RUNTIME_DIR"; then
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
# 默认 auto：vidow-portal 就在 vidow 二进制边上 → 用它（自己弹选择器 + 实时预览，
# 不依赖任何外部后端、也不碰任何环境变量）。它靠两个文件让前端找到自己：
#   $XDG_DATA_HOME/xdg-desktop-portal/portals/vidow.portal（总线名 + 接口清单）
#   <会话名>-portals.conf 里的 ScreenCast=vidow
# portal_capture 写死优先；写 none 就一个字都不写；自己那个不在边上时才退回探测
# 装了哪个外部后端（wlr > hyprland）。
mark "E auto 优先自己人"
if grep -aqx "org.freedesktop.impl.portal.ScreenCast=vidow" "$LOG/A.conf" &&
		grep -aqx "org.freedesktop.impl.portal.Screenshot=vidow" "$LOG/A.conf"; then
	pass "auto：ScreenCast / Screenshot 指向 vidow 自己的后端"
else
	fail "auto 没指向自己人：$(grep -a ScreenCast "$LOG/A.conf" 2>/dev/null | head -1)"
fi
if grep -aq "DBusName=org.freedesktop.impl.portal.desktop.vidow" "$LOG/A.portal" 2>/dev/null &&
		grep -aq "Interfaces=.*ScreenCast" "$LOG/A.portal"; then
	pass "写了 vidow.portal（前端按它找后端）"
else
	fail "vidow.portal 没写对：$(cat "$LOG/A.portal" 2>/dev/null | tr '\n' ' ')"
fi
if grep -aqx "UseIn=Vidow;" "$LOG/A.portal" 2>/dev/null; then
	pass "vidow.portal 的 UseIn 跟会话名一致"
else
	fail "UseIn 不对：$(grep -a UseIn "$LOG/A.portal" 2>/dev/null)"
fi

# ------------------------------------------------- F 前端环境不对就重启它
# 前端的 portals.conf 是**进程起来那一刻**读的，读一次就缓存住。配置内容没变、
# 但正在跑的那个前端还是宿主的环境（从宿主合成器切过来就是这样），不重启它就会
# 按宿主那份 <host>-portals.conf 选后端 —— 录屏会被指到 hyprland 上去。
mark "F 前端跑着但环境不是我们的 → 重启它"
env -u XDG_CURRENT_DESKTOP -u WAYLAND_DISPLAY sleep 60 &
FRONT_PID=$!
FAKE_MAINPID=$FRONT_PID
run_round F || exit 1
stop_round F
if has "$LOG/F.start" "show -p MainPID"; then
	pass "查了前端的 MainPID（不是瞎猜）"
else
	fail "没查前端进程"
fi
if has "$LOG/F.start" "restart xdg-desktop-portal"; then
	pass "环境不对 → 重启前端"
else
	fail "前端环境不对却没重启（录屏会指到宿主那个后端上）"
fi

mark "F2 前端没在跑 → 不重启"
FAKE_MAINPID=0
run_round F2 || exit 1
stop_round F2
if has "$LOG/F2.start" "restart xdg-desktop-portal"; then
	fail "前端没跑还去重启（白起一个）"
else
	pass "前端没跑 → 不重启（下次被激活时环境就是新的）"
fi
FAKE_MAINPID=""
kill "$FRONT_PID" 2>/dev/null

mark "E2 没有 vidow-portal 时退回探测外部后端"
mkdir -p "$RUNDIR/bin"
cp ./build/vidow "$RUNDIR/bin/vidow"   # 边上没有 vidow-portal
DETECT=""
for c in wlr hyprland; do
	for d in $(echo "${XDG_DATA_DIRS:-/usr/local/share:/usr/share}" | tr ':' ' '); do
		if [ -f "$d/xdg-desktop-portal/portals/$c.portal" ]; then
			DETECT="$c"
			break 2
		fi
	done
done
VIDOW_BIN="$RUNDIR/bin/vidow" run_round E2 || exit 1
stop_round E2
snap_conf E2
if [ -n "$DETECT" ]; then
	if grep -aqx "org.freedesktop.impl.portal.ScreenCast=$DETECT" "$LOG/E2.conf" &&
			grep -aqx "org.freedesktop.impl.portal.Screenshot=$DETECT" "$LOG/E2.conf"; then
		pass "没有自己人时指向装了的外部后端（$DETECT）"
	else
		fail "退回探测不对（本机装的是 $DETECT）：$(grep -a ScreenCast "$LOG/E2.conf" 2>/dev/null | head -1)"
	fi
else
	fail "本机没装 wlr / hyprland 后端，这一条没得测"
fi
if [ -e "$HOME_DIR/.local/share/xdg-desktop-portal/portals/vidow.portal" ]; then
	fail "不用自己人了却没把 vidow.portal 删掉"
else
	pass "不用自己人了 → vidow.portal 删掉"
fi

mark "E3 portal_capture 写死 / none"
set_capture wlr
run_round E3 || exit 1
stop_round E3
snap_conf E3
if grep -aqx "org.freedesktop.impl.portal.ScreenCast=wlr" "$LOG/E3.conf"; then
	pass "portal_capture=wlr 写死了就是 wlr（配置优先于 auto）"
else
	fail "写死 wlr 没用：$(grep -a ScreenCast "$LOG/E3.conf" 2>/dev/null | head -1)"
fi
set_capture none
run_round E4 || exit 1
stop_round E4
snap_conf E4
if grep -aq "ScreenCast\|Screenshot" "$LOG/E4.conf"; then
	fail "portal_capture=none 却还是写了：$(grep -a "ScreenCast\|Screenshot" "$LOG/E4.conf" | head -1)"
else
	pass "portal_capture=none → 一个字都不写"
fi
set_capture ""

mark "E5 自己人和外部后端都没有"
mkdir -p "$RUNDIR/nodata"
VIDOW_BIN="$RUNDIR/bin/vidow" run_round E5 XDG_DATA_DIRS="$RUNDIR/nodata" || exit 1
stop_round E5
snap_conf E5
if grep -aq "ScreenCast\|Screenshot" "$LOG/E5.conf"; then
	fail "一个后端都没有却还是写了：$(grep -a "ScreenCast\|Screenshot" "$LOG/E5.conf" | head -1)"
else
	pass "一个后端都没有 → 一个字都不写"
fi
if has "$LOG/E5.log" "ScreenCast / Screenshot 不写"; then
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
