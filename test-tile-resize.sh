#!/bin/bash
# 平铺窗口的 Mod+右键拖拽 = 改列宽（niri 的 interactive resize 那条）。
#
# 以前只有浮窗有「右键拖拽缩放」（Mod+右键 = 改浮窗自己的几何，见 float.c），
# 平铺窗口按 Mod+右键什么都不做 —— 用户报的「右键 resize 窗口的快捷方式没有绑定」。
#
# 判据：
#   1) 平铺窗口 Mod+右键按住拖：列宽**按光标位移**变（拖 +200 像素 → 列宽 +200）
#   2) 拖动期间 IPC 里 grab.mode = resize-column（拖起来了，不是点了没反应）
#   3) 松手之后宽度留着（不是拖完弹回去）
#   4) 往左拖就变窄
#   5) 两头都夹得住：拖到屏幕右边缘 → 比例 1.0；拖到左边缘 → 0.05（不越界）
#   6) 浮窗那条路没被抢：Mod+右键拖浮窗改的是它自己的几何，列宽一个字不动
#   7) 没崩、没断言
#
# 用法：./test-tile-resize.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-tile-resize-test
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
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in *tools/pattern.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/vidow ] || { echo "test-tile-resize.sh: 先构建（ninja -C build）"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-tile-resize.sh: 先构建（ninja -C build）"; exit 1; }
python3 -c 'import gi' 2>/dev/null || { echo "test-tile-resize.sh: 需要 python3-gobject"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
gaps = 10
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-tile-resize.sh: vidow 起不来"
	tail -5 "$LOG"
	exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }

python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do
	./build/vidowmsg windows 2>/dev/null | grep -q '"title": "w1"' && break
	sleep 0.25
done
sleep 1.0

# 聚焦那列的宽度比例（整数百分比，好比较）+ 像素宽。
colf() {
	./build/vidowmsg columns 2>/dev/null | python3 -c '
import sys, json
for c in json.load(sys.stdin):
    if c.get("windows"):
        print("%d %d" % (round(c["width_fraction"] * 100), c["width"]))
        break'
}
colfrac() { colf | awk '{print $1}'; }
colpx() { colf | awk '{print $2}'; }
# 窗口的几何（浮窗时用 float_geometry）。
geo() {
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    if w["title"] == "w1":
        g = w["float_geometry"] if w["floating"] else w["geometry"]
        print("%d %d %d %d" % (g["x"], g["y"], g["width"], g["height"]))
        break'
}
workw() { # 可用宽（列宽比例的换算基准）：拖到 100% 时那一列的像素宽
	./build/vidowmsg outputs 2>/dev/null | python3 -c '
import sys, json
for o in json.load(sys.stdin):
    if o.get("enabled"):
        print(o["logical_width"] - 20)   # 减去左右各 10 的 gaps
        break'
}

# 每次起拖前重算落点：窗口中心（列宽一改它就挪了）。
at_window() {
	set -- $(geo)
	CX=$(( $1 + $3 / 2 ))
	CY=$(( $2 + $4 / 2 ))
}

W0=$(colpx)
F0=$(colfrac)
GEO0=$(geo)
at_window
echo "      起手：列 ${W0}px（比例 ${F0}%），窗口 $GEO0"
if [ "${W0:-0}" -gt 100 ]; then
	pass "平铺窗口起手列宽 ${W0}px"
else
	fail "列宽读不出来（${W0:-空}）"
fi

# ---------------------------------------------------------------- 1) 往右拖
mark "1) Mod+右键按住往右拖：列宽按光标位移变"
act "pointer-motion $CX $CY"
act "pointer-button right press mod"
MID=$(./build/vidowmsg state 2>/dev/null | grep -ao 'resize-column' | head -1)
if [ -n "$MID" ]; then
	pass "按下之后 grab.mode = resize-column（拖起来了）"
else
	fail "按下之后没进改宽 grab（$(./build/vidowmsg state 2>/dev/null | grep -ao '"grab":{[^}]*}' | head -1)）"
fi
act "pointer-motion $((CX + 200)) $CY" 0.4
W1=$(colpx)
echo "      +200 之后列宽 ${W1}px"
if [ "${W1:-0}" = "$((W0 + 200))" ]; then
	pass "拖 +200 像素 → 列宽 ${W0} → ${W1}（正好 +200）"
else
	fail "拖 +200 像素列宽是 ${W1:-空}（期望 $((W0 + 200))）"
fi
act "pointer-motion $((CX + 400)) $CY" 0.4
W2=$(colpx)
echo "      +400 之后列宽 ${W2}px"
if [ "${W2:-0}" = "$((W0 + 400))" ]; then
	pass "再拖到 +400 → 列宽 ${W2}（还是跟手）"
else
	fail "拖到 +400 列宽是 ${W2:-空}（期望 $((W0 + 400))）"
fi
act "pointer-button right release mod" 0.5
if [ "$(colpx)" = "$W2" ]; then
	pass "松手之后宽度留着（${W2}px）"
else
	fail "松手之后宽度变成 $(colpx)（期望 ${W2}）"
fi
if grep -aq "grab resize: w1 起拖" "$LOG" && grep -aq "grab resize: w1 松手" "$LOG"; then
	pass "日志里起拖 / 松手都在（grab resize）"
else
	fail "日志里没有 grab resize：$(grep -a 'grab resize' "$LOG" | head -2 | tr '\n' ' ')"
fi

# ---------------------------------------------------------------- 2) 往左拖
mark "2) 往左拖：变窄"
at_window
act "pointer-motion $CX $CY"
act "pointer-button right press mod"
act "pointer-motion $((CX - 300)) $CY" 0.4
act "pointer-button right release mod" 0.5
W3=$(colpx)
echo "      -300 之后列宽 ${W3}px"
if [ "${W3:-0}" = "$((W2 - 300))" ]; then
	pass "拖 -300 像素 → 列宽 ${W2} → ${W3}（正好 -300）"
else
	fail "拖 -300 像素列宽是 ${W3:-空}（期望 $((W2 - 300))）"
fi

# ---------------------------------------------------------------- 3) 两头夹住
mark "3) 两头夹得住（1.0 / 0.05）"
at_window
act "pointer-motion $CX $CY"
act "pointer-button right press mod"
act "pointer-motion 1270 $CY" 0.4
act "pointer-motion 1275 $CY" 0.4
act "pointer-button right release mod" 0.5
FMAX=$(colfrac)
echo "      拖到屏幕右边缘：比例 ${FMAX}%（日志：$(grep -a 'grab resize' "$LOG" | tail -2 | sed 's/.*grab resize: //' | tr '\n' ' ')）"
if [ "${FMAX:-0}" = "100" ]; then
	pass "拖到右边缘夹在 1.0（比例 100%）"
else
	fail "拖到右边缘的比例是 ${FMAX:-空}（期望 100）"
fi
# 夹到 0.05 需要**足够的行程**：从窗口右边缘附近起拖，往左能拉出一千多像素
# （从窗口正中间起拖只有半屏可走，最多缩到 ~50%）。
set -- $(geo)
CX=$(( $1 + $3 - 20 ))
CY=$(( $2 + $4 / 2 ))
act "pointer-motion $CX $CY"
act "pointer-button right press mod"
act "pointer-motion 20 $CY" 0.4
act "pointer-motion 5 $CY" 0.4
act "pointer-button right release mod" 0.5
FMIN=$(colfrac)
echo "      拖到屏幕左边缘：比例 ${FMIN}%（日志：$(grep -a 'grab resize' "$LOG" | tail -2 | sed 's/.*grab resize: //' | tr '\n' ' ')）"
if [ "${FMIN:-0}" = "5" ]; then
	pass "拖到左边缘夹在 0.05（比例 5%）"
else
	fail "拖到左边缘的比例是 ${FMIN:-空}（期望 5）"
fi

# ---------------------------------------------------------------- 4) 浮窗那条路没被抢
mark "4) 浮窗：Mod+右键改的还是它自己的几何，列宽不动"
# 先把列宽放回一半：上一节把它夹到了 0.05，窗口只有 63 像素宽，而 pattern.py 的
# 最小宽度是 122 —— 那么窄的浮窗拖不动（客户端自己会顶住）。
./build/vidowmsg set-column-width 0.5 --title w1 >/dev/null 2>&1
sleep 0.5
./build/vidowmsg toggle-floating --title w1 >/dev/null 2>&1
sleep 0.6
FLOATW0=$(geo | awk '{print $3}')
F0=$(colfrac)
at_window
act "pointer-motion $CX $CY"
act "pointer-button right press mod"
act "pointer-motion $((CX - 150)) $CY" 0.4
act "pointer-button right release mod" 0.5
FLOATW1=$(geo | awk '{print $3}')
echo "      浮窗宽 ${FLOATW0} → ${FLOATW1}，列宽比例 ${F0} → $(colfrac)"
if [ "${FLOATW1:-0}" -lt "${FLOATW0:-0}" ]; then
	pass "浮窗被拖窄了（${FLOATW0} → ${FLOATW1}）"
else
	fail "浮窗没被拖窄（${FLOATW0} → ${FLOATW1}）"
fi
if [ "$(colfrac)" = "$F0" ]; then
	pass "列宽一个字没动（浮窗那条路没被平铺那条抢走）"
else
	fail "列宽被改了：${F0} → $(colfrac)"
fi

# ---------------------------------------------------------------- 5) 收尾
mark "5) 没崩、没断言"
if pgrep -x vidow >/dev/null; then
	pass "合成器还活着"
else
	fail "合成器挂了"
fi
if grep -aq "Assertion" "$LOG"; then
	fail "日志里有 assert"
else
	pass "日志里没有 assert"
fi

echo
if [ $FAILED -eq 0 ]; then
	echo "test-tile-resize.sh: OK"
else
	echo "test-tile-resize.sh: 有失败项"
fi
exit $FAILED
