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
#   8) **按窗口录屏**（types=2）：选择器只列窗口、合成器按 identifier 反查到 view、
#      窗口流出帧（size 报的是窗口尺寸）
#   9) **虚拟输出**（types=4）：合成器里真多出一块 HEADLESS-N，流结束后又拔掉
#  10) **截图接口**：Screenshot 回一个真 PNG（尺寸 = 输出像素尺寸）、PickColor 回 (ddd)
#  11) **光标元数据**（cursor_mode=4）：消费者按 GStreamer 那样要 SPA_META_Cursor 就
#      拿得到（位置 = 指针位置、热点、真位图）；缩放 2 倍时位置仍是像素坐标；
#      反证：hidden 模式下同一个消费者一个字节都拿不到
#  12) 全程 vidow 没崩
#
# 需要：pipewire 在跑、gst-launch-1.0 + gst-plugin-pipewire、python3-gobject、
#       gtk3、wayland-info、build/vidowmsg、build/vidow-portal、build/cursorprobe。
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
	# 只收**自己起的** vidow：临时 HOME（$RUNDIR/home）或者 headless 后端那两种。
	# 桌面上那个真 vidow（用户自己的 HOME、真后端）一个都不许碰 —— 以前这里是无脑
	# `pgrep -x vidow` 全杀，别人正跑着 vidow 的机器上跑这个测试会把人会话干掉。
	for p in $(pgrep -x vidow 2>/dev/null); do
		e=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null)
		case "$e" in
		*"HOME=$RUNDIR/home"*|*"WLR_BACKENDS=headless"*) kill "$p" 2>/dev/null ;;
		esac
	done
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
[ -x ./build/cursorprobe ] || { echo "test-portal-own.sh: 需要 ./build/cursorprobe"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$HOME_DIR/.config/vidow" "$LOG"

# 两块屏**位置钉死**（自动排的话谁在 0 是不确定的，6b2 要验「窗口在非 0 那块屏上」，
# 6e 又假定光标放在逻辑 100,100 时落在 HEADLESS-1 上）。
#
# **HEADLESS-2 故意做成「2560x1440 模式 + 缩放 2」**（逻辑尺寸还是 1280x720，布局
# 和以前一样）：这就是用户那台机器的几何（一块 4K 屏缩放 2 摆在 x=1920）。缩放不为 1
# 的那块屏上，「合成器开的流缓冲（逻辑 × scale 设备像素）」和「portal 报给前端的
# size」**必须是一个口径** —— 对不上时消费者（OBS）会按比值换算光标位置 / 协商缓冲，
# 表现就是「2 倍屏上录屏看不到光标、第一次录还是黑的」（用户报的）。6a 的流尺寸、
# 6b2 的窗口流尺寸 + 光标位置就是钉这一条的。
cat > "$HOME_DIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", x = 0, y = 0, scale = 1 }
vidow.output{ name = "HEADLESS-2", x = 1280, y = 0, scale = 2, mode = "2560x1440" }
CFG

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

WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman VIDOW_LOG=info \
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

# 等选择器出现，回车确认（「共享」按钮在右下角，但对话框的窗口几何和按钮实际
# 位置差着几十像素 —— 点坐标在内容不同的几轮里会飘；回车走的是同一个 accept 路径，
# 而且默认选中第一行，稳）。
wait_dialog() {
	local geo="" i
	for i in $(seq 1 40); do
		sleep 1
		geo=$($VM --socket "$IPC" windows 2>/dev/null | python3 -c '
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
		[ -n "$geo" ] && break
	done
	echo "$geo"
}

wait_and_share() {
	local geo i
	geo=$(wait_dialog)
	[ -z "$geo" ] && return 1
	sleep 1
	for i in $(seq 1 20); do
		$VM --socket "$IPC" send-key tap Return >/dev/null 2>&1
		sleep 0.5
		$VM --socket "$IPC" windows 2>/dev/null | grep -aq "共享屏幕" || return 0
	done
	return 0
}

# 等探针那边把 node 打出来（探针自己会等 Response）。
wait_node() { # <探针输出文件>
	local i
	for i in $(seq 1 40); do
		grep -aq "node=" "$1" 2>/dev/null && break
		sleep 1
	done
	grep -ao "node=[0-9]*" "$1" 2>/dev/null | head -1 | cut -d= -f2
}

# 数**输出个数**（不是「有几行写着 HEADLESS-」：一个输出的 JSON 里名字会出现好几次，
# 数行会把 2 块输出数成 4 块）。
count_outputs() {
	$VM --socket "$IPC" outputs 2>/dev/null | python3 -c '
import json,sys
try:
    print(len(json.load(sys.stdin)))
except Exception:
    print(-1)'
}

# 这个测试从**两块** headless 屏起步（6b2 要验「窗口在非 0 那块屏上」那条路）。
NOUT0=$(count_outputs)
if [ "${NOUT0:-0}" -ge 2 ]; then
	pass "两块 headless 屏都在（$NOUT0 块）"
else
	fail "只起了 ${NOUT0:-0} 块输出（期望 2 块）"
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
# 报出去的 size 必须是**这块屏的像素尺寸**（= 合成器这一路流的缓冲尺寸，见
# screencast.c 的 stream_output：output->width/height）。缩放屏上写成逻辑尺寸
# （= 像素 / scale）消费者就按比值换算光标位置 / 协商缓冲，表现是「2 倍屏上录屏
# 看不到光标、第一次录还黑屏」—— 所以这里现算期望值，不写死。
SOUT1=$(grep -a "screencast: 请求录 " "$R/vidow.log" 2>/dev/null | tail -1 |
	sed 's/.*请求录 \([^（]*\)（.*/\1/')
EXP1=$($VM --socket "$IPC" outputs 2>/dev/null | python3 -c '
import json,sys
want = sys.argv[1]
try:
    outs = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for o in outs:
    if o["name"] == want:
        print("%d, %d" % (o["layout"]["width"] * o["scale"],
                          o["layout"]["height"] * o["scale"]))
        break
' "${SOUT1:-}")
if [ -n "${SIZE:-}" ] && [ "${SIZE:-}" = "${EXP1:-}" ]; then
	pass "流尺寸 = 那块屏的像素尺寸（$SIZE = ${SOUT1:-?} 的逻辑尺寸 × scale）"
else
	fail "流尺寸不对：报的是 ${SIZE:-没拿到}，${SOUT1:-?} 的像素尺寸是 ${EXP1:-没算出来}"
fi

# position 报的是**布局逻辑坐标**（前端要的口径）。以前这里一直是 0,0 —— 因为读的
# 是 wl_output.geometry 的 x/y，而 wlroots 那对恒为 0,0；现在读 xdg-output 的
# logical_position。期望值同样现算，不写死。
POS=$(grep -ao "position': ([0-9-]*, [0-9-]*)" "$R/probe.txt" 2>/dev/null | head -1 |
	grep -o "[0-9-]*, [0-9-]*")
EXPPOS=$($VM --socket "$IPC" outputs 2>/dev/null | python3 -c '
import json,sys
want = sys.argv[1]
try:
    outs = json.load(sys.stdin)
except Exception:
    sys.exit(0)
for o in outs:
    if o["name"] == want:
        print("%d, %d" % (o["layout"]["x"], o["layout"]["y"]))
        break
' "${SOUT1:-}")
if [ -n "${POS:-}" ] && [ "${POS:-}" = "${EXPPOS:-}" ]; then
	pass "流的 position = 那块屏在布局里的位置（$POS）"
else
	fail "流的 position 不对：报的是 ${POS:-没拿到}，${SOUT1:-?} 在布局里是 ${EXPPOS:-没算出来}"
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

	# 抓帧节奏：以前**每个输出提交都抓一整帧**（整块离屏渲染 + 消费者有空缓冲时
	# 还要回读一整块），144Hz 的屏上主线程被占满 —— 屏幕上所有动画 / 拖动跟着掉到
	# 60fps（用户报的「portal 录制导致显示器帧率被限制到 60」）。现在抓帧只在
	# 「消费者要一帧、而且到点了」的时候发生（见 screencast.c 的 stream_send_due）。
	#
	# 判据：抓帧次数跟**发出去的帧数**一个量级，而不是跟输出提交次数一个量级。
	$VM --socket "$IPC" state > "$R/state-sc.json" 2>/dev/null
	python3 - "$R/state-sc.json" <<'PYEOF'
import json, sys
try:
    d = json.load(open(sys.argv[1], encoding='utf-8'))
except Exception as e:
    print("  FAIL  读不到 state：%s" % e); raise SystemExit(1)
streams = [s for s in d.get("screencast", []) if s.get("frames", 0) > 0]
if not streams:
    print("  FAIL  state.screencast 里没有发过帧的流"); raise SystemExit(1)
bad = 0
missing = []
for s in streams:
    print("      （%s：发帧 %d、抓帧 %d、输出提交 %d、消费者 %.0ffps、上限 %.1fms）" %
          (s.get("kind"), s["frames"], s["grabs"], s["commits"],
           s.get("consumer_fps", 0), s.get("cap_ms", 0)))
    print("      （一帧的主线程成本：抓帧渲染 %.2fms（最多 %.2fms）、回读 %.2fms、"
          "等 GPU %.2fms；dmabuf 直通 %d 帧）" %
          (s.get("grab_ms", 0), s.get("grab_ms_max", 0), s.get("readback_ms", 0),
           s.get("sync_ms", 0), s.get("dmabuf_frames", 0)))
    for k in ("grab_ms", "grab_ms_max", "readback_ms", "sync_ms", "dmabuf_frames"):
        if k not in s:
            missing.append(k)
    if s["grabs"] > s["frames"] + 2:
        bad += 1
if missing:
    print("  FAIL  state.screencast 少了成本字段：%s" % ", ".join(sorted(set(missing))))
    bad += 1
else:
    print("  PASS  每路流都报了一帧的主线程成本（grab_ms / readback_ms / sync_ms / "
          "dmabuf_frames）")
if bad == 0:
    print("  PASS  抓帧次数跟着发帧次数走（不是每次输出提交都抓一整帧）")
else:
    print("  FAIL  %d 路流的抓帧次数远超发帧次数 —— 又在每次提交都抓整帧了" % bad)
big = [s for s in streams if s["commits"] > s["frames"] + 5]
if big:
    print("  PASS  输出提交次数远多于发帧次数（%d 路：提交 >> 发帧）" % len(big))
else:
    print("  FAIL  没有一路流能证明「提交比发帧多」—— 场景没造出来")
capped = [s for s in streams if s.get("cap_ms", 0) > 0]
if capped:
    print("  PASS  发帧节奏有上限（%.1fms）" % capped[0]["cap_ms"])
else:
    print("  FAIL  发帧没有节奏上限（cap_ms = 0）")
sys.exit(1 if (bad or not big or not capped) else 0)
PYEOF
[ $? -eq 0 ] || FAILED=1
else
	fail "没有 node，消费那几条测不了"
fi

# --- 6b 按窗口录屏（stream_window） ----------------------------------------
# 对话框这一轮只列窗口（types=2），第一个窗口默认选中 —— 点「共享」就是「录这个
# 窗口」。窗口的 identifier 由 ext-foreign-toplevel-list 给，原样回给合成器。
echo; echo "== 6b. 按窗口录屏 =="
# 第 7 节起的 bluewin 到这儿没用了，先收掉：按窗口录屏这几轮靠 pattern，列表里只剩
# 一个窗口，「第一行是谁」就没歧义（留着它的话被录的是 bluewin —— 尺寸恰好一样，
# 断言会「碰巧对」）。
pkill -f "tools/bluewin.py" 2>/dev/null
sleep 1
python3 /home/busyo/Dev/Vidow/tools/pattern.py >"$R/pattern.log" 2>&1 &
PWIN=$!
for i in $(seq 1 20); do
	$VM --socket "$IPC" windows 2>/dev/null | grep -aq '"pattern"' && break
	sleep 0.5
done
WGEO=$($VM --socket "$IPC" windows 2>/dev/null | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if w.get("title") == "pattern":
        g=w["geometry"]
        print("%d %d" % (g["width"], g["height"]))
        break
')
timeout 150 python3 /home/busyo/Dev/Vidow/tools/screencastprobe.py --types 2 \
	--timeout 60 --hold 20 >"$R/probe2.txt" 2>&1 &
if wait_and_share; then
	pass "窗口那轮的选择器弹出来了（只列窗口）"
else
	fail "窗口那轮没等到选择器"
fi
NODE2=$(wait_node "$R/probe2.txt")
if [ -n "${NODE2:-}" ]; then
	pass "窗口流拿到了 node（node=$NODE2）"
else
	fail "窗口流没拿到 node：$(tail -3 "$R/probe2.txt" 2>/dev/null | tr '\n' ' ')"
fi
WSIZE=$(grep -ao "size': ([0-9]*, [0-9]*)" "$R/probe2.txt" 2>/dev/null | head -1 |
	grep -o "[0-9]*, [0-9]*")
if [ -n "${WSIZE:-}" ] && [ "${WSIZE%%,*}" != "0" ]; then
	pass "窗口流报了窗口尺寸（$WSIZE，vidow 里那个窗口是 ${WGEO:-?}）"
else
	fail "窗口流没报尺寸：${WSIZE:-没拿到}"
fi
if grep -aq "录窗口：identifier=" "$R/helper.log" 2>/dev/null &&
	grep -aq "screencast: 请求录窗口" "$R/vidow.log" 2>/dev/null; then
	pass "两边日志都记着「录的是窗口」（helper 发 identifier，合成器反查到了 view）"
else
	fail "日志里看不到按窗口录屏的痕迹"
fi
if [ -n "${NODE2:-}" ]; then
	timeout 40 gst-launch-1.0 -q pipewiresrc path="$NODE2" num-buffers=4 \
		! videoconvert ! pngenc ! multifilesink location="$R/w-%02d.png" \
		>"$R/gst-win.log" 2>&1
	NW=$(ls "$R"/w-*.png 2>/dev/null | wc -l)
	NDW=$(md5sum "$R"/w-*.png 2>/dev/null | awk '{print $1}' | sort -u | wc -l)
	if [ "$NW" -ge 3 ]; then
		pass "窗口流真出帧（$NW 帧，$NDW 种画面 —— 窗口里那个动画在动）"
	else
		fail "窗口流只拿到 $NW 帧：$(head -2 "$R/gst-win.log" 2>/dev/null | tr '\n' ' ')"
	fi
fi

# --- 6b2 按窗口录屏 + 光标元数据（metadata） --------------------------------
# 用户报的 OBS 那条路就是这个组合：SelectSources 里 cursor_mode=4（metadata）+
# 录的是**窗口**。窗口流的画面是「窗口等比缩放进流」，那个缩放跟屏的缩放不是一回事
# （跟窗口在屏上多大无关）。光标要是按屏的框换算，位置整个落到流外面 —— 元数据里
# 永远是 id=0，表现就是「有画面、光标不进来」。
#
# 这一轮**复用 6b 那个窗口**（6b 的 kill 挪到后面）：窗口只剩一个，选择器里第一个
# 就是它。不然刚 kill 掉的那个还在窗口列表里（关窗有动画），选择器默认选中它，
# Start 会卡在那个正在消失的窗口上（实测：Start 一直不回，后面的虚拟输出那轮也跟着
# 报 code=2）。
echo; echo "== 6b2. 按窗口录屏 + 光标元数据（OBS 那个组合） =="
# 6b 那路流还挂着（--hold 20）：等它自己收摊再开这一路，别两条会话叠着。
for i in $(seq 1 30); do
	pgrep -f "screencastprobe.py --types 2" >/dev/null 2>&1 || break
	sleep 1
done
# **把窗口搬到非 0 那块屏**：视频流的 output 是按窗口在布局里的位置反查的。以前拿
# 客户端声明的窗口几何（x/y 是表面内部的偏移）当布局坐标，反查出来永远是布局左上角
# 那一块 —— 窗口在另一块屏上时，光标元数据就在错的那块屏上找光标，永远 id=0
# （用户报的「非 0 显示器上采集不到光标」）。两块屏谁在 0 是自动排的，按 x 现查。
OUT0=$($VM --socket "$IPC" outputs 2>/dev/null | python3 -c '
import json,sys
try:
    outs=json.load(sys.stdin)
except Exception:
    sys.exit(0)
outs=outs.get("outputs",outs) if isinstance(outs,dict) else outs
for o in outs:
    if o["layout"]["x"] == 0:
        print(o["name"])
        break
')
OUT_OTHER=$($VM --socket "$IPC" outputs 2>/dev/null | python3 -c '
import json,sys
try:
    outs=json.load(sys.stdin)
except Exception:
    sys.exit(0)
outs=outs.get("outputs",outs) if isinstance(outs,dict) else outs
for o in outs:
    if o["layout"]["x"] != 0:
        print(o["name"])
        break
')
# 现查窗口**当下**的位置（列表滚动 / 6b 那一轮可能把它挪过），再点它一下聚焦
# （move-window-to-output 作用于聚焦窗口）。
WGEO_PRE=$($VM --socket "$IPC" windows 2>/dev/null | python3 -c '
import json,sys
try:
    ws=json.load(sys.stdin)
except Exception:
    sys.exit(0)
for w in ws:
    if w.get("title") == "pattern":
        g=w["geometry"]
        print("%d %d %d %d" % (g["x"], g["y"], g["width"], g["height"]))
        break
')
if [ -n "${OUT_OTHER:-}" ] && [ -n "${WGEO_PRE:-}" ]; then
	set -- $WGEO_PRE
	$VM --socket "$IPC" pick-window $(( $1 + $3 / 2 )) $(( $2 + $4 / 2 )) >/dev/null 2>&1
	sleep 0.5
	$VM --socket "$IPC" move-window-to-output "$OUT_OTHER" >/dev/null 2>&1
	sleep 1.5
	echo "== 窗口搬到 $OUT_OTHER（原来在 ${WGEO_PRE}）"
else
	fail "没找到非 0 那块屏（只有 $OUT0）或窗口几何（${WGEO_PRE:-空}）—— 6b2 这条测不了"
fi
timeout 150 python3 /home/busyo/Dev/Vidow/tools/screencastprobe.py --types 2 \
	--cursor-mode 4 --timeout 60 --hold 30 >"$R/probe6.txt" 2>&1 &
# 这一轮走**双击卡片预览**那条交互（用户要的：预览在上、名字在下，点预览就是选，
# 双击直接确认），顺带把它钉住。第一张卡片的预览中心 = 对话框原点 +（112, 140）：
# 提示一行 + 卡片内边距 6 + 预览 176x110 的中心（这一轮只列窗口 = 只有一个 tab，
# 顶上的分段控件不显示；三个 tab 时卡片还要往下让出那条控件的高度）。
DGEO=$(wait_dialog)
if [ -n "${DGEO:-}" ]; then
	# 对话框上屏之后还会被客户端「居中」一次（GTK 的 GTK_WIN_POS_CENTER_ALWAYS）：
	# 位置会从浮动默认位置挪到那块屏的中间（实测 1286 -> 1920）。点之前重新读一遍。
	sleep 1
	DGEO=$(wait_dialog)
	set -- $DGEO
	CARD_X=$(( $1 + 112 ))
	CARD_Y=$(( $2 + 140 ))
	# 落点得真在对话框里（偏移写错了会点到别的窗口上，静默不确认，很难查）。
	if [ "$CARD_X" -lt "$1" ] || [ "$CARD_X" -gt $(( $1 + $3 )) ] ||
			[ "$CARD_Y" -lt "$2" ] || [ "$CARD_Y" -gt $(( $2 + $4 )) ]; then
		fail "第一张卡片的落点跑到对话框外面了（对话框 $DGEO，落点 $CARD_X,$CARD_Y）"
		CARD_X=""
	fi
	[ -z "${CARD_X:-}" ] || {
		# GTK 的双击判定窗口窄，一次点不中很常见（漏一次后面的流就起不来，
		# 连着虚拟输出那轮也报 code=2）—— 没关就再点一次，最多三次。
		for _try in 1 2 3; do
			$VM --socket "$IPC" move-cursor "$CARD_X" "$CARD_Y" >/dev/null 2>&1
			sleep 0.6
			$VM --socket "$IPC" pointer-button click left 2 >/dev/null 2>&1
			sleep 2
			$VM --socket "$IPC" windows 2>/dev/null | grep -aq "共享屏幕" || break
		done
	}
	if $VM --socket "$IPC" windows 2>/dev/null | grep -aq "共享屏幕"; then
		fail "双击卡片预览没确认（对话框还开着；对话框在 $DGEO）"
	else
		pass "双击第一张卡片的预览直接确认了（对话框关了）"
	fi
else
	fail "窗口 + metadata 那轮没等到选择器"
fi
NODE6=$(wait_node "$R/probe6.txt")
if [ -n "${NODE6:-}" ]; then
	pass "窗口 + metadata 流拿到了 node（node=$NODE6）"
else
	fail "窗口 + metadata 流没拿到 node：$(tail -3 "$R/probe6.txt" 2>/dev/null | tr '\n' ' ')"
fi
WSIZE6=$(grep -ao "size': ([0-9]*, [0-9]*)" "$R/probe6.txt" 2>/dev/null | head -1 |
	grep -o "[0-9]*, [0-9]*" | tr -d ' ')
# 流选的那块输出得是**窗口真正所在**的那块（就是上面 6b2 开头搬到的那块）。
WOUT=$($VM --socket "$IPC" windows 2>/dev/null | python3 -c '
import json,sys
try:
    ws=json.load(sys.stdin)
except Exception:
    sys.exit(0)
for w in ws:
    if w.get("title") == "pattern":
        print(w.get("output") or "")
        break
')
RECLINE=$(grep -a "screencast: 请求录窗口" "$R/vidow.log" 2>/dev/null | tail -1)
case "${RECLINE:-}" in
*"$OUT_OTHER scale"*)
	pass "流选的输出 = 窗口所在那块屏（$OUT_OTHER；$WOUT）" ;;
*)
	fail "流选的输出不对：${RECLINE:-没日志}（窗口在 $OUT_OTHER）" ;;
esac
# **报出去的 size 必须等于这一路流的缓冲尺寸**（日志里「→ WxH」就是合成器给它开的
# 缓冲）：消费者（OBS）按 size 当元数据换算光标位置 / 协商缓冲，缩放屏上对不上就是
# 「录屏里看不到光标、第一次录还是黑的」。以前这里报的是窗口的**逻辑**尺寸，2 倍屏上
# 正好小一半 —— HEADLESS-2 是缩放 2 那块，所以这条能钉住它。
BUF6=$(echo "${RECLINE:-}" | sed -n 's/.*→ \([0-9]*x[0-9]*\).*/\1/p')
EXP6=$(echo "${BUF6:-}" | tr 'x' ',')
if [ -n "${WSIZE6:-}" ] && [ "${WSIZE6:-}" = "${EXP6:-}" ]; then
	pass "窗口流 size = 这一路流的缓冲尺寸（$WSIZE6 = 日志里的 $BUF6）"
else
	fail "窗口流 size 跟缓冲对不上：报的是 ${WSIZE6:-没拿到}，缓冲是 ${BUF6:-没拿到}"
fi
# 光标要放到**真被录的那个窗口**中间：列表里第一行才是被录的，别按标题猜
# （这一轮录的是谁，合成器日志里写着）。
REC6=$(grep -a "screencast: 请求录窗口" "$R/vidow.log" 2>/dev/null | tail -1 |
	sed -n 's/.*请求录窗口「\([^」]*\)」.*/\1/p')
WGEO2=$($VM --socket "$IPC" windows 2>/dev/null | python3 -c '
import json,sys
want = sys.argv[1]
try:
    ws=json.load(sys.stdin)
except Exception:
    sys.exit(0)
for w in ws:
    if (w.get("title") or "") == want:
        g=w["geometry"]
        print("%d %d %d %d" % (g["x"], g["y"], g["width"], g["height"]))
        break
' "${REC6:-}")
# 光标放到窗口**中间**：窗口铺满它那一格，取景框就是窗口表面，中心对中心。
if [ -n "${WGEO2:-}" ] && [ -n "${NODE6:-}" ]; then
	set -- $WGEO2
	CX=$(( $1 + $3 / 2 ))
	CY=$(( $2 + $4 / 2 ))
	$VM --socket "$IPC" move-cursor "$CX" "$CY" >/dev/null 2>&1
	sleep 1
	./build/cursorprobe "$NODE6" --declare --timeout 15 >"$R/cursor6.txt" 2>&1
	CL6=$(grep -a "bitmap=1" "$R/cursor6.txt" 2>/dev/null | head -1)
	if [ -n "${CL6:-}" ]; then
		pass "窗口流也拿到了光标位图：$CL6"
	else
		fail "窗口流没拿到光标位图：$(head -2 "$R/cursor6.txt" 2>/dev/null | tr '\n' ' ')"
	fi
	if echo "${CL6:-}" | grep -q "id=1"; then
		pass "窗口流的元数据里 id=1（有可见光标）"
	else
		fail "窗口流的元数据里 id≠1：${CL6:-没数据}"
	fi
	POS6=$(python3 - "$R/cursor6.txt" "${WSIZE6:-0,0}" <<'PY6'
import re, sys
line = ""
for l in open(sys.argv[1], errors="replace"):
    if "bitmap=1" in l:
        line = l
        break
w, h = (int(x) for x in sys.argv[2].split(","))
m = re.search(r"pos=(-?\d+),(-?\d+)", line)
if not m or w <= 0:
    print("没读到位置（流 %dx%d）" % (w, h))
else:
    x, y = int(m.group(1)), int(m.group(2))
    mid = (abs(x - w / 2) <= 40 and abs(y - h / 2) <= 40)
    print("%s pos=%d,%d 流 %dx%d 中心 %d,%d" %
          ("OK" if mid else "BAD", x, y, w, h, w // 2, h // 2))
PY6
)
	case "$POS6" in
	OK*) pass "光标在窗口中间 → 元数据里的位置就在流的中间（$POS6）" ;;
	*) fail "光标位置不对：$POS6" ;;
	esac
	# 指针在**静止的窗口**上移动时，帧也得跟着来：光标不进画面（metadata）时，
	# 它的位置 / 位图就是这一帧的内容。只有「输出提交 + 1Hz 心跳」两个推手的话，
	# 消费者手里会一直停在上一次那一帧的光标状态上（用户报的「录屏里光标时有时无 /
	# 现在又看不到光标了」）。这里判据是合成器日志：指针一动它就主动催帧。
	$VM --socket "$IPC" move-cursor $((CX + 40)) $((CY + 20)) >/dev/null 2>&1
	$VM --socket "$IPC" move-cursor $((CX - 40)) $((CY - 20)) >/dev/null 2>&1
	sleep 0.4
	if grep -aq "指针动了 —— 这一路开始跟着指针推帧" "$R/vidow.log"; then
		pass "指针移动本身也推帧（光标不进画面时，光标位置就是这一帧的内容）"
	else
		fail "指针移动没有推帧 —— 消费者手里的光标会停在上一帧的位置上"
	fi
else
	fail "窗口几何 / node 没拿到，光标那几条测不了（录的是 ${REC6:-?}，WGEO2=${WGEO2:-空}，node=${NODE6:-空}）"
fi
kill $PWIN 2>/dev/null

# --- 6c 虚拟输出（stream_virtual_output） ----------------------------------
echo; echo "== 6c. 虚拟输出（现场加一块 headless 输出） =="
timeout 150 python3 /home/busyo/Dev/Vidow/tools/screencastprobe.py --types 4 \
	--timeout 60 --hold 8 >"$R/probe3.txt" 2>&1 &
if wait_and_share; then
	pass "虚拟屏幕那轮的选择器弹出来了"
else
	fail "虚拟屏幕那轮没等到选择器"
fi
NODE3=$(wait_node "$R/probe3.txt")
if [ -n "${NODE3:-}" ]; then
	pass "虚拟输出流拿到了 node（node=$NODE3）"
else
	fail "虚拟输出没拿到 node：$(tail -3 "$R/probe3.txt" 2>/dev/null | tr '\n' ' ')"
fi
for i in $(seq 1 20); do
	[ "$(count_outputs)" -gt "${NOUT0:-1}" ] && break
	sleep 0.5
done
NOUT=$(count_outputs)
if [ "${NOUT:-0}" -gt "${NOUT0:-1}" ]; then
	pass "合成器里真多出来一块输出（$NOUT0 块 → $NOUT 块）"
else
	fail "输出没变多（$NOUT0 块 → $NOUT 块）"
fi
if grep -aq "加了块虚拟输出" "$R/vidow.log" 2>/dev/null; then
	pass "合成器日志记着加了虚拟输出"
else
	fail "合成器没记虚拟输出"
fi
if [ -n "${NODE3:-}" ]; then
	timeout 40 gst-launch-1.0 -q pipewiresrc path="$NODE3" num-buffers=2 \
		! videoconvert ! pngenc ! multifilesink location="$R/v-%02d.png" \
		>"$R/gst-virt.log" 2>&1
	NV=$(ls "$R"/v-*.png 2>/dev/null | wc -l)
	if [ "$NV" -ge 2 ]; then
		pass "虚拟输出真出帧（$NV 帧，空屏靠心跳）"
	else
		fail "虚拟输出只拿到 $NV 帧：$(head -2 "$R/gst-virt.log" 2>/dev/null | tr '\n' ' ')"
	fi
fi
# 探针退出 → 会话关掉 → 这一路流结束 → 虚拟输出该被拔掉。
for i in $(seq 1 45); do
	[ "$(count_outputs)" -le "${NOUT0:-1}" ] && break
	sleep 1
done
NOUT2=$(count_outputs)
if [ "${NOUT2:-9}" -eq "${NOUT0:-1}" ]; then
	pass "这一路流结束之后虚拟输出被拔掉了（回到 $NOUT2 块）"
else
	fail "虚拟输出没拔掉（还有 $NOUT2 块输出，基线 $NOUT0 块）"
fi

# --- 6c2 区域（框一块屏来录） ----------------------------------------------
#
# 「区域」是选择器里的第三个 tab：**点那张卡**就铺一层全屏浮层，在**活的**画面上
# 拖一个框（不冻屏 —— 冻屏是截图那条路），松手把这块区域记下来，再按「共享」。
# 这一轮钉住：tab 切得过去、浮层能拖、区域坐标报得对（协议要的是**布局逻辑坐标**，
# 报给前端的 size 是像素尺寸）、合成器收到的就是那块区域、消费者真出帧。
echo; echo "== 6c2. 按区域录屏（框一块，不冻屏） =="
timeout 150 python3 /home/busyo/Dev/Vidow/tools/screencastprobe.py --types 1 \
	--timeout 60 --hold 20 >"$R/probe-reg.txt" 2>&1 &
DGEO=$(wait_dialog)
if [ -n "${DGEO:-}" ]; then
	pass "区域那轮的选择器弹出来了（types=1：屏幕 + 区域两个 tab）"
else
	fail "区域那轮没等到选择器"
fi
if [ -n "${DGEO:-}" ]; then
	sleep 1
	DGEO=$(wait_dialog)
	set -- $DGEO
	# 分段控件居中排布（两段各约 81 宽、整条 y 约 82..116，中线约 99px）
	$VM --socket "$IPC" move-cursor $(( $1 + $3 / 2 + 66 )) $(( $2 + 97 )) >/dev/null 2>&1
	sleep 0.3
	$VM --socket "$IPC" pointer-button click left >/dev/null 2>&1
	sleep 0.8
	# 区域那一页只有一张卡：**点这张卡**就是「去框一块」（没有单独的按钮了）
	$VM --socket "$IPC" move-cursor $(( $1 + 113 )) $(( $2 + 214 )) >/dev/null 2>&1
	sleep 0.3
	$VM --socket "$IPC" pointer-button click left >/dev/null 2>&1
	sleep 1.5
	if $VM --socket "$IPC" windows 2>/dev/null | grep -aq "选择区域"; then
		pass "框选浮层弹出来了（全屏，没冻屏）"
	else
		fail "点了区域那张卡但浮层没出来"
	fi
	# 在活的画面上拖一块。**坐标是布局逻辑坐标**，而且得拖在浮层所在那块屏上
	# （浮层跟着光标走：光标在 HEADLESS-2，对话框也在那儿）——HEADLESS-2 在布局
	# x=1280、缩放 2，所以拖 1400,100 → 1900,400 = 这块屏上的 120,100 500x300。
	$VM --socket "$IPC" move-cursor 1400 100 >/dev/null 2>&1
	sleep 0.3
	$VM --socket "$IPC" pointer-button press left >/dev/null 2>&1
	sleep 0.3
	$VM --socket "$IPC" move-cursor 1900 400 >/dev/null 2>&1
	sleep 0.5
	$VM --socket "$IPC" pointer-button release left >/dev/null 2>&1
	sleep 1.2
	if $VM --socket "$IPC" windows 2>/dev/null | grep -aq "选择区域"; then
		fail "松手之后框选浮层没关掉"
	else
		pass "松手之后浮层关掉、回到选择器（区域记下来了）"
	fi
	if grep -aq "区域：.*拖一个框" "$R/helper.log" 2>/dev/null; then
		pass "后端日志记着开了框选浮层"
	else
		fail "后端日志没记框选：$(tail -3 "$R/helper.log" 2>/dev/null | tr '\n' ' ')"
	fi
	if grep -aq "区域：.*上 120,100 500x300" "$R/helper.log" 2>/dev/null; then
		pass "框出来的区域是 120,100 500x300（相对那块屏的逻辑坐标）"
	else
		fail "区域坐标不对：$(grep -a "区域：" "$R/helper.log" 2>/dev/null | tail -2 | tr '\n' ' ')"
	fi
	# 按「共享」（浮层那次点击之后默认选中的就是刚框好的区域，回车同一条路）
	wait_and_share
fi
NODEREG=$(wait_node "$R/probe-reg.txt")
if [ -n "${NODEREG:-}" ]; then
	pass "区域流拿到了 node（node=$NODEREG）"
else
	fail "区域流没拿到 node：$(tail -3 "$R/probe-reg.txt" 2>/dev/null | tr '\n' ' ')"
fi
if grep -aq "请求录区域 1400,100 500x300" "$R/vidow.log" 2>/dev/null; then
	pass "合成器收到的就是框的那块区域（协议要布局坐标：1400,100 500x300）"
else
	fail "合成器没收到那块区域：$(grep -a "请求录区域" "$R/vidow.log" 2>/dev/null | tail -1)"
fi
RSIZE=$(grep -ao "size': ([0-9]*, [0-9]*)" "$R/probe-reg.txt" 2>/dev/null | head -1 |
	grep -o "[0-9]*, [0-9]*" | tr -d ' ')
if [ "${RSIZE:-}" = "1000,600" ]; then
	pass "报给前端的 size = 区域的**像素**尺寸（500x300 逻辑 × 缩放 2 = 1000, 600）"
else
	fail "报给前端的 size 不对：${RSIZE:-空}（想要 1000,600 = 500x300 逻辑 × 缩放 2）"
fi
if [ -n "${NODEREG:-}" ]; then
	timeout 40 gst-launch-1.0 -q pipewiresrc path="$NODEREG" num-buffers=2 \
		! videoconvert ! pngenc ! multifilesink location="$R/reg-%02d.png" \
		>"$R/gst-reg.log" 2>&1
	NR=$(ls "$R"/reg-*.png 2>/dev/null | wc -l)
	if [ "$NR" -ge 2 ]; then
		pass "区域流真出帧（$NR 帧）"
	else
		fail "区域流只拿到 $NR 帧：$(head -2 "$R/gst-reg.log" 2>/dev/null | tr '\n' ' ')"
	fi
fi

# --- 6d 截图 / 取色（Screenshot / PickColor） ------------------------------
echo; echo "== 6d. 截图接口（Screenshot / PickColor） =="
if gdbus introspect --session --dest org.freedesktop.impl.portal.desktop.vidow \
	--object-path /org/freedesktop/portal/desktop 2>/dev/null |
	grep -q "interface org.freedesktop.impl.portal.Screenshot"; then
	pass "vidow 的后端导出了 Screenshot 接口"
else
	fail "后端没导出 Screenshot"
fi
timeout 60 python3 /home/busyo/Dev/Vidow/tools/screenshotprobe.py \
	--timeout 30 >"$R/shot.txt" 2>&1
SHOT_URI=$(grep -ao "uri=file://[^ ]*" "$R/shot.txt" 2>/dev/null | head -1 | cut -d= -f2-)
if [ -n "${SHOT_URI:-}" ] && [ -f "${SHOT_URI#file://}" ]; then
	SHOT_SIZE=$(python3 -c '
import sys
from PIL import Image
print("%dx%d" % Image.open(sys.argv[1]).size)' "${SHOT_URI#file://}" 2>/dev/null)
	pass "整屏截图回了个真文件（$SHOT_URI，$SHOT_SIZE）"
	# 截图是**输出像素**尺寸（哪块屏取决于截图请求选的那块，两块都算过）。
	if [ "${SHOT_SIZE:-}" = "1280x720" ] || [ "${SHOT_SIZE:-}" = "2560x1440" ]; then
		pass "截图尺寸 = 输出像素尺寸（$SHOT_SIZE）"
	else
		fail "截图尺寸不对：${SHOT_SIZE:-读不出来}（期望 1280x720 或 2560x1440）"
	fi
else
	fail "截图没回 uri：$(tail -3 "$R/shot.txt" 2>/dev/null | tr '\n' ' ')"
fi
timeout 90 python3 /home/busyo/Dev/Vidow/tools/screenshotprobe.py --pick-color \
	--timeout 60 >"$R/pick.txt" 2>&1 &
for i in $(seq 1 30); do
	PICK_GEO=$($VM --socket "$IPC" windows 2>/dev/null | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if "取色" in (w.get("title") or ""):
        g=w["geometry"]
        print("%d %d" % (g["x"] + g["width"] // 2, g["y"] + g["height"] // 2))
        break
')
	[ -n "$PICK_GEO" ] && break
	sleep 1
done
if [ -n "$PICK_GEO" ]; then
	pass "取色的全屏取色窗口弹出来了（点中间一下）"
else
	fail "没等到取色窗口"
fi
# 点一下取色（点空了就再点：取色窗口刚上屏那一下可能还没拿到尺寸）
for i in 1 2 3 4 5; do
	[ -n "$PICK_GEO" ] || break
	set -- $PICK_GEO
	$VM --socket "$IPC" move-cursor "$1" "$2" >/dev/null 2>&1
	sleep 0.4
	$VM --socket "$IPC" pointer-button click >/dev/null 2>&1
	for _ in $(seq 1 6); do
		grep -aq "color=(" "$R/pick.txt" 2>/dev/null && break
		sleep 1
	done
	grep -aq "color=(" "$R/pick.txt" 2>/dev/null && break
done
for i in $(seq 1 30); do
	grep -aq "color=" "$R/pick.txt" 2>/dev/null && break
	sleep 1
done
if grep -aq "color=(" "$R/pick.txt" 2>/dev/null; then
	pass "取色回了个颜色（$(grep -ao "color=.*" "$R/pick.txt" | head -1)）"
else
	fail "取色没回颜色：$(tail -3 "$R/pick.txt" 2>/dev/null | tr '\n' ' ')"
fi

# --- 6e 光标元数据（pointer_mode=metadata） ---------------------------------
echo; echo "== 6e. 光标元数据（SPA_META_Cursor） =="
# 后端得先报出 metadata（前端按 AvailableCursorModes 校验应用要的 cursor_mode）。
CM=$(gdbus call --session --dest org.freedesktop.impl.portal.desktop.vidow \
	--object-path /org/freedesktop/portal/desktop \
	--method org.freedesktop.DBus.Properties.Get \
	org.freedesktop.impl.portal.ScreenCast AvailableCursorModes 2>/dev/null)
if echo "${CM:-}" | grep -q "uint32 7"; then
	pass "AvailableCursorModes 报了 hidden|embedded|metadata（${CM}）"
else
	fail "AvailableCursorModes 没报 metadata：${CM:-读不出来}"
fi
# 光标得在画面上。**哪块屏是「第一张卡片」跟谁在 0 无关**（输出列表的顺序是自动的），
# 所以指针放到「流那块屏」上的 (100,100)：等流建起来之后按日志里的输出名现查（下面）。
timeout 150 python3 /home/busyo/Dev/Vidow/tools/screencastprobe.py --types 1 \
	--cursor-mode 4 --timeout 60 --hold 30 >"$R/probe4.txt" 2>&1 &
if wait_and_share; then
	pass "metadata 那轮的选择器弹出来了（cursor_mode=4 透传给了后端）"
else
	fail "metadata 那轮没等到选择器"
fi
NODE4=$(wait_node "$R/probe4.txt")
if [ -n "${NODE4:-}" ]; then
	pass "metadata 流拿到了 node（node=$NODE4）"
else
	fail "metadata 流没拿到 node：$(tail -3 "$R/probe4.txt" 2>/dev/null | tr '\n' ' ')"
fi
# 流是哪块屏的（日志里写着），光标就往那块屏上放：元数据里的位置是**这块屏**的
# 输出像素坐标，放错屏就一个光标都找不到（id=0）。
SOUT=$(grep -a "screencast: 请求录 " "$R/vidow.log" 2>/dev/null | tail -1 |
	sed 's/.*请求录 \([^（]*\)（.*/\1/')
SOX=$($VM --socket "$IPC" outputs 2>/dev/null | python3 -c '
import json,sys
want = sys.argv[1]
try:
    outs=json.load(sys.stdin)
except Exception:
    sys.exit(0)
for o in outs:
    if o["name"] == want:
        print(o["layout"]["x"])
        break
' "${SOUT:-}")
# 这块屏的 scale：元数据里的位置是**输出像素**（= 逻辑 × scale），所以逻辑 100,100
# 在缩放 2 的屏上报出来就是 200,200。期望值现算 —— HEADLESS-2 是缩放 2 那块。
SOSC=$($VM --socket "$IPC" outputs 2>/dev/null | python3 -c '
import json,sys
want = sys.argv[1]
try:
    outs=json.load(sys.stdin)
except Exception:
    sys.exit(0)
for o in outs:
    if o["name"] == want:
        print(o["scale"])
        break
' "${SOUT:-}")
echo "      （流是 $SOUT 那块屏，x=${SOX:-?} scale=${SOSC:-?}）"
[ -n "${SOX:-}" ] && { $VM --socket "$IPC" move-cursor $(( SOX + 100 )) 100 >/dev/null 2>&1; sleep 0.5; }
# 消费者要按**生产者声明的那样**去要这个元数据（固定值 / 范围取交集，见
# pipewire 的 pw_buffers_negotiate）：GStreamer 的 pipewiresrc 是这么要的，
# cursorprobe --declare 照抄它。要不到的话缓冲里连 SPA_META_Cursor 都没有。
if [ -n "${NODE4:-}" ]; then
	./build/cursorprobe "$NODE4" --declare --timeout 15 >"$R/cursor.txt" 2>&1
	CLINE=$(grep -a "bitmap=1" "$R/cursor.txt" 2>/dev/null | head -1)
	if [ -n "${CLINE:-}" ]; then
		pass "消费者拿到了光标元数据 + 位图：$CLINE"
	else
		fail "没拿到光标位图：$(tail -2 "$R/cursor.txt" 2>/dev/null | tr '\n' ' ')"
	fi
	if echo "${CLINE:-}" | grep -q "id=1"; then
		pass "元数据里 id=1（有可见光标）"
	else
		fail "元数据里 id≠1：${CLINE:-没数据}"
	fi
	EXP4=$(( 100 * ${SOSC:-1} ))
	if echo "${CLINE:-}" | grep -qE "pos=${EXP4},${EXP4} hotspot=[0-9]+,[0-9]+"; then
		pass "光标位置 = 指针位置（逻辑 100,100 → 流里 pos=${EXP4},${EXP4}，热点也报出来了）"
	else
		fail "光标位置不对：${CLINE:-没数据}（期望 pos=${EXP4},${EXP4}）"
	fi
	if echo "${CLINE:-}" | grep -qE "size=[1-9][0-9]*x[1-9][0-9]* stride=[1-9][0-9]* hash=0x0*[1-9a-f]"; then
		pass "位图有尺寸有内容（$(echo "$CLINE" | grep -o 'size=[0-9x]* stride=[0-9]* hash=0x[0-9a-f]*')）"
	else
		fail "位图是空的：${CLINE:-没数据}"
	fi
	# 缩放屏上的换算：wlr_output_cursor 的 x/y 本来就是**输出像素**，早先按
	# 「逻辑坐标 × scale」算会翻一倍（第二块屏上更离谱，直接画到框外）。
	$VM --socket "$IPC" output-scale "${SOUT:-HEADLESS-1}" 2 >/dev/null 2>&1
	sleep 1
	$VM --socket "$IPC" move-cursor $(( ${SOX:-0} + 100 )) 100 >/dev/null 2>&1
	sleep 0.5
	./build/cursorprobe "$NODE4" --declare --timeout 15 >"$R/cursor2.txt" 2>&1
	if grep -aq "pos=200,200" "$R/cursor2.txt" 2>/dev/null; then
		pass "缩放 2 倍时元数据位置也是像素坐标（逻辑 100,100 → 200,200）"
	else
		fail "缩放 2 倍时位置不对：$(grep -a 'meta=1' "$R/cursor2.txt" | head -1)"
	fi
	$VM --socket "$IPC" output-scale "${SOUT:-HEADLESS-1}" 1 >/dev/null 2>&1
	sleep 1

fi
# 反证：hidden 模式下**同样**要元数据的消费者一个字节都拿不到（元数据是照
# pointer_mode 报的，不是无脑塞）。
timeout 150 python3 /home/busyo/Dev/Vidow/tools/screencastprobe.py --types 1 \
	--cursor-mode 1 --timeout 60 --hold 8 >"$R/probe5.txt" 2>&1 &
if wait_and_share; then
	pass "反证那轮（cursor_mode=1）的选择器弹出来了"
else
	fail "反证那轮没等到选择器"
fi
NODE5=$(wait_node "$R/probe5.txt")
if [ -n "${NODE5:-}" ]; then
	./build/cursorprobe "$NODE5" --declare --timeout 8 >"$R/cursor5.txt" 2>&1
	if grep -aq "RESULT=none" "$R/cursor5.txt" 2>/dev/null; then
		pass "反证：hidden 模式不报光标元数据（消费者要了也没有）"
	else
		fail "hidden 模式居然报了元数据：$(head -1 "$R/cursor5.txt" 2>/dev/null)"
	fi
else
	fail "反证那轮没拿到 node"
fi


# --- 6f 会话持久化（persist_mode / restore_data，impl ScreenCast v4） --------
#
# 规范（flatpak.github.io/xdg-desktop-portal/docs 的 impl ScreenCast，当前 v6）：
#   SelectSources 的 options 里可能带 persist_mode（0/1/2）和 restore_data（(suv)）；
#   Start 的 results 里回 restore_data + persist_mode（有效 persist_mode = 2 时**前端**
#   把它存进权限库、按 app_id 存，下次作为 SelectSources 的 options 再喂给后端）。
# 两边责任分得清楚：**数据存在前端**（我们看不到权限库），后端只做「生成一份能自证的」
# 和「收到时认得出来就跳过选择器」。所以这一节能验的是：
#   1) persist_mode=2 → Start 的 results 里回了 restore_data（(suv) 三元组：vendor=vidow
#      / 私有版本 / 非空数据，数据形如 monitor|HEADLESS-2|1）；
#   2) 同一个 app_id 再请求一次（**客户端什么都不用带**）→ 前端把它喂回来 → 后端
#      **不弹选择器**、直接恢复成同一块源；
#   3) 那块源没了（关掉窗口）→ **回退弹选择器**，不崩；
#   4) 垃圾 restore_data（错 vendor / 错版本 / 空数据 / 结构不对 / D-Bus 类型不对）→
#      忽略 + 弹选择器，一个都不能崩。
#
# 实测到的前端行为（xdp 1.20.4，写下来免得下次又猜）：前端是**两层数据之间的翻译层** ——
# 后端给它 `restore_data`（(suv) 三元组），它换成自己的 **`restore_token`（字符串）** 发给
# 客户端（前端日志：`Replacing restore data received from portal impl with a token`），
# 下次客户端把 token 带回来（SelectSources 的 `restore_token`），它再翻译回 `restore_data`
# 喂给后端。所以：
#   * 判据 1 看的是**后端日志**里那份「回 restore_data」（客户端侧看到的是 token）；
#   * 判据 2/3 走**真链路**：客户端第二次请求把上一轮拿到的 `restore_token` 带回去；
#   * 判据 4 的类型级垃圾只能**直接问后端**（前端按自己的协议先挡一遍，而且客户端塞的
#     `restore_data` 选项它会丢掉 —— 那是后端和前端之间的字段，客户端不该碰）。

cat >"$R/persistprobe.py" <<'PYP'
#!/usr/bin/env python3
"""带持久化的 ScreenCast 请求：SelectSources 里能带 persist_mode / restore_data，
Start 的 results 里把 restore_data / persist_mode 打出来。"""
import argparse, os, sys
import gi
gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib

DEST = "org.freedesktop.portal.Desktop"
OBJ = "/org/freedesktop/portal/desktop"
SC = "org.freedesktop.portal.ScreenCast"
REQ = "org.freedesktop.portal.Request"

ap = argparse.ArgumentParser()
ap.add_argument("--types", type=int, default=1)
ap.add_argument("--cursor-mode", type=int, default=1)
ap.add_argument("--persist", type=int, default=0)
ap.add_argument("--restore-data", default=None,
                help="私有数据字符串；给了就在 SelectSources 里带 restore_data=(suv)")
ap.add_argument("--restore-token", default=None,
                help="前端在 Start 的 results 里给的那个 token（下一次请求带回来）")
ap.add_argument("--restore-vendor", default="vidow")
ap.add_argument("--restore-version", type=int, default=1)
ap.add_argument("--timeout", type=float, default=20.0)
args = ap.parse_args()

conn = Gio.bus_get_sync(Gio.BusType.SESSION, None)
sender = conn.get_unique_name()[1:].replace(".", "_")
pid = os.getpid()
seq = [0]
loop = GLib.MainLoop()
seen = {}

def req_path(t):
    return "%s/request/%s/%s" % (OBJ, sender, t)

def sess_path(t):
    return "%s/session/%s/%s" % (OBJ, sender, t)

def on_resp(_c, _s, path, _i, _sig, params_, *rest):
    seen[path] = params_.unpack()
    loop.quit()

conn.signal_subscribe(DEST, REQ, "Response", None, None, 0, on_resp)
conn.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus",
               "org.freedesktop.DBus", "GetId", None, None,
               Gio.DBusCallFlags.NONE, -1, None)

def call(method, params, handle):
    seen.clear()
    box = {}

    def on_timeout():
        box["timeout"] = True
        loop.quit()
        return False

    tid = GLib.timeout_add(int(args.timeout * 1000), on_timeout)
    ret = conn.call_sync(DEST, OBJ, SC, method, params, None,
                         Gio.DBusCallFlags.NONE, -1, None)
    actual = ret.unpack()[0] if ret is not None else "(无)"
    loop.run()
    if not box.get("timeout"):
        GLib.source_remove(tid)
    if box.get("timeout"):
        print("  %s: 超时（%gs 没有 Response）—— 对话框没人点？" % (method, args.timeout),
              flush=True)
        return None
    code, results = seen.get(actual, seen.get(handle, (None, None)))
    if code != 0:
        print("  %s: Response code=%s" % (method, code), flush=True)
        return None
    print("  %s: code=0" % method, flush=True)
    return results or {}

seq[0] += 1
token = "vpp%da%d" % (pid, seq[0])
params = GLib.Variant("(a{sv})", ({
    "handle_token": GLib.Variant("s", token),
    "session_handle_token": GLib.Variant("s", "vpps%d" % pid),
},))
res = call("CreateSession", params, req_path(token))
if res is None:
    sys.exit(1)
session = res.get("session_handle") or sess_path("vpps%d" % pid)

seq[0] += 1
token = "vpp%db%d" % (pid, seq[0])
opts = {
    "handle_token": GLib.Variant("s", token),
    "types": GLib.Variant("u", args.types),
    "cursor_mode": GLib.Variant("u", args.cursor_mode),
}
if args.persist:
    opts["persist_mode"] = GLib.Variant("u", args.persist)
if args.restore_token is not None:
    opts["restore_token"] = GLib.Variant("s", args.restore_token)
    print("  发送 restore_token=%s" % args.restore_token, flush=True)
if args.restore_data is not None:
    opts["restore_data"] = GLib.Variant("(suv)", (
        args.restore_vendor, args.restore_version,
        GLib.Variant("s", args.restore_data)))
    print("  发送 restore_data=(%s, %d, %r)" % (args.restore_vendor,
                                               args.restore_version,
                                               args.restore_data), flush=True)
print("SelectSources:", flush=True)
if call("SelectSources", GLib.Variant("(oa{sv})", (session, opts)),
        req_path(token)) is None:
    sys.exit(1)

seq[0] += 1
token = "vpp%dc%d" % (pid, seq[0])
print("Start:", flush=True)
res = call("Start", GLib.Variant("(osa{sv})", (
    session, "", {"handle_token": GLib.Variant("s", token)})), req_path(token))
if res is None:
    sys.exit(1)
streams = res.get("streams") or []
print("    streams=%d" % len(streams), flush=True)
for node, props in streams:
    print("      node=%s props=%s" % (node, props), flush=True)
rd = res.get("restore_data")
if rd is None:
    print("    没有 restore_data", flush=True)
else:
    vendor, version, data = rd
    print("    restore_data: vendor=%s version=%d data=%s" % (vendor, version, data),
          flush=True)
tok = res.get("restore_token")
if tok is not None:
    print("    restore_token=%s" % tok, flush=True)
pm = res.get("persist_mode")
print("    persist_mode=%s" % ("（没回）" if pm is None else pm), flush=True)
try:
    conn.call_sync(DEST, session, "org.freedesktop.portal.Session", "Close", None, None,
                   Gio.DBusCallFlags.NONE, -1, None)
except Exception as exc:  # noqa: BLE001
    print("关会话失败（不影响前面拿到的流）：%s" % exc, flush=True)
if not streams:
    print("    （没有流）", flush=True)
    sys.exit(1)
print("OK", flush=True)
sys.exit(0)
PYP


picker_lines() { grep -ac "Start，弹选择器" "$R/helper.log" 2>/dev/null; }
# 后端回的那份 restore_data（日志里的三元组）→ shell 变量：RDV / RDEV / RDD / RDCNT。
rd_fields() {
	local line
	line=$(grep -a "Start：回 restore_data" "$R/helper.log" 2>/dev/null | tail -1)
	RDLINE=$line
	RDV=$(echo "$line" | sed -n 's/.*vendor=\([^ ]*\) 版本=.*/\1/p')
	RDEV=$(echo "$line" | sed -n 's/.*版本=\([0-9]*\) 数据=.*/\1/p')
	RDD=$(echo "$line" | sed -n 's/.*数据=\(.*\)）.*/\1/p')
	RDCNT=$(grep -ac "Start：回 restore_data" "$R/helper.log" 2>/dev/null)
}
probe_size() { grep -ao "size': ([0-9-]*, [0-9-]*)" "$1" 2>/dev/null | head -1 | grep -o "[0-9-]*, [0-9-]*"; }
probe_pos() { grep -ao "position': ([0-9-]*, [0-9-]*)" "$1" 2>/dev/null | head -1 | grep -o "[0-9-]*, [0-9-]*"; }
last_source() { grep -a "要一路流" "$R/helper.log" 2>/dev/null | tail -1 | sed 's/^vidow-portal: //; s/: 要一路流.*//'; }

echo; echo "== 6f. 会话持久化（restore_data / persist_mode，接口 v4） =="

# 接口版本必须是 4：**只声明 4，不虚报 5/6**（v5 的 mapping_id、v6 的 pipewire-serial
# 是流属性，我们不在 Start 的 streams 里给，报了就是给消费者制造期待）。前端也是按这个
# 属性决定要不要把 persist_mode / restore_data 转给后端的。
VER=$(gdbus call --session --dest org.freedesktop.impl.portal.desktop.vidow \
	--object-path /org/freedesktop/portal/desktop \
	--method org.freedesktop.DBus.Properties.Get \
	org.freedesktop.impl.portal.ScreenCast version 2>/dev/null)
if echo "${VER:-}" | grep -q "uint32 4"; then
	pass "后端的 ScreenCast version = 4（v4 才有 persist_mode / restore_data）"
else
	fail "ScreenCast version 不是 4：${VER:-读不出来}"
fi

# --- 6f1 persist_mode=2 → Start 的 results 里要回一份 restore_data ----------
PICK0=$(picker_lines)
RDC0=$(grep -ac "Start：回 restore_data" "$R/helper.log" 2>/dev/null)
# 6f2 要比的是「6f1 弹完之后」的基线（6f1 自己就弹了一次）。

timeout 150 python3 "$R/persistprobe.py" --types 1 --persist 2 --timeout 60 \
	>"$R/p1.txt" 2>&1 &
if wait_and_share; then
	pass "6f1 带 persist_mode=2 的第一次请求，选择器弹出来了"
else
	fail "6f1 第一次没等到选择器：$(tail -3 "$R/p1.txt" | tr '\n' ' ')"
fi
NODE1=$(wait_node "$R/p1.txt")
if [ -n "${NODE1:-}" ]; then
	pass "6f1 第一次拿到了流（node=$NODE1）"
else
	fail "6f1 第一次没拿到流：$(tail -3 "$R/p1.txt" | tr '\n' ' ')"
fi
rd_fields
if [ "${RDV:-}" = "vidow" ] && [ "${RDEV:-}" = "1" ] && [ -n "${RDD:-}" ]; then
	pass "6f1 Start 的 results 里回了 restore_data（vendor=vidow 版本=1 数据=$RDD）"
else
	fail "6f1 Start 的 results 里没有合法的 restore_data：${RDLINE:-（没有那一行日志）}"
fi
if [ "${RDCNT:-0}" -gt "${RDC0:-0}" ]; then
	pass "6f1 日志里多了「Start：回 restore_data」这一行（就是判据 1 的证据）"
else
	fail "6f1 日志里没有「Start：回 restore_data」"
fi
case "${RDD:-}" in
monitor\|*\|1|monitor\|*\|2|monitor\|*\|4)
	pass "6f1 数据的形状是自描述字符串（$RDD = 源类型|标识符|光标模式）" ;;
*)
	fail "6f1 restore_data 的私有数据形状不对：${RDD:-空}" ;;
esac
SIZE1=$(probe_size "$R/p1.txt")
POS1=$(probe_pos "$R/p1.txt")
SRC1=$(last_source)
TOK1=$(grep -ao "restore_token=.*" "$R/p1.txt" 2>/dev/null | head -1 | cut -d= -f2-)
if [ -n "${TOK1:-}" ]; then
	pass "6f1 前端把那份 restore_data 换成了 restore_token 发给客户端（$TOK1）"
else
	fail "6f1 客户端没拿到 restore_token（前端没认我们回的 restore_data？）"
fi
PICK1=$(picker_lines)
echo "      （第一次：源=$SRC1 size=$SIZE1 pos=$POS1，数据=$RDD，token=$TOK1）"

# --- 6f2 同一个 app_id 再请求一次（客户端不带任何东西）→ 不弹选择器 --------
# 这一条是**真链路**的判据：客户端这次没有 restore_data 可带（前端也不让它带），
# 前端手里那份就是我们上一轮给的 —— 后端能跳过选择器，只可能是前端把它喂回来了。
if [ -n "${RDD:-}" ]; then
	timeout 120 python3 "$R/persistprobe.py" --types 1 --persist 2 \
		--restore-token "$TOK1" --timeout 30 >"$R/p2.txt" 2>&1 &
	PP=$!
	DLGSEEN=0
	for i in $(seq 1 60); do
		if $VM --socket "$IPC" windows 2>/dev/null | grep -aq "共享屏幕"; then
			DLGSEEN=1
			break
		fi
		kill -0 "$PP" 2>/dev/null || break
		sleep 0.2
	done
	wait "$PP"
	PICK2=$(picker_lines)
	if [ "$DLGSEEN" = 0 ]; then
		pass "6f2 第二次请求**没有**弹选择器窗口（前端把上一轮的 restore_data 喂回来了）"
	else
		fail "6f2 第二次请求还是弹了选择器"
	fi
	if [ "${PICK2:-0}" = "${PICK1:-0}" ]; then
		pass "6f2 后端日志里没有新的「Start，弹选择器」（$PICK1 → $PICK2）"
	else
		fail "6f2 后端又弹了一次选择器（「Start，弹选择器」$PICK1 → $PICK2）"
	fi
	if grep -aq "restore_data：判定合法" "$R/helper.log" &&
			grep -aq "restore_data：恢复成" "$R/helper.log"; then
		pass "6f2 日志：收到 → 判定合法 → 直接恢复（$(grep -a 'restore_data：恢复成' "$R/helper.log" | tail -1 | sed 's/^vidow-portal: //')）"
	else
		fail "6f2 日志里没有「判定合法 / 恢复成」：$(grep -a 'restore_data' "$R/helper.log" | tail -3 | tr '\n' ' ')"
	fi
	NODE2=$(wait_node "$R/p2.txt")
	if [ -n "${NODE2:-}" ]; then
		pass "6f2 恢复出来的流拿到了 node（node=$NODE2）"
	else
		fail "6f2 恢复出来的流没拿到 node：$(tail -3 "$R/p2.txt" | tr '\n' ' ')"
	fi
	SIZE2=$(probe_size "$R/p2.txt")
	POS2=$(probe_pos "$R/p2.txt")
	SRC2=$(last_source)
	if [ -n "${SIZE2:-}" ] && [ "${SIZE2:-}" = "${SIZE1:-}" ] && [ "${POS2:-}" = "${POS1:-}" ]; then
		pass "6f2 恢复的流指的还是同一块源（size=$SIZE2 pos=$POS2，第一次 $SIZE1/$POS1）"
	else
		fail "6f2 恢复的流不是同一块源：size/pos = ${SIZE2:-空}/${POS2:-空}，第一次是 ${SIZE1:-空}/${POS1:-空}"
	fi
	if [ -n "${SRC2:-}" ] && [ "${SRC2}" = "${SRC1:-}" ]; then
		pass "6f2 恢复的流是同一块输出（$SRC2 = $SRC1）"
	else
		fail "6f2 恢复的流换了一块输出：${SRC2:-空} vs ${SRC1:-空}"
	fi
	rd_fields
	if [ -n "${RDD:-}" ] && [ -n "${RDV:-}" ]; then
		pass "6f2 恢复之后又回了一份新的 restore_data（$RDD，前端会再换成新 token）"
	else
		fail "6f2 恢复之后没回新的 restore_data"
	fi
else
	fail "6f1 没拿到 restore_data，6f2 测不了"
fi

# --- 6f3 源没了：关掉那个窗口 → 回退弹选择器（不是报错、不崩） -------------
echo "      （6f3：窗口那条路 —— 录一个窗口，把它关掉，再让前端拿存下来的数据喂回来）"
python3 /home/busyo/Dev/Vidow/tools/pattern.py >"$R/pd1.log" 2>&1 &
WP1=$!
python3 /home/busyo/Dev/Vidow/tools/pattern.py >"$R/pd2.log" 2>&1 &
WP2=$!
for i in $(seq 1 20); do
	[ "$($VM --socket "$IPC" windows 2>/dev/null | grep -ac '"pattern"')" -ge 2 ] && break
	sleep 0.5
done
timeout 150 python3 "$R/persistprobe.py" --types 2 --persist 2 --timeout 60 \
	>"$R/p3.txt" 2>&1 &
if wait_and_share; then
	pass "6f3 窗口那轮（persist_mode=2）的选择器弹出来了"
else
	fail "6f3 窗口那轮没等到选择器"
fi
NODE3=$(wait_node "$R/p3.txt")
if [ -n "${NODE3:-}" ]; then
	pass "6f3 窗口那轮拿到了流（node=$NODE3）"
else
	fail "6f3 窗口那轮没拿到流：$(tail -3 "$R/p3.txt" | tr '\n' ' ')"
fi
rd_fields
RDW=${RDD:-}
TOK3=$(grep -ao "restore_token=.*" "$R/p3.txt" 2>/dev/null | head -1 | cut -d= -f2-)
case "${RDW:-}" in
window\|*\|1|window\|*\|2|window\|*\|4)
	pass "6f3 录了一个窗口，存的 restore_data 是它的 identifier（$RDW）" ;;
*)
	fail "6f3 没拿到窗口的 restore_data：${RDW:-空}" ;;
esac
# 关掉两个探针窗口，再开一个新的：新窗口的 identifier 和存下来的那个不一样 ——
# 这就是「拔了源」那条路（ext_foreign_toplevel 的 closed 是后端能看到的信号）。
kill $WP1 $WP2 2>/dev/null
for i in $(seq 1 30); do
	[ "$($VM --socket "$IPC" windows 2>/dev/null | grep -ac '"pattern"')" -eq 0 ] && break
	sleep 0.5
done
python3 /home/busyo/Dev/Vidow/tools/pattern.py >"$R/pd3.log" 2>&1 &
WP3=$!
for i in $(seq 1 20); do
	[ "$($VM --socket "$IPC" windows 2>/dev/null | grep -ac '"pattern"')" -ge 1 ] && break
	sleep 0.5
done
if [ -n "${RDW:-}" ] && [ -n "${TOK3:-}" ]; then
	PICK3=$(picker_lines)
	timeout 150 python3 "$R/persistprobe.py" --types 2 --persist 2 \
		--restore-token "$TOK3" --timeout 60 >"$R/p4.txt" 2>&1 &
	sleep 3
	DLG3=$(wait_dialog)
	if [ -n "${DLG3:-}" ]; then
		pass "6f3 源没了（那个窗口关了）→ 回退弹了选择器（对话框 $DLG3）"
	else
		fail "6f3 源没了却没弹选择器：$(tail -3 "$R/p4.txt" | tr '\n' ' ')"
	fi
	# 回退的原因有两条路都可能走到（都对，日志里会说清是哪条）：
	#   ① 后端手里那份窗口列表已经把那个窗口标成没了（ext_foreign_toplevel 的 closed，
	#      选择器也是按它过滤的）→ 还没建流就回退；
	#   ② 列表里那个窗口还挂着（合成器发 closed 的时机 / 顺序不一定）→ 建流被合成器
	#      拒掉（`没有这个窗口（它可能已经关了）`）→ 照样回退，不是回失败。
	if grep -aq "restore_data：窗口 .* 不在了，回退弹选择器" "$R/helper.log" 2>/dev/null ||
			grep -aq "restore_data：合成器不认这些源" "$R/helper.log" 2>/dev/null; then
		pass "6f3 日志说清了为什么回退（$(grep -a '回退弹选择器' "$R/helper.log" | tail -1 | sed 's/^vidow-portal: //')）"
	else
		fail "6f3 日志里没有「窗口 … 不在了 / 合成器不认这些源 → 回退」"
	fi
	if [ "${PICK3:-0}" -gt "${PICK2:-0}" ]; then
		pass "6f3 日志里「Start，弹选择器」多了一行（$PICK3 > $PICK2）"
	else
		fail "6f3 日志里没看到新的「Start，弹选择器」（$PICK2 → $PICK3）"
	fi
	# 把回退出来的选择器答掉（回车 = 共享第一行，这会儿列表里是新开的那个窗口）
	if [ -n "${DLG3:-}" ]; then
		sleep 1
		for i in $(seq 1 20); do
			$VM --socket "$IPC" send-key tap Return >/dev/null 2>&1
			sleep 0.5
			$VM --socket "$IPC" windows 2>/dev/null | grep -aq "共享屏幕" || break
		done
	fi
	for i in $(seq 1 30); do
		grep -aq "node=" "$R/p4.txt" 2>/dev/null && break
		sleep 1
	done
	NODE4=$(grep -ao "node=[0-9]*" "$R/p4.txt" 2>/dev/null | head -1 | cut -d= -f2)
	if [ -n "${NODE4:-}" ]; then
		pass "6f3 回退之后照常走完（新选择器选的那个窗口出流 node=$NODE4）"
	else
		fail "6f3 回退之后没出流：$(tail -3 "$R/p4.txt" | tr '\n' ' ')"
	fi
else
	fail "6f3 没拿到窗口的 restore_data，测不了"
fi
kill $WP3 2>/dev/null

# --- 6f4 垃圾 restore_data：忽略 + 弹选择器，不崩在 D-Bus 类型上 ----------
#   (a) 字符串级垃圾（错 vendor / 错版本 / 空数据 / 结构不对）：直接问后端，
#       每条都要**忽略 + 记原因**；
#   (b) 类型级垃圾（整条换成字符串 / (suu) / v 里是 uint32 / 前两段类型不对）：
#       前端会先把不对的类型挡掉，所以只能直接问后端 —— 「不崩在 D-Bus 类型上」
#       正是要验后端自己扛得住；
#   (c) 走一遍完整回退：CreateSession → SelectSources（垃圾）→ Start，垃圾必须被
#       忽略、**照常弹选择器**（不是报错）。
GARBAGE=0
garbage_case() { # <说明> <vendor> <版本> <数据>
	local vendor=$2 ver=$3 data=$4
	gdbus call --session --dest org.freedesktop.impl.portal.desktop.vidow \
		--object-path /org/freedesktop/portal/desktop \
		--method org.freedesktop.impl.portal.ScreenCast.SelectSources \
		"/org/freedesktop/portal/desktop/request/garbage$$" \
		"/org/freedesktop/portal/desktop/session/garbage$$" "garbage-app" \
		"{'types': <uint32 1>, 'persist_mode': <uint32 2>, 'restore_data': <('$vendor', uint32 $ver, <'$data'>)>}" \
		>/dev/null 2>&1 || GARBAGE=$((GARBAGE + 1))
}
BAD0=$(grep -ac "restore_data：判定非法" "$R/helper.log" 2>/dev/null)
garbage_case "乱写的 vendor" kde 1 "monitor|HEADLESS-1|1"
garbage_case "错版本号" vidow 2 "monitor|HEADLESS-1|1"
garbage_case "空数据" vidow 1 ""
garbage_case "结构不对（少一段）" vidow 1 "monitor|HEADLESS-1"
garbage_case "光标模式不认识" vidow 1 "monitor|HEADLESS-1|7"
BAD1=$(grep -ac "restore_data：判定非法" "$R/helper.log" 2>/dev/null)
if [ "$GARBAGE" = 0 ] && [ "${BAD1:-0}" -ge $(( ${BAD0:-0} + 5 )) ]; then
	pass "6f4 (a) 五种字符串级垃圾全被判非法忽略（「判定非法」$BAD0 → $BAD1）"
else
	fail "6f4 (a) 字符串级垃圾没处理干净（失败 $GARBAGE 次，「判定非法」$BAD0 → $BAD1）"
fi
if grep -aq "vendor 不是 vidow" "$R/helper.log" &&
		grep -aq "私有版本号不是 1" "$R/helper.log" &&
		grep -aq "数据是空的" "$R/helper.log" &&
		grep -aq "不是 kind|标识符|cursor_mode 三段" "$R/helper.log" &&
		grep -aq "光标模式不是 1/2/4" "$R/helper.log"; then
	pass "6f4 (a) 五种垃圾各自给了原因（错 vendor / 错版本 / 空 / 结构 / 光标模式）"
else
	fail "6f4 (a) 垃圾的原因没记全：$(grep -a '判定非法' "$R/helper.log" | tail -5 | tr '\n' ' ')"
fi
# (b) 类型级
TYPEBAD=0
type_garbage() { # <restore_data 的 GVariant 文本>
	gdbus call --session --dest org.freedesktop.impl.portal.desktop.vidow \
		--object-path /org/freedesktop/portal/desktop \
		--method org.freedesktop.impl.portal.ScreenCast.SelectSources \
		"/org/freedesktop/portal/desktop/request/tb$$" \
		"/org/freedesktop/portal/desktop/session/tb$$" "garbage-app" \
		"{'types': <uint32 1>, 'restore_data': $1}" >/dev/null 2>&1 || TYPEBAD=$((TYPEBAD + 1))
}
type_garbage "<'junk'>"
type_garbage "<('vidow', uint32 1, uint32 5)>"
type_garbage "<('vidow', uint32 1, <uint32 5>)>"
type_garbage "<(uint32 3, uint32 1, <'x'>)>"
type_garbage "<('vidow', uint32 1)>"
if [ "$TYPEBAD" = 0 ] && pgrep -x vidow-portal >/dev/null; then
	pass "6f4 (b) 五种类型级垃圾（字符串 / (suu) / v 里是 uint32 / 前两段类型不对 / 少一段）都没把后端打崩"
else
	fail "6f4 (b) 类型级垃圾把后端弄出问题了（失败 $TYPEBAD 次，进程 $(pgrep -x vidow-portal >/dev/null && echo 在 || echo 不在)）"
fi
if grep -aq "restore_data：收到 vendor=vidow 版本=1 数据=(v 里不是字符串)" "$R/helper.log"; then
	pass "6f4 (b) v 里不是字符串时安全拒掉（记了原因，没去解那个 variant）"
else
	fail "6f4 (b) v 里是 uint32 那条没记原因：$(grep -a 'restore_data：收到' "$R/helper.log" | tail -2 | tr '\n' ' ')"
fi
# (c) 完整的「垃圾 → 忽略 → 照常弹选择器」（直接问后端，走真的 CreateSession/Start）
GSESS=/org/freedesktop/portal/desktop/session/garbage$$/s1
GREQ=/org/freedesktop/portal/desktop/request/garbage$$/r1
gdbus call --session --dest org.freedesktop.impl.portal.desktop.vidow \
	--object-path /org/freedesktop/portal/desktop \
	--method org.freedesktop.impl.portal.ScreenCast.CreateSession \
	"$GREQ" "$GSESS" "garbage-app" "{'handle_token': <'r1'>}" \
	>/dev/null 2>&1
gdbus call --session --dest org.freedesktop.impl.portal.desktop.vidow \
	--object-path /org/freedesktop/portal/desktop \
	--method org.freedesktop.impl.portal.ScreenCast.SelectSources \
	"$GREQ" "$GSESS" "garbage-app" \
	"{'types': <uint32 1>, 'persist_mode': <uint32 2>, 'restore_data': <('vidow', uint32 1, <'monitor|HEADLESS-1'>)>}" \
	>/dev/null 2>&1
timeout 60 gdbus call --session --dest org.freedesktop.impl.portal.desktop.vidow \
	--object-path /org/freedesktop/portal/desktop \
	--method org.freedesktop.impl.portal.ScreenCast.Start \
	"$GREQ" "$GSESS" "garbage-app" "" "{'handle_token': <'r2'>}" \
	>"$R/g4.txt" 2>&1 &
SLEEP_DLG=$(wait_dialog)
if [ -n "${SLEEP_DLG:-}" ]; then
	pass "6f4 (c) 垃圾 restore_data 被忽略、**照常弹选择器**（对话框 $SLEEP_DLG）"
	$VM --socket "$IPC" send-key tap Escape >/dev/null 2>&1
else
	fail "6f4 (c) 垃圾 restore_data 之后没弹选择器：$(tail -2 "$R/g4.txt" 2>/dev/null | tr '\n' ' ')"
fi
sleep 1
if pgrep -x vidow-portal >/dev/null && ! grep -aqiE "Assertion|Aborted|core dump" "$R/vidow.log"; then
	pass "6f4 垃圾 restore_data 全流程走完，vidow-portal 活着、vidow 没崩"
else
	fail "6f4 垃圾 restore_data 这条路上出了崩 / 断言"
fi

# 这一节的边界（写清楚，别让人以为验了没验的）：
#   * 客户端看不到 restore_data / persist_mode（前端把它们吃了，只存权限库）—— 判据 1
#     的证据是**后端日志**里那份三元组；判据 2/3 是真链路（客户端第二次什么都没带）。
#   * app_id：这个 harness 里前端算出来的是**空串**（客户端是个普通 python 脚本，不在
#     systemd 的 app-*.scope 里），两次请求一样 —— 权限库按它存 / 取，所以能来回；
#     真机上 app_id 是应用自己的（firefox / OBS 那些）。
#   * **输出被拔掉**（wl_output 的 global 没了）那条路没测：后端现在只往 outputs 列表里
#     加、从没删过（registry_global_remove 是空的），所以「输出没了」它看不出来 —— 这是
#     这次没动的边界。窗口那条路有确切信号（ext_foreign_toplevel 的 closed），6f3 验的
#     就是它。

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
