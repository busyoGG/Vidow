#!/bin/bash
# KDE 的录屏 portal（xdg-desktop-portal-kde 的 ScreenCast）在 vidow 上跑通。
#
# 两件事缺一不可，这个脚本两件都测：
#
#   1. **vidow 提供 zkde_screencast_unstable_v1**（KWin 私有协议，见 src/screencast.c）。
#      KDE 的 ScreenCast 后端建在它上面：绑不到这个 global，它连
#      org.freedesktop.impl.portal.ScreenCast 接口都不注册 —— 所以非 KWin 的合成器上
#      「KDE 的录屏」根本不存在。这个协议还要求**合成器自己建 PipeWire 流**，所以
#      vidow 那边同时是个 PipeWire 生产者。
#
#   2. **XDG_CURRENT_DESKTOP=KDE**。xdg-desktop-portal-kde 的 DesktopPortal 构造函数里
#      写死了（src/desktopportal.cpp）：
#          if (qgetenv("XDG_CURRENT_DESKTOP").compare("KDE", Qt::CaseInsensitive) == 0) {
#              ... ScreenCastPortal / ScreenshotPortal / RemoteDesktopPortal ...
#          }
#      是**整串相等**，不是「包含」：`KDE:Vidow`、`Vidow:KDE` 都不行（实测过）。
#      所以在 vidow 里要用 KDE 那套 portal，配置得写 portal_desktop = "KDE"。
#
# 判据：
#   1) vidow 暴露 zkde_screencast_unstable_v1（版本 6）
#   2) 会话名 KDE 时 KDE 后端导出 ScreenCast 接口
#   3) 会话名 Vidow 时它不导出（对照组 —— 说明第 2 条不是碰巧）
#   4) CreateSession / SelectSources / Start 全 code=0，拿到 1 路流
#   5) 流的尺寸就是输出的像素尺寸
#   6) 真连上那个 PipeWire node 能拿到帧（gst 消费者，≥5 帧）
#   7) 屏幕变了，帧跟着变（不同 md5 的帧 ≥2）
#   8) 画面静止时消费者照样拿到帧（1Hz 心跳，不是只在损伤时推）
#   9) 全程 vidow 没崩
#
# 需要：pipewire 在跑、gst-launch-1.0 + gst-plugin-pipewire、python3-gobject、
#       xdg-desktop-portal-kde、wayland-info、grim 之外还要 vidowmsg（build/vidowmsg）。
#
# 用法：./test-screencast.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-screencast-test
HOME_DIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $(pgrep -x vidow 2>/dev/null); do kill "$p" 2>/dev/null; done
	for p in $(pgrep -f bluewin.py 2>/dev/null); do kill "$p" 2>/dev/null; done
	# 私有总线上的 portal 进程：dbus-run-session 结束时不一定跟着走，留着会占着
	# 后面的测试（判据是「环境里的 bus 地址不是真实会话那条」）。
	for p in $(pgrep -f "^/usr/lib/xdg-desktop-portal" 2>/dev/null); do
		a=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
			grep -m1 '^DBUS_SESSION_BUS_ADDRESS=')
		case "$a" in
		""|"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus") ;;
		*) kill "$p" 2>/dev/null ;;
		esac
	done
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

for tool in gst-launch-1.0 python3 wayland-info; do
	command -v "$tool" >/dev/null || { echo "test-screencast.sh: 需要 $tool"; exit 1; }
done
pgrep -x pipewire >/dev/null || { echo "test-screencast.sh: 需要 pipewire 在跑"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-screencast.sh: 需要 ./build/vidowmsg"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$HOME_DIR/.config/vidow" "$LOG"

# 录屏指给 KDE 后端（vidow 的探测默认给 wlr/hyprland，那两个不是「KDE 的录屏」）
cat >"$HOME_DIR/.config/vidow/portals.conf" <<'EOF'
[preferred]
org.freedesktop.impl.portal.ScreenCast=kde
EOF

# 渲染器：默认 pixman（不需要 GPU，哪儿都能跑）。VIDOW_TEST_RENDERER=gles2 走真
# GPU —— **只有那条路上才有 dmabuf**（录屏直通那条路：抓帧缓冲直接当 dmabuf 交给
# 消费者，一帧不落 CPU），所以直通那几条断言也只在那种模式下跑（见内层第 9 节）。
RENDERER=${VIDOW_TEST_RENDERER:-pixman}
RENDER_ENVS=(WLR_RENDERER="$RENDERER")
if [ "$RENDERER" = gles2 ]; then
	RENDER_ENVS+=(WLR_RENDER_DRM_DEVICE=${VIDOW_TEST_DRM_DEVICE:-/dev/dri/renderD128})
fi

# 真 XDG_RUNTIME_DIR：portal 要连用户会话里的 PipeWire，换到临时目录就连不上了。
XDG_RUNTIME_DIR=/run/user/1000 HOME="$HOME_DIR" \
env WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 "${RENDER_ENVS[@]}" \
	VIDOW_LOG=info VIDOW_PORTAL_NO_RESTART=1 \
	./build/vidow >"$LOG/vidow.log" 2>&1 &
VIDOW_PID=$!
for i in $(seq 1 40); do
	grep -aq "vidow ready" "$LOG/vidow.log" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG/vidow.log"; then
	echo "test-screencast.sh: vidow 起不来"; exit 1
fi
SOCK=$(grep -a "vidow ready" "$LOG/vidow.log" | grep -ao "wayland-[0-9]*" | head -1)
IPC="/run/user/1000/vidow-$SOCK.sock"
# portal 读的是 <会话名>-portals.conf；会话名要写 KDE，所以得有一份 kde-portals.conf
cp "$HOME_DIR/.config/xdg-desktop-portal/vidow-portals.conf" \
	"$HOME_DIR/.config/xdg-desktop-portal/kde-portals.conf" 2>/dev/null

mark "1 vidow 暴露了 zkde_screencast_unstable_v1"
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY="$SOCK" wayland-info >"$LOG/wi.txt" 2>&1
if grep -aq "zkde_screencast_unstable_v1" "$LOG/wi.txt"; then
	V=$(grep -a "zkde_screencast_unstable_v1" "$LOG/wi.txt" | grep -ao "version: *[0-9]*" | grep -o "[0-9]*")
	pass "协议在（版本 $V）"
else
	fail "协议不在 —— KDE 的录屏不可能工作"
fi

# 对照组单独一条私有总线：跟主链路挤在一条总线上的话，前端的后端代理会串味
# （实测过：对照组里起的那个前端会一直占着 org.freedesktop.portal.Desktop，
# 主链路那边的请求还是打到它身上，拿到的 cursor mode 是另一个后端的）。
cat >"$LOG/control.sh" <<'CONTROL'
set -u
R="$1"
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; }
export HOME="$R/home" XDG_CONFIG_HOME="$R/home/.config"
export XDG_CURRENT_DESKTOP=Vidow
dbus-update-activation-environment --all
/usr/lib/xdg-desktop-portal-kde >/dev/null 2>&1 &
sleep 3
N_VIDOW=$(gdbus introspect --session --dest org.freedesktop.impl.portal.desktop.kde \
	--object-path /org/freedesktop/portal/desktop 2>/dev/null \
	| grep -c "interface org.freedesktop.impl.portal.ScreenCast")
if [ "$N_VIDOW" = 0 ]; then
	pass "会话名 Vidow 时 KDE 后端不导出 ScreenCast（对照组）"
else
	fail "会话名 Vidow 时它居然导出了 ScreenCast（$N_VIDOW）—— 对照不成立"
fi
CONTROL

# 内层：私有 dbus 里跑前端 + KDE 后端 + 真请求 + 真消费。结果打成 PASS/FAIL 行。
cat >"$LOG/inner.sh" <<'INNER'
set -u
R="$1"; SOCK="$2"; IPC="$3"; RENDERER="$4"
VM=/home/busyo/Dev/Vidow/build/vidowmsg
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; }
export HOME="$R/home" XDG_CONFIG_HOME="$R/home/.config"

# --- 2 会话名 KDE：接口出现 -------------------------------------------------
export XDG_CURRENT_DESKTOP=KDE
dbus-update-activation-environment --all
/usr/lib/xdg-desktop-portal >"$R/xdp.log" 2>&1 &
sleep 3
if gdbus introspect --session --dest org.freedesktop.impl.portal.desktop.kde \
	--object-path /org/freedesktop/portal/desktop 2>/dev/null \
	| grep -q "interface org.freedesktop.impl.portal.ScreenCast"; then
	pass "会话名 KDE 时 KDE 后端导出了 ScreenCast"
else
	fail "会话名 KDE 时还是没有 ScreenCast —— 那这个开关也没用"
fi

# --- 4/5 真请求：CreateSession → SelectSources → Start ----------------------
timeout 150 python3 /home/busyo/Dev/Vidow/tools/screencastprobe.py \
	--types 1 --timeout 60 --hold 45 >"$R/probe.txt" 2>&1 &
# Start 会弹「选择要与应用程序共享的屏幕」—— 点那张「共享这块屏」的卡片。
# 位置按窗口几何算：卡片大致在窗口纵向 18%~45% 处（比例来自实际截图量出来的）。
for i in $(seq 1 30); do
	sleep 1
	$VM --socket "$IPC" windows 2>/dev/null | grep -aq "共享屏幕" && break
done
GEO=$($VM --socket "$IPC" windows 2>/dev/null | python3 -c '
import json,sys
try:
    ws=json.load(sys.stdin)
except Exception:
    sys.exit(0)
for w in ws:
    if "共享屏幕" in (w.get("title") or ""):
        g=w["geometry"]
        print(int(g["x"]+g["width"]/2), int(g["y"]+g["height"]*0.31))
        break
')
if [ -n "${GEO:-}" ]; then
	pass "KDE 的选择器出对话框了（$(echo "$GEO" | tr ' ' ',')）"
	sleep 1
	$VM --socket "$IPC" move-cursor $GEO >/dev/null 2>&1
	sleep 0.3
	$VM --socket "$IPC" pointer-button click >/dev/null 2>&1
else
	fail "没等到 KDE 的选择器对话框"
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

# --- 6/7 真消费：连上那个 node 拿帧，屏幕动起来帧要跟着变 -------------------
if [ -n "${NODE:-}" ]; then
	# 帧率先给个上限：不限速的话帧是**爆发式**的（消费者一还缓冲我们就抓一张），
	# 六帧全在同一瞬间抓 —— 「屏幕变了帧跟着变」那条断言就成了掷骰子（gles2 下
	# 必红）。限到 5fps 之后帧是摊开的，那条断言才有意义（顺带也测了发帧节奏）。
	timeout 60 gst-launch-1.0 -q pipewiresrc path="$NODE" num-buffers=6 \
		! video/x-raw,framerate=5/1 \
		! videoconvert ! pngenc ! multifilesink location="$R/f-%02d.png" \
		>"$R/gst.log" 2>&1 &
	GST=$!
	sleep 1
	# 制造画面变化：开一个纯蓝窗口，然后**一直**开关 overview（动画 = 一堆损伤）。
	# 别只抖一下就等：gst 连上来的时间跟机器负载有关，抖早了它一帧都收不到
	# （套件里就这样红过一次）。
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
		fail "帧没变（$NF 帧全一样）—— 说明喂的是同一张图"
	fi
	# --- 9 直通（dmabuf）：抓帧缓冲直接当 dmabuf 交给消费者 ---------------
	# 这一条是「录制的时候整个桌面卡」那个 bug 的判据：回读（GPU→CPU 同步拷贝）
	# 是主线程上的（2560x1440 一帧 6.2ms），走直通之后 readback_ms 必须是 0。
	# pixman 会话里根本没有 dmabuf（缓冲是 shm），这条路走不了 —— 那种模式下
	# dmabuf_frames 恒为 0，正常。
	if [ "$RENDERER" = gles2 ]; then
		DB=$($VM --socket "$IPC" state 2>/dev/null | python3 -c '
import json,sys
st=json.load(sys.stdin)
sc=st.get("screencast") or []
print(sum(s.get("dmabuf_frames",0) for s in sc),
      max([s.get("readback_ms",0) for s in sc] or [0]),
      max([s.get("sync_ms",0) for s in sc] or [0]))
')
		set -- $DB
		if [ "${1:-0}" -gt 0 ]; then
			pass "走 dmabuf 直通发了 ${1} 帧（一帧不落 CPU）"
		else
			fail "一帧都没走 dmabuf 直通（$RENDERER 下应该有）—— 又退回回读了"
		fi
		if [ "${2:-1}" = "0" ] || [ "${2:-1}" = "0.0" ]; then
			pass "回读成本 0ms（直通那条路上没有 GPU→CPU 拷贝）"
		else
			fail "回读还有 ${2}ms —— 直通没生效"
		fi
	else
		pass "pixman 会话：没有 dmabuf，走回读（直通那条路在 gles2 下测）"
	fi

	# 静止的屏幕 + 晚连上来的消费者：得靠心跳（1Hz）给帧，否则要等到下一次画面
	# 变化才有第一帧 —— 套件里负载高的时候 gst 连晚了，就是这么红的。
	timeout 30 gst-launch-1.0 -q pipewiresrc path="$NODE" num-buffers=2 \
		! videoconvert ! pngenc ! multifilesink location="$R/s-%02d.png" \
		>"$R/gst2.log" 2>&1
	NS=$(ls "$R"/s-*.png 2>/dev/null | wc -l)
	if [ "$NS" -ge 2 ]; then
		pass "画面完全静止时消费者照样拿到帧（$NS 帧，靠 1Hz 心跳）"
	else
		fail "静止画面上只拿到 $NS 帧（心跳没起作用）"
	fi
else
	fail "没有 node，消费那两条测不了"
fi
INNER

mark "2/3 会话名这个开关到底管不管用"
WAYLAND_DISPLAY="$SOCK" timeout 60 dbus-run-session -- bash "$LOG/control.sh" \
	"$RUNDIR" >"$LOG/control.out" 2>&1
sed -n 's/^\(  \(PASS\|FAIL\).*\)$/\1/p' "$LOG/control.out"
grep -aq "^  FAIL" "$LOG/control.out" && FAILED=1

mark "4/5/6/7 私有 dbus 会话里跑真链路"
WAYLAND_DISPLAY="$SOCK" timeout 300 dbus-run-session -- bash "$LOG/inner.sh" \
	"$RUNDIR" "$SOCK" "$IPC" "$RENDERER" >"$LOG/inner.out" 2>&1
sed -n 's/^\(  \(PASS\|FAIL\).*\)$/\1/p' "$LOG/inner.out"
grep -aq "^  FAIL" "$LOG/inner.out" && FAILED=1

mark "8 vidow 全程活着"
if grep -aqiE "Assertion|Aborted|core dump" "$LOG/vidow.log"; then
	fail "vidow 日志里有断言 / abort"
else
	pass "vidow 没崩"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "== test-screencast: 全部通过 =="
else
	echo "== test-screencast: 有失败（完整输出：$LOG/inner.out）=="
fi
exit "$FAILED"
