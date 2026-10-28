#!/usr/bin/env bash
# 真实 GTK 菜单复现：鼠标移到菜单上，菜单会不会被关掉。
#
# 为什么用 GTK 而不是自己写的 xpopup：GTK / Qt 的菜单是 **override-redirect
# + XGrabPointer** 的，指针抓取就是「菜单弹出来期间指针归我」的意思 —— 指针
# 移到菜单上、甚至移到菜单外面，X 客户端都还能收到事件、自己判断要不要关。
#
# Vidow 不处理 XCB_GRAB_POINTER / XCB_UNGRAB_POINTER（xwm_x11.c 的
# switch 里只有 7 种事件，没有这两个），所以抓取请求被丢掉。后果：指针一离开
# 父窗口的命中区，Vidow 就发 LeaveNotify，GTK 认为「点别处了」→ 关菜单。
# 用户报的「鼠标移过去就关闭」就是这条。
#
# 判据：菜单弹出来之后把指针移到菜单上，菜单**还在**。
set -u

BIN=${VIDOW_BIN:-./build/vidow}
RUNDIR=/tmp/vidow-gtk-menu
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

mark "起一个真实 GTK 窗口"
python3 /tmp/gtk-menu.py > "$RUNDIR/gtk.out" 2>&1 &
GTK_PID=$!
for _ in $(seq 40); do
	grep -aq '^ready' "$RUNDIR/gtk.out" && break
	sleep 0.25
done
if grep -aq '^ready' "$RUNDIR/gtk.out"; then
	pass "GTK 窗口起来了"
else
	fail "GTK 窗口没起来"
	cat "$RUNDIR/gtk.out" | tail -5
fi
sleep 1

mark "点一下窗口，弹出 GTK 菜单"
# 窗口是 400x300，点中间偏下一点
./build/vidowmsg move-cursor 200 200 > /dev/null 2>&1
sleep 0.2
./build/vidowmsg pointer-button click left > /dev/null 2>&1
sleep 1

if grep -aq '^menu-popped' "$RUNDIR/gtk.out"; then
	pass "菜单弹出来了"
else
	fail "菜单没弹出来"
fi

# 菜单弹出来之后，看有几个 X11 窗口（应该多一个 override-redirect 的）
mark "把指针移到菜单上（菜单在点击位置下方，约 210,215）"
BEFORE_LINE=$(wc -l < "$LOG")
for step in 1 2 3 4 5; do
	my=$((200 + 5 + step * 4))
	./build/vidowmsg move-cursor 215 "$my" > /dev/null 2>&1
	sleep 0.1
done
sleep 0.8

SINCE=$(tail -n +$((BEFORE_LINE + 1)) "$LOG")
if echo "$SINCE" | grep -aq 'x11-life.*unmap\|x11-life.*drop'; then
	fail "鼠标移上去之后菜单被 unmap / drop 了（就是用户报的「移过去就关闭」）"
	echo "$SINCE" | grep -a 'x11-life' | tail -3 | sed 's/^/         /'
else
	pass "鼠标移上去之后菜单还在"
fi

if kill -0 "$GTK_PID" 2>/dev/null; then
	pass "GTK 进程还活着"
else
	fail "GTK 进程退出了"
fi

kill "$GTK_PID" 2>/dev/null
kill "$VIDOW_PID" 2>/dev/null
wait "$VIDOW_PID" 2>/dev/null

echo
echo "PASS: $PASSED  FAIL: $FAILED"
[ "$FAILED" -eq 0 ]
