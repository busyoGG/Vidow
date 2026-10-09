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
# 双击直接确认），顺带把它钉住。第一张卡片的预览中心 = 对话框原点 +（108, 182）：
# 边框 12 + 标题一行 + 段落标题一行 + 预览 176x110 的中心。
DGEO=$(wait_dialog)
if [ -n "${DGEO:-}" ]; then
	# 对话框上屏之后还会被客户端「居中」一次（GTK 的 GTK_WIN_POS_CENTER_ALWAYS）：
	# 位置会从浮动默认位置挪到那块屏的中间（实测 1286 -> 1920）。点之前重新读一遍。
	sleep 1
	DGEO=$(wait_dialog)
	set -- $DGEO
	CARD_X=$(( $1 + 108 ))
	CARD_Y=$(( $2 + 182 ))
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
