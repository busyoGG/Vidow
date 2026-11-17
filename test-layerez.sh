#!/bin/bash
# 客户端改独占区（不重新 map）时，合成器必须重算可用区。
#
# 用户报：「偶尔会出现从全屏状态退出后，bar 显示了但工作区排除区域没有生效
# 的 bug」。真机上的 vshell 顶栏就是这么做的：进全屏时 exclusiveZone = 0（让
# 全屏窗口真铺满），退出全屏再变回 42 —— 表面一直是同一块，中间没有
# unmap / map。以前 vidow 只在 initial_commit 里调 vidow_arrange_layers()，
# 之后的变化没人管：usable_area 一直停在「整屏」，窗口于是铺到状态栏底下。
#
# 判据：
#   1) 独占区 0 的时候：usable_area 整屏不变
#   2) 客户端改成 42 之后（同一块表面、只重新 commit）：usable_area y=42、
#      height = H-42
#   3) 窗口跟着让位（tiled 窗口的 y 变成 42+gaps = 48）
#   4) 日志里有「独占区变了」那一行（说明走的是新判据，不是碰巧）
#   5) 日志无 assert / abort
#
# 用法：./test-layerez.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layerez-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "$(printenv VIDOW_TEST_KEEP)" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v python3 >/dev/null || { echo "test-layerez.sh: 需要 python3"; exit 1; }
for exe in ./build/vidow ./build/frames ./build/layerframes ./build/vidowmsg; do
	[ -x "$exe" ] || { echo "test-layerez.sh: 先 ninja -C build"; exit 1; }
done

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 80); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.2
done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "vidow 没起来"; tail -5 "$LOG"; exit 1
fi
for _ in $(seq 1 80); do
	WAYLAND_DISPLAY=$(ls "$RUNDIR/run" 2>/dev/null | grep -m1 '^wayland-[0-9]*$')
	[ -n "$WAYLAND_DISPLAY" ] && break
	sleep 0.1
done
export WAYLAND_DISPLAY
sleep 0.5

usable() {
	./build/vidowmsg state 2>/dev/null | python3 -c '
import json,sys
st=json.load(sys.stdin)
for o in st.get("outputs") or []:
    if str(o.get("name","")).startswith("HEADLESS"):
        u=o.get("usable_area") or {}
        print(u.get("x"), u.get("y"), u.get("width"), u.get("height"))
        raise SystemExit
print("-1 -1 -1 -1")'
}
win_y() {
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json,sys
try:
    ws=json.load(sys.stdin)
except Exception:
    print(-1); raise SystemExit
for w in ws:
    if not w.get("floating") and not w.get("fullscreen"):
        g = w.get("geometry") or {}
        print(g.get("y", -1)); raise SystemExit
print(-1)'
}

mark "1. 先开一个平铺窗口（量它让不让位）"
./build/frames "w1" 30 > "$RUNDIR/w1.log" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
set -- $(usable)
U0_Y=$2
U0_H=$4
W0_Y=$(win_y)
echo "     usable=$1,$2 $3 x $4   窗口 y=$W0_Y"

mark "2. 起一块顶栏（独占区 0），1.2s 之后改成 42（只重新 commit，不重新 map）"
VIDOW_TEST_LAYER_FRAMES_SIZE=1280x42 VIDOW_TEST_LAYER_ANCHOR=top \
VIDOW_TEST_LAYER_EZ=0 VIDOW_TEST_LAYER_EZ_AT_MS=1200:42 \
	./build/layerframes 30 > "$RUNDIR/bar.log" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 0.7
set -- $(usable)
B_Y=$2
B_H=$4
if [ "$B_Y" = "$U0_Y" ] && [ "$B_H" = "$U0_H" ]; then
	pass "独占区 0 的时候可用区没变（$1,$2 $3 x $4）"
else
	fail "独占区 0 的时候可用区就变了（$1,$2 $3 x $4，期望 y=$U0_Y h=$U0_H）"
fi

sleep 1.4
set -- $(usable)
A_X=$1
A_Y=$2
A_W=$3
A_H=$4
if [ "$A_Y" = "42" ] && [ "$A_H" = "678" ] && [ "$A_W" = "1280" ]; then
	pass "改成独占区 42 之后可用区重算了（$A_X,$A_Y $A_W x $A_H）"
else
	fail "改成独占区 42 之后可用区没跟上（$A_X,$A_Y $A_W x $A_H，期望 0,42 1280x678）"
fi
A_WIN_Y=$(win_y)
if [ "$A_WIN_Y" = "48" ]; then
	pass "窗口让位了（y=48 = 42 + gaps 6）"
else
	fail "窗口没让位（y=$A_WIN_Y，期望 48）"
fi

mark "3. 新判据真的跑了"
if grep -aq '独占区变了' "$LOG"; then
	pass "日志：$(grep -a '独占区变了' "$LOG" | tail -1 | sed -e 's/.*\] //')"
else
	fail "日志里没有「独占区变了」—— 走的不是新判据（可能是别的路径碰巧修好了）"
fi

mark "4. 日志干净"
grep -aqE 'assert|abort|Assertion' "$LOG" && fail "日志里有 assert/abort" \
	|| pass "日志里没有 assert / abort"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
