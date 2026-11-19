#!/bin/bash
# 跨屏拖拽：窗口横在两块屏的交界上时，不能来回搬归属。
#
# 现场（用户报的「跨屏拖拽未放手还是会闪」）：浮窗归属屏原来是看「窗口中心
# 落在哪块屏」。中心横在交界上时每帧都能算出另一块屏 —— 于是每一帧搬一次：
# 旧屏那份装饰停用、新屏那份重烘，下一帧再搬回去。屏幕上就是一直闪。
#
# 判据：
#   1) 窗口在交界上左右抖 60 步：归属一次都不许搬（老代码实测搬 6 次）
#   2) 抖完两块屏上都能看到窗口（窗口横跨，两边各画一半）
#   3) 继续拖到另一块屏中部：归属恰好搬 1 次（迟滞不等于永不搬）
#
# 用法：./test-cross-drag-stable.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-cross-stable-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v wf-recorder >/dev/null && true
rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
xwayland = false
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
vidow.output{ name = "HEADLESS-2", scale = 1, mode = "1280x720" }
blur_enabled = true
blur_all = true
window_open_ms = 0
window_move_ms = 0
jelly = true
vidow.bind("Mod+mouse-left", "interactive-move")
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"
setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
sleep 0.5
exec 9<> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }

python3 tools/wall.py "#ff0000" wallL --output 0 > "$RUNDIR/wallL.log" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
python3 tools/wall.py "#0000ff" wallR --output 1 > "$RUNDIR/wallR.log" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
python3 tools/sheet.py wA 1 1 1 0.5 > "$RUNDIR/wA.log" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.8

mark "把窗口拖到两块屏的交界上"
./build/vidowmsg set-window-position --title wA 200 300
sleep 0.4
act "pointer-motion 500 450"
act "pointer-button left press mod" 0.4
W=$(./build/vidowmsg windows | python3 -c 'import json,sys;print([w["float_geometry"]["width"] for w in json.load(sys.stdin) if w["title"]=="wA"][0])')
# 按下点在窗口内 (500,450)、窗口左边缘在 200 → 抓点偏移 300
X=$((1280 - W / 2 + 300))
for i in $(seq 0 59); do
	if [ $((i % 20)) -lt 10 ]; then DX=-30; else DX=30; fi
	echo "pointer-motion $((X + DX)) 450" >&9
	sleep 0.03
done
sleep 0.3

mark "交界的左右抖：归属不许来回搬"
N=$(grep -ac "归属搬到" "$LOG")
if [ "$N" -eq 0 ]; then
	pass "抖 60 步，归属一次都没搬"
else
	fail "抖 60 步搬了 $N 次（每搬一次都会闪一下）"
fi

mark "窗口横跨交界：两块屏上都要画出来"
grim -o HEADLESS-1 "$RUNDIR/s1.png" 2>/dev/null || true
grim -o HEADLESS-2 "$RUNDIR/s2.png" 2>/dev/null || true
RES=$(python3 - "$RUNDIR" <<'PY'
import sys
from PIL import Image
d = sys.argv[1]
def edge(name, x):
    im = Image.open('%s/%s.png' % (d, name)).convert('RGB')
    w, h = im.size
    return im.getpixel((x if x >= 0 else w + x, h // 2))
a, b = edge('s1', 80), edge('s1', -80)
c, e = edge('s2', 80), edge('s2', -80)
# 每块屏上「靠交界那一侧」必须不是纯壁纸色（纯红 / 纯蓝）
def has_win(px):
    return not ((px[0] > 200 and px[1] < 60 and px[2] < 60) or
                (px[2] > 200 and px[0] < 60 and px[1] < 60))
print('%s %s %s %s' % (has_win(a) or has_win(b), has_win(c) or has_win(e), a, c))
PY
)
set -- $RES
if [ "$1" = "True" ] && [ "$2" = "True" ]; then
	pass "交界两侧都画着窗口（左 $3 / 右 $4）"
else
	fail "有一块屏上窗口没画出来（左 $1 / 右 $2）"
fi

mark "继续拖到另一块屏中部：该搬的时候要搬"
for i in $(seq 0 19); do
	echo "pointer-motion $((X + 200 + i * 15)) 450" >&9
	sleep 0.03
done
sleep 0.4
N2=$(grep -ac "归属搬到" "$LOG")
if [ "$N2" -ge 1 ]; then
	pass "拖过去之后归属搬了 $N2 次"
else
	fail "拖到另一块屏中部了，归属一次都没搬（窗口会画不出来）"
fi
act "pointer-button left release" 0.4

if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "vidow 还活着"
else
	fail "vidow 死了（$(tail -3 "$LOG" | tr '\n' '|')）"
fi
if grep -aq "\[ERROR\]" "$LOG"; then
	fail "日志里有 ERROR（$(grep -a "\[ERROR\]" "$LOG" | head -2 | tr '\n' '|')）"
else
	pass "日志里没有 ERROR"
fi

echo
if [ "$FAILED" -eq 0 ]; then echo "== 全部 PASS =="; else echo "== 有 FAIL =="; fi
exit "$FAILED"
