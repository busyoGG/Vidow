#!/bin/bash
# vidow 自己的录屏 portal（tools/vidowportal.c）在 vidow 上跑通 —— 且**不依赖 KDE**。
#
# 这条链和 test-screencast.sh（KDE 的后端）的区别就在「后端是谁」：
#   test-screencast.sh   xdg-desktop-portal-kde + XDG_CURRENT_DESKTOP=KDE（骗整个会话）
#   这个                 vidow-portal + XDG_CURRENT_DESKTOP=Vidow（自己的后端，谁都不骗）
#
# vidow 启动时干三件事：写 ~/.local/share/xdg-desktop-portal/portals/vidow.portal
# （前端按它知道有个叫 org.freedesktop.impl.portal.desktop.vidow 的后端）、把
# <会话名>-portals.conf 里的 ScreenCast / Screenshot 指向 vidow、把 vidow-portal
# 起起来占住那个总线名。然后前端（xdg-desktop-portal）就只做转发，对话框和流都是
# 自己人：对话框是 GTK3（带每块屏的实时预览），流由合成器建（zkde_screencast）。
#
# 判据：
#   1) vidow.portal 写对了（总线名 + ScreenCast 接口），portals.conf 里 ScreenCast=vidow
#   2) vidow 起了 vidow-portal，它占住了那个总线名并导出 ScreenCast
#   3) 会话名是 Vidow（不是 KDE）—— 没骗任何应用，也没写 systemd drop-in
#   4) 前端在这个会话里选的就是 vidow 这个后端
#   5) CreateSession / SelectSources / Start 全 code=0，拿到 1 路流
#   6) 选择器对话框真弹出来了（vidow 的窗口列表里看得到），点「共享」就出流
#   7) 流尺寸 = 输出像素尺寸；gst 消费者拿到帧、帧跟着画面变、静止时靠心跳也有帧
#   8) 全程 vidow 没崩
#
# 需要：pipewire 在跑、gst-launch-1.0 + gst-plugin-pipewire、python3-gobject、
#       gtk3、wayland-info、build/vidowmsg、build/vidow-portal。
#
# 用法：./test-portal-own.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-portal-own-test
HOME_DIR="$RUNDIR/home"
LOG="$RUNDIR/log"
HELPER_LOG=/run/user/1000/vidow-portal.log
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $(pgrep -x vidow 2>/dev/null); do kill "$p" 2>/dev/null; done
	# vidow-portal 挂在私有总线上（dbus-run-session 结束不一定带走），按总线地址认。
	for p in $(pgrep -x vidow-portal 2>/dev/null); do
		a=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
			grep -m1 '^DBUS_SESSION_BUS_ADDRESS=')
		case "$a" in
		""|"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus") ;;
		*) kill "$p" 2>/dev/null ;;
		esac
	done
	for p in $(pgrep -f "^/usr/lib/xdg-desktop-portal" 2>/dev/null); do
		a=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
			grep -m1 '^DBUS_SESSION_BUS_ADDRESS=')
		case "$a" in
		""|"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus") ;;
		*) kill "$p" 2>/dev/null ;;
		esac
	done
	rm -f "$HELPER_LOG"
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

for tool in gst-launch-1.0 python3 wayland-info; do
	command -v "$tool" >/dev/null || { echo "test-portal-own.sh: 需要 $tool"; exit 1; }
done
pgrep -x pipewire >/dev/null || { echo "test-portal-own.sh: 需要 pipewire 在跑"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-portal-own.sh: 需要 ./build/vidowmsg"; exit 1; }
[ -x ./build/vidow-portal ] || { echo "test-portal-own.sh: 需要 ./build/vidow-portal"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$HOME_DIR/.config" "$LOG"

# 一条私有 dbus 会话里跑全套：vidow（它自己会把 vidow-portal 起在同一条总线上）、
# 前端、真请求、真消费。不手动起任何 impl 后端 —— 这就是这个测试要证明的事。
cat >"$LOG/inner.sh" <<'INNER'
set -u
R="$1"
VM=/home/busyo/Dev/Vidow/build/vidowmsg
B=/home/busyo/Dev/Vidow/build
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
FAILED=0

export HOME="$R/home" XDG_CONFIG_HOME="$R/home/.config"
export XDG_DATA_HOME="$R/home/.local/share"
export XDG_CURRENT_DESKTOP=Vidow
export XDG_RUNTIME_DIR=/run/user/1000
export VIDOW_PORTAL_NO_RESTART=1
export VIDOW_PORTAL_LOG="$R/helper.log"

WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman VIDOW_LOG=info \
	"$B/vidow" >"$R/vidow.log" 2>&1 &
VPID=$!
for i in $(seq 1 60); do
	grep -aq "vidow ready" "$R/vidow.log" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$R/vidow.log"; then
	fail "vidow 起不来"
	sed 's/^/        /' "$R/vidow.log" | tail -5
	exit 1
fi
SOCK=$(grep -a "vidow ready" "$R/vidow.log" | grep -ao "wayland-[0-9]*" | head -1)
IPC="/run/user/1000/vidow-$SOCK.sock"
export WAYLAND_DISPLAY="$SOCK"

# --- 1 文件落地 ------------------------------------------------------------
PF="$XDG_DATA_HOME/xdg-desktop-portal/portals/vidow.portal"
if grep -q "DBusName=org.freedesktop.impl.portal.desktop.vidow" "$PF" 2>/dev/null &&
	grep -q "org.freedesktop.impl.portal.ScreenCast" "$PF"; then
	pass "vidow 写了 vidow.portal（$(grep -c . "$PF") 行）"
else
	fail "vidow.portal 没写对：$(cat "$PF" 2>/dev/null | tr '\n' ' ')"
fi
CF="$XDG_CONFIG_HOME/xdg-desktop-portal/vidow-portals.conf"
if grep -qx "org.freedesktop.impl.portal.ScreenCast=vidow" "$CF" 2>/dev/null; then
	pass "portals.conf 里 ScreenCast=vidow（会话名还是 Vidow）"
else
	fail "portals.conf 里 ScreenCast 不是 vidow：$(grep -i screencast "$CF" 2>/dev/null | tr '\n' ' ')"
fi
if [ -e "$XDG_CONFIG_HOME/systemd/user/plasma-xdg-desktop-portal-kde.service.d/vidow.conf" ]; then
	fail "居然写了 KDE 的 systemd drop-in —— 那就不叫「不碰别的」了"
else
	pass "没写任何 systemd drop-in"
fi

# --- 2 vidow 起了自己的后端 -------------------------------------------------
for i in $(seq 1 40); do
	pgrep -x vidow-portal >/dev/null && break
	sleep 0.25
done
if pgrep -x vidow-portal >/dev/null; then
	pass "vidow 把 vidow-portal 起起来了"
else
	fail "vidow-portal 没起来（日志：$(tail -2 "$R/helper.log" 2>/dev/null | tr '\n' ' ')）"
fi
if grep -aq "连上合成器" "$R/helper.log" 2>/dev/null &&
	grep -aq "已占住 org.freedesktop.impl.portal.desktop.vidow" "$R/helper.log"; then
	pass "vidow-portal 连上了合成器（$(grep -a '连上合成器' "$R/helper.log" | head -1 | sed 's/.*：//')）并占了总线名"
else
	fail "vidow-portal 日志不对：$(head -3 "$R/helper.log" 2>/dev/null | tr '\n' ' ')"
fi

# --- 3/4 前端在这个会话里选它 ----------------------------------------------
dbus-update-activation-environment --all
/usr/lib/xdg-desktop-portal >"$R/xdp.log" 2>&1 &
for i in $(seq 1 20); do
	gdbus introspect --session --dest org.freedesktop.impl.portal.desktop.vidow \
		--object-path /org/freedesktop/portal/desktop >/dev/null 2>&1 && break
	sleep 0.5
done
if gdbus introspect --session --dest org.freedesktop.impl.portal.desktop.vidow \
	--object-path /org/freedesktop/portal/desktop 2>/dev/null |
	grep -q "interface org.freedesktop.impl.portal.ScreenCast"; then
	pass "vidow 自己的后端导出 ScreenCast（前端认得它）"
else
	fail "总线上没有 vidow 后端的 ScreenCast 接口"
fi
if gdbus introspect --session --dest org.freedesktop.portal.Desktop \
	--object-path /org/freedesktop/portal/desktop 2>/dev/null |
	grep -q "interface org.freedesktop.portal.ScreenCast"; then
	pass "前端对客户端暴露了 ScreenCast"
else
	fail "前端没暴露 ScreenCast"
fi

# --- 5/6 真请求：CreateSession → SelectSources → Start ---------------------
timeout 150 python3 /home/busyo/Dev/Vidow/tools/screencastprobe.py \
	--types 1 --timeout 60 --hold 45 >"$R/probe.txt" 2>&1 &

# Start 会弹选择器。等它出现，然后点右下角的「共享」。
GEO=""
for i in $(seq 1 40); do
	sleep 1
	GEO=$($VM --socket "$IPC" windows 2>/dev/null | python3 -c '
import json,sys
try:
    ws=json.load(sys.stdin)
except Exception:
    sys.exit(0)
for w in ws:
    if "共享屏幕" in (w.get("title") or ""):
        g=w["geometry"]
        print("%d %d %d %d" % (g["x"], g["y"], g["width"], g["height"]))
        break
')
	[ -n "$GEO" ] && break
done
if [ -n "$GEO" ]; then
	set -- $GEO
	pass "选择器对话框弹出来了（${1},${2} ${3}x${4}）"
	# 右下角那个按钮：窗口 12px 边距 + 按钮大概 80x34，取右下角往内一点。
	CX=$(( $1 + $3 - 50 ))
	CY=$(( $2 + $4 - 30 ))
	sleep 1
	$VM --socket "$IPC" move-cursor $CX $CY >/dev/null 2>&1
	sleep 0.3
	$VM --socket "$IPC" pointer-button click >/dev/null 2>&1
else
	fail "没等到选择器对话框（helper 日志：$(tail -3 "$R/helper.log" 2>/dev/null | tr '\n' ' ')）"
fi

for i in $(seq 1 40); do
	grep -aq "node=" "$R/probe.txt" 2>/dev/null && break
	sleep 1
done
NODE=$(grep -ao "node=[0-9]*" "$R/probe.txt" 2>/dev/null | head -1 | cut -d= -f2)
if [ -n "${NODE:-}" ]; then
	pass "Start 返回了 PipeWire node（node=$NODE）"
else
	fail "Start 没返回 node：$(tail -3 "$R/probe.txt" 2>/dev/null | tr '\n' ' ')"
fi
SIZE=$(grep -ao "size': ([0-9]*, [0-9]*)" "$R/probe.txt" 2>/dev/null | head -1 | grep -o "[0-9]*, [0-9]*")
if [ "${SIZE:-}" = "1280, 720" ]; then
	pass "流尺寸 = 输出的像素尺寸（$SIZE）"
else
	fail "流尺寸不对：${SIZE:-没拿到}（期望 1280, 720）"
fi

# --- 7 真消费 ---------------------------------------------------------------
if [ -n "${NODE:-}" ]; then
	timeout 60 gst-launch-1.0 -q pipewiresrc path="$NODE" num-buffers=6 \
		! videoconvert ! pngenc ! multifilesink location="$R/f-%02d.png" \
		>"$R/gst.log" 2>&1 &
	GST=$!
	sleep 1
	XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY="$SOCK" \
		python3 /home/busyo/Dev/Vidow/tools/bluewin.py >/dev/null 2>&1 &
	for i in $(seq 1 12); do
		$VM --socket "$IPC" toggle-overview >/dev/null 2>&1
		sleep 0.7
		kill -0 $GST 2>/dev/null || break
	done
	wait $GST
	NF=$(ls "$R"/f-*.png 2>/dev/null | wc -l)
	ND=$(md5sum "$R"/f-*.png 2>/dev/null | awk '{print $1}' | sort -u | wc -l)
	if [ "$NF" -ge 5 ]; then
		pass "消费者从 node 拿到了 $NF 帧"
	else
		fail "只拿到 $NF 帧（期望 ≥5）：$(head -2 "$R/gst.log" 2>/dev/null | tr '\n' ' ')"
	fi
	if [ "$ND" -ge 2 ]; then
		pass "屏幕变了帧跟着变（$NF 帧里有 $ND 种不同画面）"
	else
		fail "帧没变（$NF 帧全一样）"
	fi
	timeout 30 gst-launch-1.0 -q pipewiresrc path="$NODE" num-buffers=2 \
		! videoconvert ! pngenc ! multifilesink location="$R/s-%02d.png" \
		>"$R/gst2.log" 2>&1
	NS=$(ls "$R"/s-*.png 2>/dev/null | wc -l)
	if [ "$NS" -ge 2 ]; then
		pass "画面静止时消费者照样拿到帧（$NS 帧，靠 1Hz 心跳）"
	else
		fail "静止画面上只拿到 $NS 帧"
	fi
else
	fail "没有 node，消费那几条测不了"
fi

# --- 9 KDE 后端在这条私有总线上根本没被拉起来 ------------------------------
# 怎么证明「这次录屏不是 KDE 后端干的」：
#   - KDE 后端要能注册 ScreenCast，进程环境里必须有 XDG_CURRENT_DESKTOP=KDE
#     （desktopportal.cpp 整串比较），而这条链里它从来没出现过；
#   - CreateSession / SelectSources / Start 三个请求都是我们自己那个进程收的
#     （它日志里记着），流也是它跟合成器要的。
if grep -aq "Start 成功" "$R/helper.log" 2>/dev/null; then
	pass "三个请求都是 vidow-portal 自己收的（KDE 后端没资格注册 ScreenCast）"
else
	fail "helper 日志里没有「Start 成功」，说不清是谁处理的"
fi

# --- 8 vidow 活着 ----------------------------------------------------------
if grep -aqiE "Assertion|Aborted|core dump" "$R/vidow.log"; then
	fail "vidow 日志里有断言 / abort"
else
	pass "vidow 没崩"
fi

kill $VPID 2>/dev/null
exit "$FAILED"
INNER

mark "1~8 私有 dbus 会话里跑真链路（vidow 自己的后端）"
timeout 400 dbus-run-session -- bash "$LOG/inner.sh" "$RUNDIR" >"$LOG/inner.out" 2>&1
RC=$?
sed -n 's/^\(  \(PASS\|FAIL\).*\)$/\1/p' "$LOG/inner.out"
if [ "$RC" != 0 ] && ! grep -aq "^  FAIL" "$LOG/inner.out"; then
	fail "内层脚本没跑完（rc=$RC）：$(tail -3 "$LOG/inner.out" | tr '\n' ' ')"
fi

if grep -aq "XDG_CURRENT_DESKTOP=KDE" "$LOG/inner.out" 2>/dev/null; then
	fail "环境里出现了 XDG_CURRENT_DESKTOP=KDE"
else
	pass "会话名一直是 Vidow（没骗任何应用）"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "== test-portal-own: 全部通过 =="
else
	echo "== test-portal-own: 有失败（完整输出：$LOG/inner.out）=="
fi
exit "$FAILED"
