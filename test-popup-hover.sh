#!/usr/bin/env bash
# 真实复现：X11 弹出物（菜单）弹出来之后，把指针移到它上面，看它会不会被关掉。
#
# 为什么单独写一个：test-xwayland-popup.sh 只验「弹出来没」「像素对不对」，
# 完全不碰指针 —— 而用户报的是「鼠标移过去就关闭」。这条路径上真正干活的是
# 命中测试（vidow_view_at）+ 指针焦点切换（wlr_seat_pointer_notify_enter /
# clear_focus），和画出来没关系。
#
# 判据（三条都得成立）：
#   1. 弹出物弹出来了（日志有「归类为弹出物」或像素对）
#   2. 指针移到弹出物上之后，它**还在**（日志里没有 unmap / dissociate）
#   3. 指针停在弹出物上的那一帧，弹出物矩形还是它自己的颜色（没被关掉）
set -u

BIN=${VIDOW_BIN:-./build/vidow}
XPOPUP=${XPOPUP_BIN:-./build/xpopup}
RUNDIR=/tmp/vidow-popup-hover
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"

pass() { echo "  PASS  $*"; PASSED=$((PASSED + 1)); }
fail() { echo "  FAIL  $*"; FAILED=$((FAILED + 1)); }
mark() { echo; echo "== $* =="; }

cleanup() {
	if [ "${VIDOW_TEST_KEEP:-}" = "1" ]; then
		echo "（保留 $RUNDIR 供排查）"
		return
	fi
	rm -rf "$RUNDIR"
}
trap cleanup EXIT

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run" "$RUNDIR/shots"
chmod 700 "$RUNDIR/run"

cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = true
window_open_ms = 1
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=1
export WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR"
export VIDOW_LOG=debug
export VIDOW_X11_TRACE=1
export DISPLAY=
export WAYLAND_DISPLAY=
unset VIDOV_SOCKET

PASSED=0
FAILED=0

"$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!

for _ in $(seq 60); do
	grep -aq 'vidow ready' "$LOG" && break
	sleep 0.25
done
grep -aq 'vidow ready' "$LOG" || { echo "vidow 没起来"; exit 1; }

export DISPLAY
DISPLAY=$(grep -ao 'DISPLAY=:[0-9]*' "$LOG" | tail -1 | cut -d= -f2)
export WAYLAND_DISPLAY
WAYLAND_DISPLAY=$(grep -ao 'WAYLAND_DISPLAY=[^ ]*' "$LOG" | tail -1 |
	cut -d= -f2)
if [ -z "$DISPLAY" ]; then
	echo "拿不到 DISPLAY"; exit 1
fi

# 弹窗摆哪儿：给一块干净区域，父窗口在左上。
PX=200; PY=300; PW=200; PH=120
# 弹出物正中间（用来把指针挪上去）
CX=$((PX + PW / 2)); CY=$((PY + PH / 2))

mark "弹出物弹出来（menu，非 override-redirect）"
LINE=$(wc -l < "$LOG")
setsid "$XPOPUP" popup or "$PX" "$PY" "$PW" "$PH" fixed \
	> "$RUNDIR/popup.out" 2>&1 &
POPUP_PID=$!
sleep 2

if grep -aq '归类为弹出物\|弹出物' "$LOG"; then
	pass "日志里有弹出物"
else
	fail "日志里没有弹出物"
fi

mark "把指针移到弹出物正中间（$CX,$CY）"
# 记下行号：只检查「移动指针之后」这一段，别把测试收尾时的 kill 算进来
MOVE_LINE=$(wc -l < "$LOG")
# 分几步挪过去，模拟真实的鼠标移动（一步到位有时不触发 enter）
for step in 1 2 3 4 5; do
	mx=$((PX + PW * step / 10))
	my=$((PY + PH * step / 10))
	./build/vidowmsg move-cursor "$mx" "$my" > /dev/null 2>&1
	sleep 0.08
done
sleep 0.5

# 判据 2：指针移上去之后弹出物还在吗
if kill -0 "$POPUP_PID" 2>/dev/null; then
	pass "客户端进程还活着"
else
	fail "客户端进程退出了（弹出物被关掉了）"
fi

SINCE_MOVE=$(tail -n +$((MOVE_LINE + 1)) "$LOG")
if echo "$SINCE_MOVE" | grep -aq 'x11-life.*unmap\|x11-life.*dissociate'; then
	fail "移动指针之后弹出物被 unmap / dissociate 了（鼠标移上去就关）"
	echo "$SINCE_MOVE" | grep -a 'x11-life' | tail -3 | sed 's/^/         /'
else
	pass "移动指针之后弹出物还在（没有 unmap / dissociate）"
fi

# 判据 3：像素
mark "弹出物矩形还是它自己的颜色"
SHOT="$RUNDIR/shots/hover.png"
if command -v grim > /dev/null 2>&1; then
	grim -o HEADLESS-1 "$SHOT" 2>/dev/null ||
		grim "$SHOT" 2>/dev/null
fi
if [ -f "$SHOT" ]; then
	# 弹出物是红色 0xc00000，父窗口是蓝色 0x0000c0
	COLOR=$(python3 - "$SHOT" "$CX" "$CY" <<'PY'
import sys
try:
    from PIL import Image
except ImportError:
    print("nopil"); sys.exit()
img = Image.open(sys.argv[1]).convert("RGB")
x, y = int(sys.argv[2]), int(sys.argv[3])
print("%d,%d,%d" % img.getpixel((x, y)))
PY
)
	case "$COLOR" in
	192,0,0) pass "弹出物像素是它自己的颜色 ($COLOR)" ;;
	nopil) echo "  SKIP  没有 PIL，跳过像素判据" ;;
	*) fail "弹出物像素是 $COLOR，预期 192,0,0（红色）" ;;
	esac
else
	echo "  SKIP  没有 grim，跳过像素判据"
fi

kill "$POPUP_PID" 2>/dev/null
kill "$VIDOW_PID" 2>/dev/null
wait "$VIDOW_PID" 2>/dev/null

echo
echo "PASS: $PASSED  FAIL: $FAILED"
[ "$FAILED" -eq 0 ]
