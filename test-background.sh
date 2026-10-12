#!/bin/bash
# 桌面背景（壁纸）归谁画。
#
# vidow **不画**壁纸：壁纸永远是 layer-shell 的 background 层客户端画的
# （swaybg / swww / quickshell 都走这条路）。vidow 这边只有两件事：
#   1) background_color：谁都没画的时候垫在最底下的纯色（别是一块死黑）
#   2) 把「谁在画壁纸」报出来（vidowmsg background）+ 变了就发 background-changed
#
# 判据（像素 + IPC）：
#   1) 没有壁纸客户端：整屏就是 background_color；查询报 clients=0、covered=false
#   2) 起一个 background 层的客户端（tools/wall.py）：查询报 clients=1、那屏
#      covered=true、surface 的 namespace/layer/几何都对；画面被壁纸盖住
#   3) 客户端退出：画面回到 background_color、covered 又变 false
#   4) background-changed：上屏 / 下屏各发一次，载荷是背景状态（带 clients）
#   5) background_color 能热改（set-config 之后立刻换色，不用重启）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-background-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "${WATCH_PID:-}" ] && kill "$WATCH_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v grim >/dev/null || { echo "test-background.sh: 需要 grim"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-background.sh: 需要 python3-gobject"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
window_open_ms = 150
background_color = "#ff00ff"
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY=

rm -f "$LOG"
setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-background.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
VM="./build/vidowmsg"

bg() { $VM background 2>/dev/null; }
field() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
center() { python3 -c "
import sys
from PIL import Image
print(Image.open(sys.argv[1]).convert('RGB').getpixel((640, 360)))" "$1"; }

# 订阅 background-changed（原始信封，方便直接 grep）
( $VM --raw watch background-changed > "$RUNDIR/watch.log" 2>&1 &
	echo $! > "$RUNDIR/watch.pid" )
WATCH_PID=$(cat "$RUNDIR/watch.pid")
sleep 0.8

mark "没有壁纸客户端：整屏就是 background_color"
grim "$RUNDIR/no-wall.png"
C=$(center "$RUNDIR/no-wall.png")
[ "$C" = "(255, 0, 255)" ] &&
	pass "画面就是 background_color（中心 $C）" ||
	fail "中心是 $C，不是 background_color #ff00ff"
bg > "$RUNDIR/bg0.json"
[ "$(field "d['color']" < "$RUNDIR/bg0.json")" = "#ff00ffff" ] &&
	pass "查询报出 background_color：#ff00ffff" ||
	fail "查询报的 color 不对：$(field "d['color']" < "$RUNDIR/bg0.json")"
[ "$(field "d['clients']" < "$RUNDIR/bg0.json")" = "0" ] &&
	pass "查询报 clients=0（没人画壁纸）" ||
	fail "clients 不是 0：$(field "d['clients']" < "$RUNDIR/bg0.json")"
[ "$(field "d['outputs'][0]['covered']" < "$RUNDIR/bg0.json")" = "False" ] &&
	pass "查询报这块屏 covered=false" ||
	fail "covered 不是 false"

mark "起一个 background 层的客户端（tools/wall.py）"
setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map wall" "$LOG" && break; sleep 0.25; done
sleep 1.2
grim "$RUNDIR/wall.png"
C=$(center "$RUNDIR/wall.png")
[ "$C" != "(255, 0, 255)" ] &&
	pass "壁纸盖住了兜底色（中心 $C）" ||
	fail "壁纸没画上来，中心还是 $C"
bg > "$RUNDIR/bg1.json"
[ "$(field "d['clients']" < "$RUNDIR/bg1.json")" = "1" ] &&
	pass "查询报 clients=1" ||
	fail "clients 不是 1：$(field "d['clients']" < "$RUNDIR/bg1.json")"
[ "$(field "d['outputs'][0]['covered']" < "$RUNDIR/bg1.json")" = "True" ] &&
	pass "查询报这块屏 covered=true（有人铺满了）" ||
	fail "covered 不是 true"
[ "$(field "d['surfaces'][0]['namespace']" < "$RUNDIR/bg1.json")" = "wall" ] &&
	pass "surface 的 namespace = wall" ||
	fail "namespace 不对"
[ "$(field "d['surfaces'][0]['layer']" < "$RUNDIR/bg1.json")" = "background" ] &&
	pass "它在 background 层" ||
	fail "layer 不是 background"
[ "$(field "d['surfaces'][0]['geometry']['width']" < "$RUNDIR/bg1.json")" = "1280" ] &&
	pass "几何铺满整块屏（1280x720）" ||
	fail "几何不对：$(field "d['surfaces'][0]['geometry']" < "$RUNDIR/bg1.json")"

mark "壁纸客户端退出：又回到兜底色"
for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
CLIENT_PIDS=""
sleep 1.0
grim "$RUNDIR/no-wall2.png"
C=$(center "$RUNDIR/no-wall2.png")
[ "$C" = "(255, 0, 255)" ] &&
	pass "画面回到 background_color（中心 $C）" ||
	fail "中心是 $C，不是 #ff00ff"
bg > "$RUNDIR/bg2.json"
[ "$(field "d['clients']" < "$RUNDIR/bg2.json")" = "0" ] &&
	pass "查询报 clients=0" ||
	fail "clients 不是 0"
[ "$(field "d['outputs'][0]['covered']" < "$RUNDIR/bg2.json")" = "False" ] &&
	pass "covered 又变回 false" ||
	fail "covered 不是 false"

mark "background-changed 事件"
sleep 0.5
N=$(grep -ac '"event":"background-changed"' "$RUNDIR/watch.log")
[ "$N" -ge 2 ] &&
	pass "上屏 / 下屏各发了一次（收到 $N 条）" ||
	fail "只收到 $N 条 background-changed"
grep -aq '"clients":1' "$RUNDIR/watch.log" &&
	pass "载荷是背景状态（带 clients）而不是单个 surface" ||
	fail "载荷里没有 clients 字段：$(head -c 200 "$RUNDIR/watch.log")"
kill "$WATCH_PID" 2>/dev/null
WATCH_PID=""

mark "background_color 能热改"
$VM set-config background_color "#00ff00" >/dev/null 2>&1
sleep 0.6
grim "$RUNDIR/green.png"
C=$(center "$RUNDIR/green.png")
[ "$C" = "(0, 255, 0)" ] &&
	pass "set-config 之后立刻换成绿色（中心 $C）" ||
	fail "中心是 $C，不是 #00ff00"
bg > "$RUNDIR/bg3.json"
[ "$(field "d['color']" < "$RUNDIR/bg3.json")" = "#00ff00ff" ] &&
	pass "查询跟着变：#00ff00ff" ||
	fail "查询还是 $(field "d['color']" < "$RUNDIR/bg3.json")"
$VM set-config background_color "#00000000" >/dev/null 2>&1
sleep 0.5
bg > "$RUNDIR/bg4.json"
[ "$(field "d['color']" < "$RUNDIR/bg4.json")" = "#00000000" ] &&
	pass "alpha 0 也认（#00000000 = 完全不画底）" ||
	fail "alpha 0 没认：$(field "d['color']" < "$RUNDIR/bg4.json")"

mark "收尾"
grep -aq "Assertion" "$LOG" && fail "日志里有 assert" || pass "日志里没有 assert"
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.5
kill -0 "$VIDOW_PGID" 2>/dev/null && fail "合成器没退" || pass "合成器收干净了"
VIDOW_PGID=""

echo
if [ $FAILED -eq 0 ]; then
	echo "test-background.sh: OK"
else
	echo "test-background.sh: 有失败项"
fi
exit $FAILED
