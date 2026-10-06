#!/bin/bash
# 抓帧遮挡：窗口规则 capture = "hide" 让窗口 / layer surface 在别人截图、录屏里
# 不出现，而**屏幕上照常显示**。
#
# 凭什么能做到「只从抓帧里消失」：vidow 自己实现输出抓帧（src/capture.c 的
# ext-image-copy-capture 输出源 + src/screencopy.c 的 zwlr_screencopy）—— 抓帧
# 那一帧是 vidow **自己渲染**的（渲染前把要藏的节点压成透明，见 capturemask.c），
# 屏幕上那一帧完全不受影响。wlroots 那两份都是把屏幕缓冲直接拷给客户端，所以以前
# 「抓帧 == 屏幕那一帧」，藏着的东西录屏一开始就会从屏幕上消失。
#
# 判据：
#   1) 规则读进来：vidowmsg rules 里 capture / capture_mosaic 字段对得上
#   2) ext-image-copy-capture（grim）：抓到的帧里没有它（0 个蓝像素）
#   3) 窗口照常活着、还能点：IPC 里 mapped/visible，pick-window 命中的还是它
#      （藏法是压透明度，不是关节点 —— 关节点会让窗口点不着）
#   4) 对照组 capture = "off"：同一个窗口照样进抓帧
#   5) 屏幕上还有它：嵌套跑（外层 vidow 当观察者，外层 grim 抓到的就是内层的屏幕）
#   6) **录屏期间**屏幕上照样有它（wf-recorder 走 screencopy，默认 copy_with_damage）
#      —— 这条是这次改动的核心
#   7) 录像文件里没有它（screencopy 那条路也藏住了）；-D（纯 copy）再验一遍
#   8) layer surface 的规则（namespace = "..."）：抓帧里没有，屏幕上还有
#   9) capture = "mosaic" 还没做 → 配置错误里说清楚（别静默当成 hide）
#  10) 日志里没有 assert / abort
#
# 用法：./test-capture-hide.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 bluewin/dot/wf-recorder/grim。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-capture-hide-test
FAILED=0
VIDOW_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*bluewin.py*|*tools/dot.py*|*wf-recorder*|*grim*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

cleanup() {
	stop_clients
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -TERM -- "-$pid" 2>/dev/null
	done
	sleep 0.4
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -KILL -- "-$pid" 2>/dev/null
	done
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v grim >/dev/null || { echo "test-capture-hide.sh: 需要 grim"; exit 1; }
command -v wf-recorder >/dev/null || { echo "test-capture-hide.sh: 需要 wf-recorder"; exit 1; }
command -v ffmpeg >/dev/null || { echo "test-capture-hide.sh: 需要 ffmpeg"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "test-capture-hide.sh: 需要 python3-PIL"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-capture-hide.sh: 先构建"; exit 1; }

# 像素判据：蓝窗口（tools/bluewin.py）的纯蓝，和 dot 壁纸的亮点（近白）。
pixels() { # <png> <blue|bright>
	python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
n = 0
for y in range(im.size[1]):
    for x in range(im.size[0]):
        r, g, b = px[x, y]
        if sys.argv[2] == "blue":
            if b > 180 and r < 100 and g < 100: n += 1
        else:
            if r > 200 and g > 200 and b > 200: n += 1
print(n)
PY
}
blue() { pixels "$1" blue; }
bright() { pixels "$1" bright; }

mkdir -p "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
export XDG_RUNTIME_DIR="$RUNDIR/run"
export DISPLAY= WAYLAND_DISPLAY= WLR_LIBINPUT_NO_DEVICES=1

# 单屏（headless）跑一段：写配置、起 vidow、返回 WAYLAND_DISPLAY。
start_headless() { # <home 目录> <配置内容> <日志文件>
	local home="$1" cfg="$2" log="$3"
	mkdir -p "$home/.config/vidow"
	printf '%s\n' "$cfg" > "$home/.config/vidow/config.lua"
	(
		export HOME="$home" VIDOW_CONFIG_LUA="$home/.config/vidow/config.lua"
		export VIDOW_LOG=debug WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1
		setsid ./build/vidow > "$log" 2>&1 &
		echo $! > "$log.pid"
	)
	local pid
	pid=$(cat "$log.pid")
	VIDOW_PIDS="$VIDOW_PIDS $pid"
	for _ in $(seq 1 60); do
		grep -aq "vidow ready" "$log" && break
		sleep 0.25
	done
	grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$log" | tail -1 | cut -d= -f2
}

msg() { ./build/vidowmsg "$@" 2>/dev/null; }

# ------------------------------------------------------------ 1) 规则读进来
mark "1. 规则解析 / vidowmsg rules"
H1="$RUNDIR/one"
WD=$(start_headless "$H1" 'scale = "1"
window_open_ms = 60
vidow.rule{ app_id = "bluewin", capture = "hide", capture_mosaic = 24 }' "$RUNDIR/one.log")
export WAYLAND_DISPLAY="$WD"
LOG="$RUNDIR/one.log"
[ -n "$WD" ] && pass "vidow 起来了（$WD）" || { fail "vidow 没起来"; exit 1; }
sleep 0.5
RULES=$(msg --raw rules)
echo "$RULES" | grep -q '"capture":"hide"' && pass "rules 里 capture = hide" \
	|| fail "rules 里没有 capture=hide：$RULES"
echo "$RULES" | grep -q '"capture_mosaic":24' && pass "capture_mosaic = 24 读进来了" \
	|| fail "capture_mosaic 没读进来：$RULES"

# --------------------------------------------------- 2) 3) 4) 抓帧里没有它
mark "2. grim（ext-image-copy-capture）抓到的帧里没有它"
HOME="$H1" python3 tools/bluewin.py > "$RUNDIR/blue.log" 2>&1 &
BPID=$!
for _ in $(seq 1 40); do
	grep -aq "place bluewin" "$LOG" && break
	sleep 0.25
done
PLACE=$(grep -a "place bluewin" "$LOG" | tail -1)
if [ -z "$PLACE" ]; then
	fail "bluewin 没起来"
else
	pass "窗口上屏：${PLACE##*place }"
	WX=$(echo "$PLACE" | sed 's/.*-> \([0-9-]*\),[0-9-]* .*/\1/')
	WY=$(echo "$PLACE" | sed 's/.*-> [0-9-]*,\([0-9-]*\) .*/\1/')
	sleep 0.6
	grim "$RUNDIR/hidden.png" >/dev/null 2>&1
	N=$(blue "$RUNDIR/hidden.png")
	[ "$N" = "0" ] && pass "抓帧里 0 个蓝像素" || fail "抓帧里有 $N 个蓝像素（没藏住）"

	mark "3. 窗口照常活着、还能点"
	WIN=$(msg --raw windows)
	echo "$WIN" | grep -q '"mapped":true' && pass "窗口还是 mapped" || fail "窗口不在：$WIN"
	echo "$WIN" | grep -q '"visible":true' && pass "窗口还是 visible（藏法是压透明度，不是关节点）" \
		|| fail "窗口 visible 不为 true：$WIN"
	AT=$(msg pick-window $((WX + 100)) $((WY + 100)))
	echo "$AT" | grep -q "bluewin" && pass "pick-window 命中的还是它（输入不受影响）" \
		|| fail "pick-window 没命中：$AT"

	mark "4. 对照组：capture = off 的窗口照样进抓帧"
	msg --raw rules >/dev/null
	sed -i 's/capture = "hide"/capture = "off"/' "$H1/.config/vidow/config.lua"
	for _ in $(seq 1 40); do
		grep -aq 'capture = "off"' "$LOG" && break
		sleep 0.25
	done
	sleep 0.6
	grim "$RUNDIR/shown.png" >/dev/null 2>&1
	N=$(blue "$RUNDIR/shown.png")
	[ "$N" -gt 0 ] && pass "改成 off 之后抓帧里又有它了（$N 个蓝像素）" \
		|| fail "capture = off 了还是没抓到它（$N 个蓝像素）—— 那 2) 说明不了什么"
fi
kill "$BPID" 2>/dev/null
kill -TERM -- "-$(cat "$RUNDIR/one.log.pid")" 2>/dev/null
sleep 0.4
kill -KILL -- "-$(cat "$RUNDIR/one.log.pid")" 2>/dev/null
sleep 0.3

# ------------------------------------------- 5) 6) 7) 嵌套：屏幕上还有它 / 录屏
mark "5. 屏幕上还有它（嵌套：外层 vidow 当观察者）"
HOUT="$RUNDIR/outer"
HIN="$RUNDIR/inner"
WDOUT=$(start_headless "$HOUT" 'scale = "1"' "$RUNDIR/outer.log")
mkdir -p "$HIN/.config/vidow"
printf 'scale = "1"\nvidow.rule{ app_id = "bluewin", capture = "hide" }\nvidow.rule{ namespace = "dot", capture = "hide" }\n' \
	> "$HIN/.config/vidow/config.lua"
(
	export HOME="$HIN" VIDOW_CONFIG_LUA="$HIN/.config/vidow/config.lua"
	export VIDOW_LOG=debug WLR_BACKENDS=wayland WAYLAND_DISPLAY="$WDOUT"
	setsid ./build/vidow > "$RUNDIR/inner.log" 2>&1 &
	echo $! > "$RUNDIR/inner.log.pid"
)
IPID=$(cat "$RUNDIR/inner.log.pid")
VIDOW_PIDS="$VIDOW_PIDS $IPID"
for _ in $(seq 1 60); do
	grep -aq "vidow ready" "$RUNDIR/inner.log" && break
	sleep 0.25
done
WDIN=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$RUNDIR/inner.log" | tail -1 | cut -d= -f2)
if [ -z "$WDIN" ]; then
	fail "内层 vidow 没起来"; tail -3 "$RUNDIR/inner.log"
else
	pass "内层 vidow 起来了（$WDIN，外层 $WDOUT）"
	WAYLAND_DISPLAY="$WDIN" HOME="$HIN" python3 tools/bluewin.py > "$RUNDIR/blue2.log" 2>&1 &
	BPID=$!
	for _ in $(seq 1 40); do
		grep -aq "place bluewin" "$RUNDIR/inner.log" && break
		sleep 0.25
	done
	sleep 1.0
	WAYLAND_DISPLAY="$WDIN" grim "$RUNDIR/inner.png" >/dev/null 2>&1
	WAYLAND_DISPLAY="$WDOUT" grim "$RUNDIR/outer.png" >/dev/null 2>&1
	NI=$(blue "$RUNDIR/inner.png")
	NO=$(blue "$RUNDIR/outer.png")
	[ "$NI" = "0" ] && pass "内层抓帧里没有它" || fail "内层抓帧里有 $NI 个蓝像素"
	[ "$NO" -gt 0 ] && pass "屏幕上还有它（外层抓到 $NO 个蓝像素）" \
		|| fail "屏幕上也没有它了（$NO 个蓝像素）—— 屏幕那一帧被动了"

	mark "6. 录屏期间屏幕上照样有它（wf-recorder / screencopy）"
	WAYLAND_DISPLAY="$WDIN" HOME="$HIN" setsid wf-recorder -c libx264 -p crf=28 \
		-f "$RUNDIR/rec.mkv" > "$RUNDIR/rec.log" 2>&1 &
	RPID=$!
	sleep 1.5
	WAYLAND_DISPLAY="$WDOUT" grim "$RUNDIR/outer-rec.png" >/dev/null 2>&1
	WAYLAND_DISPLAY="$WDIN" grim "$RUNDIR/inner-rec.png" >/dev/null 2>&1
	NO2=$(blue "$RUNDIR/outer-rec.png")
	NI2=$(blue "$RUNDIR/inner-rec.png")
	[ "$NO2" -gt 0 ] && pass "**录屏中**屏幕上还有它（$NO2 个蓝像素）" \
		|| fail "录屏一开始它就消失了（$NO2 个蓝像素）"
	[ "$NI2" = "0" ] && pass "录屏中另一路抓帧里也没有它" || fail "录屏中抓帧里有 $NI2 个蓝像素"
	kill -INT -- "-$RPID" 2>/dev/null
	sleep 2
	kill -TERM -- "-$RPID" 2>/dev/null
	sleep 0.5
	WAYLAND_DISPLAY="$WDIN" HOME="$HIN" setsid wf-recorder -D -c libx264 -p crf=28 \
		-f "$RUNDIR/rec-nodamage.mkv" > "$RUNDIR/rec2.log" 2>&1 &
	RPID2=$!
	sleep 1.5
	kill -INT -- "-$RPID2" 2>/dev/null
	sleep 2
	kill -TERM -- "-$RPID2" 2>/dev/null

	mark "7. 录像文件里没有它"
	check_rec() { # <文件> <说明>
		local f="$1" what="$2" dur n
		[ -s "$f" ] || { fail "$what：录像文件是空的"; return; }
		dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$f" 2>/dev/null)
		python3 - "$dur" <<'PY' && pass "$what：录到 ${dur}s" || fail "$what：时长不对（$dur）"
import sys
d = float(sys.argv[1] or 0)
sys.exit(0 if d > 0.5 else 1)
PY
		ffmpeg -v error -y -i "$f" -vf "select=eq(n\,10)" -frames:v 1 "$RUNDIR/frame.png" 2>/dev/null
		[ -s "$RUNDIR/frame.png" ] || { fail "$what：取不出第 10 帧"; return; }
		n=$(blue "$RUNDIR/frame.png")
		[ "$n" = "0" ] && pass "$what：第 10 帧里 0 个蓝像素" \
			|| fail "$what：第 10 帧里有 $n 个蓝像素"
	}
	check_rec "$RUNDIR/rec.mkv" "copy_with_damage（默认）"
	check_rec "$RUNDIR/rec-nodamage.mkv" "-D 纯 copy"

	mark "8. layer surface 的规则（namespace = \"dot\"）"
	WAYLAND_DISPLAY="$WDIN" HOME="$HIN" python3 tools/dot.py > "$RUNDIR/dot.log" 2>&1 &
	DPID=$!
	for _ in $(seq 1 40); do
		grep -aq "layer" "$RUNDIR/inner.log" && break
		sleep 0.25
	done
	sleep 1.0
	WAYLAND_DISPLAY="$WDIN" grim "$RUNDIR/layer-in.png" >/dev/null 2>&1
	WAYLAND_DISPLAY="$WDOUT" grim "$RUNDIR/layer-out.png" >/dev/null 2>&1
	BI=$(bright "$RUNDIR/layer-in.png")
	BO=$(bright "$RUNDIR/layer-out.png")
	[ "$BI" = "0" ] && pass "抓帧里没有 dot 壁纸的亮点（0 个）" \
		|| fail "抓帧里还有 $BI 个亮点（layer 规则没生效）"
	[ "$BO" -gt 0 ] && pass "屏幕上还有 dot 壁纸（$BO 个亮点）" \
		|| fail "屏幕上也没有 dot 壁纸了（$BO 个亮点）"
	kill "$DPID" 2>/dev/null
	kill "$BPID" 2>/dev/null
fi
kill -TERM -- "-$IPID" 2>/dev/null
sleep 0.4
kill -KILL -- "-$IPID" 2>/dev/null

# --------------------------------------------------- 9) mosaic 还没做的报错
mark "9. capture = \"mosaic\"：配置错误说清楚（不静默）"
H3="$RUNDIR/bad"
start_headless "$H3" 'scale = "1"
vidow.rule{ app_id = "bluewin", capture = "mosaic" }' "$RUNDIR/bad.log" >/dev/null
grep -aq 'capture 只能是 "off" 或 "hide"' "$RUNDIR/bad.log" \
	&& pass "配置错误里点了 capture 的合法值" || fail "配置错误没说清楚"
grep -aq '"mosaic" 还没做' "$RUNDIR/bad.log" \
	&& pass "并且说明 mosaic 还没做" || fail "没说 mosaic 还没做"

# --------------------------------------------------------------- 10) 日志
mark "10. 日志干净"
for f in "$RUNDIR/one.log" "$RUNDIR/inner.log" "$RUNDIR/bad.log"; do
	[ -f "$f" ] || continue
	if grep -aqE "assert|abort|Assertion" "$f"; then
		fail "$(basename "$f") 里有 assert / abort：$(grep -aE 'assert|abort|Assertion' "$f" | tail -1)"
	else
		pass "$(basename "$f") 里没有 assert / abort"
	fi
done

echo
[ "$FAILED" = "0" ] && echo "全部通过" || echo "有失败项"
exit $FAILED
