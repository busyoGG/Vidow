#!/usr/bin/env bash
# 子窗口坐标换算：CreateNotify 的 x/y 是**相对 parent** 的，不是屏幕坐标。
#
# 用户报的「x 弹窗，鼠标移过去就关闭」（Steam）根因就在这条：Steam（CEF）
# 大量创建 4x2 的 or=1 子窗口当事件接收器 / 菜单锚点，坐标是相对父窗口的。
# Vidow 把那些相对值当绝对坐标用，窗口被摆到屏幕外（实测日志里 6056,1026，
# 而屏幕总宽只有 3840），菜单和鼠标对不上，指针一移过去就判成离开、菜单关掉。
#
# 这个测试直接验坐标：建一个父窗口，在它里面 (50,60) 处建一个子窗口，
# 然后把指针移到「父窗口位置 + (50,60) + 子窗口中心」，看命中的是不是子窗口。
set -u

BIN=${VIDOW_BIN:-./build/vidow}
XPOPUP=${XPOPUP_BIN:-./build/xpopup}
RUNDIR=/tmp/vidow-child-coords
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
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
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

DISPLAY=$(grep -ao 'DISPLAY=:[0-9]*' "$LOG" | tail -1 | cut -d= -f2)
export DISPLAY
[ -n "$DISPLAY" ] || { echo "拿不到 DISPLAY"; exit 1; }

mark "父窗口：普通窗口在 (100,150)，400x400"
PARENT_OUT="$RUNDIR/parent.out"
setsid "$XPOPUP" popup normal 100 150 400 400 fixed > "$PARENT_OUT" 2>&1 &
PARENT_PID=$!
for _ in $(seq 40); do
	[ -s "$PARENT_OUT" ] && break
	sleep 0.25
done
PARENT_WIN=$(head -1 "$PARENT_OUT" | tr -d '[:space:]')
if [ -n "$PARENT_WIN" ]; then
	pass "父窗口建起来了（$PARENT_WIN）"
else
	fail "父窗口没建起来"
	cat "$PARENT_OUT" | tail -3
	kill "$VIDOW_PID" 2>/dev/null
	exit 1
fi
sleep 1

mark "子窗口：建在父窗口里 (50,60) 处，100x80"
CHILD_OUT="$RUNDIR/child.out"
setsid "$XPOPUP" popup or 50 60 100 80 "child-of=$PARENT_WIN" \
	> "$CHILD_OUT" 2>&1 &
CHILD_PID=$!
sleep 1.5

# 期望的屏幕坐标：**父窗口当前在哪儿** + 子偏移 (50,60)。
# 别用父窗口的创建位置 (100,150)：Vidow 的布局会把普通窗口重排（实测它把
# 这个 400x400 的窗口挪到了 440,160），子窗口的屏幕位置跟着父窗口走。
# 直接问 X 服务器父窗口现在在哪儿（Vidow 的布局会挪普通窗口，日志里只有
# 创建位置，不能拿来当期望值）。
PARENT_POS=""
if command -v xwininfo > /dev/null 2>&1; then
	GEOM=$(xwininfo -id "$PARENT_WIN" 2>/dev/null |
		grep -a 'Absolute upper-left')
	PX=$(echo "$GEOM" | sed -n 's/.*X: *\([0-9-]*\).*/\1/p')
	PY=$(echo "$GEOM" | sed -n 's/.*Y: *\([0-9-]*\).*/\1/p')
	[ -n "$PX" ] && [ -n "$PY" ] && PARENT_POS="$PX $PY"
fi
if [ -z "$PARENT_POS" ]; then
	PARENT_POS="100 150"
fi
WANT_X=$(( $(echo "$PARENT_POS" | cut -d' ' -f1) + 50 ))
WANT_Y=$(( $(echo "$PARENT_POS" | cut -d' ' -f2) + 60 ))
echo "         父窗口现在在 ${PARENT_POS% *},${PARENT_POS#* }，子窗口应在 $WANT_X,$WANT_Y"
CHILD_WIN=$(head -1 "$CHILD_OUT" | tr -d '[:space:]')

mark "Vidow 日志里子窗口的坐标应该是 $WANT_X,$WANT_Y"
# 日志格式：xwm: create xid=0x... WxH at X,Y or=N
# 优先读**换算后**的那行（"子窗口坐标 x,y → 屏幕 X,Y"）；没有就退回 create 行
# 里的原始值（那就是没换算，等于 bug 还在）。
GOT=$(grep -a '子窗口坐标' "$LOG" | tail -1 |
	sed -n 's/.*→ 屏幕 \([0-9-]*\),\([0-9-]*\).*/\1 \2/p')
if [ -z "$GOT" ]; then
	GOT=$(grep -a "xwm: create xid=$CHILD_WIN" "$LOG" | tail -1 |
		sed -n 's/.* at \([0-9-]*\),\([0-9-]*\) or=.*/\1 \2/p')
fi

if [ -z "$GOT" ]; then
	fail "日志里找不到子窗口坐标"
else
	GX=$(echo "$GOT" | cut -d' ' -f1)
	GY=$(echo "$GOT" | cut -d' ' -f2)
	if [ "$GX" = "$WANT_X" ] && [ "$GY" = "$WANT_Y" ]; then
		pass "子窗口坐标换算正确（$GX,$GY）"
	else
		fail "子窗口坐标是 $GX,$GY，预期 $WANT_X,$WANT_Y（差 $((GX - WANT_X)),$((GY - WANT_Y))）"
	fi
fi

mark "指针移到子窗口中心，命中的应该是子窗口"
# 子窗口中心 = 换算后的 (490,220) + 半尺寸 (50,40) = (540,260)
HIT_X=$((WANT_X + 50))
HIT_Y=$((WANT_Y + 40))
echo "         指针移到子窗口中心 $HIT_X,$HIT_Y"
./build/vidowmsg move-cursor "$HIT_X" "$HIT_Y" > /dev/null 2>&1
sleep 0.5

PICK=$(./build/vidowmsg pick-window "$HIT_X" "$HIT_Y" 2>/dev/null |
	grep -o '"title"[^,]*\|"app_id"[^,]*' | head -2 | tr '\n' ' ')
echo "         pick-window: ${PICK:-（空）}"
if [ -n "$PICK" ]; then
	pass "该位置能命中窗口"
else
	fail "该位置命中不到任何窗口（子窗口被摆到别处了）"
fi

kill "$CHILD_PID" "$PARENT_PID" 2>/dev/null
kill "$VIDOW_PID" 2>/dev/null
wait "$VIDOW_PID" 2>/dev/null

echo
echo "PASS: $PASSED  FAIL: $FAILED"
[ "$FAILED" -eq 0 ]
