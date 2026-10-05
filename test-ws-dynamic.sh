#!/bin/bash
# 动态工作区（niri 那套）：每块屏一份纵向栈，栈底永远留一块空的，中间空了切走就消失，
# 序号是位置、越界夹住，具名工作区永远在。
#
# 这几条都是「不看就不知道」的行为：栈底那块空的什么时候冒出来、什么时候收掉、
# 序号怎么算 —— 全是黑盒能验的（IPC 的 workspaces 就报 idx / name / named / window_count）。
#
# 判据：
#   1) 空配置起来：只有 1 块，是空的
#   2) 窗口开在唯一那块上 → 底下立刻补一块空的
#   3) 切到栈底那块再开一个窗口 → 又补一块（3 块：2 个有窗口 + 1 个空）
#   4) 空的、没名字的那块在工作区**中间**：切走就自动收掉
#   5) 具名工作区（配置里建的）空了也不消失，而且按声明顺序排在栈顶、一开始就亮着
#   6) focus-workspace 序号越界 → 夹到最后一块（不是「现建一块」）
#   7) 栈底那块是空的：切到它、再切回来，它还在（它是「新工作区」位）
#
# 用法：./test-ws-dynamic.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-wsdyn-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-ws-dynamic.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

run_case() {
	# $1 = 配置里的具名工作区声明（可空）
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
vidow.output{ name = "HEADLESS-1", scale = 1 }
scale = "1"
gaps = 10
window_move_ms = 0
$1
CFG
	export XDG_RUNTIME_DIR="$RUNDIR/run"
	export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
	export HOME="$CFGDIR" VIDOW_LOG=debug DISPLAY= WAYLAND_DISPLAY=
	BIN=${VIDOW_BIN:-./build/vidow}
	setsid "$BIN" > "$LOG" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
}

# 栈的样子：每块一行 "序号:名字(亮)(N窗)"，没名字写 "-"
stack() {
	./build/vidowmsg workspaces | python3 -c '
import sys, json
out = []
for w in json.load(sys.stdin):
    if w["special"]:
        continue
    out.append("%d:%s%s%s" % (w["idx"], w["name"] or "-",
                              "*" if w["active"] else "",
                              "(%d)" % w["window_count"] if w["window_count"] else ""))
print(" ".join(out))'
}

echo "== 动态增删（空配置：不预先建任何工作区）=="
run_case ""
S=$(stack)
[ "$S" = "1:-*" ] && pass "起来只有 1 块空的（$S）" || fail "起来该只有 1 块空的，实际 [$S]"

python3 ./tools/wall.py >/dev/null 2>&1 & CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.2
python3 ./tools/pattern.py w1 >/dev/null 2>&1 & CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.6
S=$(stack)
[ "$S" = "1:-*(1) 2:-" ] && pass "窗口落在唯一那块上 → 底下补一块空的（$S）" \
	|| fail "该是「1:-(1) 2:-*」，实际 [$S]"

# 切到栈底那块（序号 2）再开一个窗口 → 它占了栈底，于是又补一块
./build/vidowmsg focus-workspace 2 >/dev/null; sleep 0.5
python3 ./tools/pattern.py w2 >/dev/null 2>&1 & CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.6
S=$(stack)
[ "$S" = "1:-(1) 2:-*(1) 3:-" ] && pass "窗口开在栈底那块上 → 又补一块（$S）" \
	|| fail "该是「1:-(1) 2:-(1)* 3:-」，实际 [$S]"

# 序号越界：只有 3 块，focus-workspace 9 该夹到第 3 块
./build/vidowmsg focus-workspace 9 >/dev/null; sleep 0.5
S=$(stack)
[ "$S" = "1:-(1) 2:-(1) 3:-*" ] && pass "focus-workspace 9 夹到最后一块（$S）" \
	|| fail "越界没夹住，实际 [$S]"

# 关掉第 1 块上的窗口（它是「中间」那块、空的、没名字）→ 当场收掉，序号往前挪
./build/vidowmsg close-window --id "$(./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    if w["title"] == "w1":
        print(w["id"])')" >/dev/null 2>&1
sleep 1.0
S=$(stack)
[ "$S" = "1:-(1) 2:-*" ] && pass "关掉窗口后那块空的、没名字的工作区当场收掉，序号往前挪（$S）" \
	|| fail "该收掉中间那块空的并重排成「1:-(1) 2:-*」，实际 [$S]"
if grep -aq "收掉空工作区" "$LOG"; then
	pass "日志记下了那次收编（收掉空工作区）"
else
	fail "没有收掉空工作区的日志"
fi
kill -TERM -- "-$VIDOW_PID" 2>/dev/null; sleep 0.4; kill -KILL -- "-$VIDOW_PID" 2>/dev/null
VIDOW_PID=""


echo
echo "== 具名工作区：永远在、按声明顺序排栈顶、一开始就亮着 =="
run_case 'vidow.workspace_create("chat")'
S=$(stack)
[ "$S" = "1:chat* 2:-" ] && pass "具名工作区排栈顶、一开始就亮着（$S）" \
	|| fail "该是「1:chat* 2:-」，实际 [$S]"
python3 ./tools/wall.py >/dev/null 2>&1 & CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.0
python3 ./tools/pattern.py w1 >/dev/null 2>&1 & CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
# 把它搬走：具名工作区空了也不该消失
./build/vidowmsg move-window-to-workspace 2 >/dev/null 2>&1
sleep 0.8
S=$(stack)
case "$S" in
*chat*) pass "具名工作区空了也不消失（$S）" ;;
*)	fail "具名工作区被收掉了：[$S]" ;;
esac
./build/vidowmsg focus-workspace chat >/dev/null 2>&1
S=$(stack)
case "$S" in
*"chat*"*) pass "按名字切回具名工作区（$S）" ;;
*)	fail "按名字切不过去：[$S]" ;;
esac
kill -TERM -- "-$VIDOW_PID" 2>/dev/null; sleep 0.4; kill -KILL -- "-$VIDOW_PID" 2>/dev/null

echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-ws-dynamic.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
