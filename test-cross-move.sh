#!/bin/bash
# 跨屏挪窗口：窗口要跟着搬过去，底衬也要采**新屏**的。
#
# 现场（用户报的「跨屏拖拽 跨过去之后采样会出问题（还是采样旧屏幕）」）：
# 浮窗「住在哪块屏」是由它的工作区决定的（view_home_output = 窗口 -> 工作区
# -> 屏）。只改几何不改归属，窗口仍旧挂在原来那块屏的工作区上，可见性判据
# 一算就是「停在屏外」—— 屏幕上开始什么都不画，底衬也还按旧屏采。
#
# 判据（两屏不同壁纸：左红右蓝，半透明白窗）：
#   1) 挪之前：窗口在左屏，屏内能看到窗口
#   2) 挪之后：窗口的 output 变成右屏（归属真的搬了）
#   3) 挪之后：右屏上能看到窗口（不是凭空消失）
#   4) 挪之后：窗口内部的底衬是**蓝**的（新屏），不是红的（旧屏）
#
# 用法：./test-cross-move.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-cross-move-test
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

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-cross-move.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1 }
vidow.output{ name = "HEADLESS-2", scale = 1 }
blur_enabled = true
blur_all = true
blur_levels = 4
blur_offset = 1.5
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
vidow.bind("Mod+mouse-left", "interactive-move")
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
# 指针注入：headless 里没有真鼠标，用 VIDOW_TEST_ACTION 的 FIFO 形式发
# （和 test-drag.sh 同一套）。跨屏拖拽那条路只能这么验。
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

mark "两块屏：左红右蓝，半透明白窗"
setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.8
exec 9> "$RUNDIR/actions"

# 布局顺序由合成器自动排：先起的那块在 0,0，第二块在它右边。
LNAME=$(grep -a "output HEADLESS-" "$LOG" | grep -a "布局 0,0" |
	sed 's/.*output \(HEADLESS-[0-9]*\).*/\1/' | head -1)
RNAME=$(grep -a "output HEADLESS-" "$LOG" | grep -a "布局 1280,0" |
	sed 's/.*output \(HEADLESS-[0-9]*\).*/\1/' | head -1)
if [ -z "$LNAME" ] || [ -z "$RNAME" ]; then
	fail "读不出两块屏的布局（左=$LNAME 右=$RNAME）"
	tail -5 "$LOG"
	exit "$FAILED"
fi
pass "左屏 $LNAME、右屏 $RNAME（自动排）"

# 壁纸按屏名上色：--output 认的是监控的序号，不是名字。
python3 ./tools/wall.py "#ff0000" wallL --output 0 > "$RUNDIR/wallL.log" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
python3 ./tools/wall.py "#0000ff" wallR --output 1 > "$RUNDIR/wallR.log" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.0
python3 ./tools/sheet.py wA 1 1 1 0.5 > "$RUNDIR/wA.log" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5

act() { echo "$1" >&9; sleep "${2:-0.3}"; }

win_out() {
	./build/vidowmsg windows --title wA | python3 -c '
import sys, json
w = json.load(sys.stdin)
if isinstance(w, list):
    w = w[0]
print(w.get("output") or "")'
}
win_geom() {
	./build/vidowmsg windows --title wA | python3 -c '
import sys, json
w = json.load(sys.stdin)
if isinstance(w, list):
    w = w[0]
g = w.get("float_geometry") or {}
print("%d %d %d %d" % (g.get("x", 0), g.get("y", 0),
                       g.get("width", 0), g.get("height", 0)))'
}

# 抓一张指定屏，并报窗口中心那个点的颜色。
#   sample <输出名> <png> <局部 x> <局部 y>
sample() {
	grim -o "$1" "$2" 2>/dev/null || { echo "none"; return 1; }
	python3 - "$2" "$3" "$4" <<'PY'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
x, y = int(sys.argv[2]), int(sys.argv[3])
if not (0 <= x < im.size[0] and 0 <= y < im.size[1]):
    print("outside")
    raise SystemExit(1)
print("%d,%d,%d" % im.getpixel((x, y)))
PY
}

# 等某个点真的不是纯壁纸色（等合成器把那一帧画出来，最多 4 秒）。
#   wait_visible <输出名> <png> <x> <y>
wait_visible() {
	local i up to t
	for i in $(seq 1 40); do
		to=$(sample "$1" "$2" "$3" "$4")
		case "$to" in
		none|outside) sleep 0.1; continue ;;
		esac
		r=$(echo "$to" | cut -d, -f1)
		if [ "$r" -gt 100 ]; then
			echo "$to"
			return 0
		fi
		sleep 0.1
	done
	echo "$to"
	return 1
}

mark "1：挪之前，窗口在左屏"
OUT1=$(win_out)
read -r GX GY GW GH <<< "$(win_geom)"
if [ "$OUT1" = "$LNAME" ]; then
	pass "窗口归左屏（$OUT1）"
else
	fail "窗口归 $OUT1，不是左屏 $LNAME"
fi
PX=$((GX + GW / 2))
PY_=$((GY + GH / 4))
COL=$(wait_visible "$LNAME" "$RUNDIR/before.png" "$PX" "$PY_") &&
	pass "左屏上看得见窗口（($PX,$PY_)=$COL）" ||
	fail "左屏上看不见窗口（($PX,$PY_)=$COL）"

mark "2：挪到右屏（几何越过 1280 边界）"
./build/vidowmsg set-window-position --title wA 1500 100
sleep 1.5
OUT2=$(win_out)
read -r GX GY GW GH <<< "$(win_geom)"
if [ "$OUT2" = "$RNAME" ]; then
	pass "归属跟着搬到右屏（$OUT2）"
else
	fail "归属还是 $OUT2，没搬到右屏 $RNAME —— 窗口会停在旧屏的「屏外」判定里"
fi
# 右屏原点 1280：窗口在屏内是 x - 1280
PX=$((GX - 1280 + GW / 2))
PY_=$((GY + GH / 4))
COL=$(wait_visible "$RNAME" "$RUNDIR/after.png" "$PX" "$PY_")
if [ $? -eq 0 ]; then
	pass "右屏上看得见窗口（($PX,$PY_)=$COL）"
else
	fail "右屏上看不见窗口（($PX,$PY_)=$COL）"
	LEFT=$(sample "$LNAME" "$RUNDIR/after-left.png" "$PX" "$PY_")
	fail "（参考）左屏同一个点 = $LEFT"
fi
# 底衬必须来自右屏（蓝），不是左屏（红）
PY2=$((GY + GH / 2))
COL2=$(sample "$RNAME" "$RUNDIR/after.png" "$PX" "$PY2")
R=$(echo "$COL2" | cut -d, -f1)
B=$(echo "$COL2" | cut -d, -f3)
case "$COL2" in
none|outside) fail "右屏截图拿不到（$COL2）" ;;
*)
	if [ "$B" -gt $((R + 40)) ]; then
		pass "窗口里的底衬是右屏的蓝（($PX,$PY2)=$COL2）"
	else
		fail "窗口里的底衬不是右屏的蓝（($PX,$PY2)=$COL2，要 B > R+40）"
	fi
	;;
esac

mark "2b：真的用鼠标从右屏拖回左屏（跨屏拖拽那条路）"
FL=$(./build/vidowmsg windows --title wA | python3 -c '
import sys, json
w = json.load(sys.stdin)
if isinstance(w, list):
    w = w[0]
print(1 if w.get("floating") else 0)')
if [ "$FL" = "1" ]; then
	pass "窗口是浮的（拖动才有效）"
else
	./build/vidowmsg toggle-floating --title wA >/dev/null
	sleep 0.5
	pass "把窗口切成浮窗再拖"
fi
read -r GX GY GW GH <<< "$(win_geom)"
act "pointer-motion $((GX + 200)) $((GY + 200))"
act "pointer-button left press mod"
act "pointer-motion $((GX - 200)) $((GY + 200))" 0.25
act "pointer-motion 900 $((GY + 200))" 0.25
act "pointer-motion 520 $((GY + 200))" 0.25
act "pointer-button left release mod" 0.8
OUT3=$(win_out)
if [ "$OUT3" = "$LNAME" ]; then
	pass "拖回左屏 → 归属跟着变回 $LNAME"
else
	fail "拖回左屏后归属还是 $OUT3（应该是 $LNAME）"
fi
read -r GX3 GY3 GW3 GH3 <<< "$(win_geom)"
PX3=$((GX3 + GW3 / 2))
PY3=$((GY3 + GH3 / 4))
# 左屏底色是纯红：窗口是半透明白，压上去绿色通道必然抬起来。
COL3=""
for _i in $(seq 1 40); do
	COL3=$(sample "$LNAME" "$RUNDIR/after-drag.png" "$PX3" "$PY3")
	case "$COL3" in
	none|outside) sleep 0.1; continue ;;
	esac
	case "$COL3" in
	none|outside) sleep 0.1; continue ;;
	esac
	G3=$(echo "$COL3" | cut -d, -f2)
	[ "$G3" -gt 40 ] && break
	sleep 0.1
done
G3=$(echo "$COL3" | cut -d, -f2)
if [ "${G3:-0}" -gt 40 ]; then
	pass "拖完左屏上看得见窗口（($PX3,$PY3)=$COL3）"
else
	fail "拖完左屏上看不见窗口（($PX3,$PY3)=$COL3）—— 跨屏拖拽之后窗口没画出来"
fi

mark "3：vidow 全程活着"
if { [ -n "$VIDOW_PGID" ] && kill -0 -- "-$VIDOW_PGID" 2>/dev/null; }; then
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
if [ "$FAILED" -eq 0 ]; then
	echo "== 全部 PASS =="
else
	echo "== 有 FAIL =="
fi
exit "$FAILED"
