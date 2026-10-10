#!/bin/bash
# 平铺窗口拖拽的落点：**插进已有列**（一列里上下叠起来）+ 落点预览块。
#
# niri 的落点只有两种（src/layout/scrolling.rs 的 insert_position）：
#   横向：光标离哪条列的**左边缘**最近 → 在那儿新开一条列（NewColumn）；
#   纵向：光标离某条列里哪个**窗口缝**最近 → 插进那条列的那一位（InColumn）；
#   两个距离谁小听谁的。这就是「拖到窗口身上 = 并进它那条列」那条手感。
#
# 这个测试盯三件事：
#   1) 贴目标列的**上边缘** → 插进那条列、成为最上面那个（高度对半劈）；
#   2) 贴目标列的**下边缘** → 插进那条列的最下面（三个窗口时三等分）；
#   3) 贴列带的**缝**（左边）→ 还是新开一条列（niri 的 NewColumn，老行为不许坏）。
# 外加：拖的过程里屏幕上真画着那块半透明预览（量像素），松手就收掉。
#
# 怎么注入鼠标：headless 里没有真鼠标，用 `VIDOW_TEST_ACTION` 的 FIFO 形式发
#   pointer-motion x y / pointer-button left press|release mod
# 走的是和真实输入完全同一条路（vidow_pointer_motion / vidow_pointer_button）。
#
# 坐标都是算出来的：一块 1280x720 的屏、gaps 10 → 可用区 10,10 1260x700；
# 每条列 0.3 → 378 宽，三条列就是 10 / 398 / 786，不滚动（视口恒为 0）。
#
# 用法：./test-drag-insert.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-drag-insert-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
SHOT="$RUNDIR/shots"
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
	{ echo "test-drag-insert.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }
python3 -c "import numpy, PIL" 2>/dev/null ||
	{ echo "test-drag-insert.sh: 需要 python3-numpy + pillow"; exit 1; }
command -v grim >/dev/null ||
	{ echo "test-drag-insert.sh: 需要 grim"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run" "$SHOT"
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

BIN=${VIDOW_BIN:-./build/vidow}
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "test-drag-insert.sh: vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }
# 窗口状态一行一条：标题 / 在哪块屏 / 浮动与否 / 位置尺寸
wins() {
	./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    g = w["float_geometry"] if w["floating"] else w["geometry"]
    print("%s %s %s %d %d %d %d" % (w["title"], w["output"], w["floating"],
                                    g["x"], g["y"], g["width"], g["height"]))'
}
field() { wins | awk -v t="$1" '$1 == t { print $'"$2"' }'; }
cols() {
	./build/vidowmsg columns | python3 -c '
import sys, json
print(len(json.load(sys.stdin)))'
}
# 一条窗口的几何，拼成 "x,y,w,h" 方便比
geom() { echo "$(field "$1" 4),$(field "$1" 5),$(field "$1" 6),$(field "$1" 7)"; }
# 布置一个窗口：起一个 pattern 窗口，把它的列宽设成 0.3（378 宽）
spawn() {
	python3 ./tools/pattern.py "$1" >/dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 1.5
	./build/vidowmsg focus-window --title "$1" >/dev/null
	./build/vidowmsg set-column-width 0.3 >/dev/null
	sleep 0.3
}

# 窗口里那个「抓点」：都要避开窗口边缘的装饰圈
GRAB_DX=100
GRAB_DY=190
# 拖一条窗口到 (x,y)：从 (起点x+dx, 起点y+dy) 按下、拖过去、停住（先不松手）
start_drag() { # $1 = 标题, $2 = 目标 x, $3 = 目标 y, $4 = 抓点 dx, $5 = 抓点 dy
	local x=$(field "$1" 4) y=$(field "$1" 5)
	act "pointer-motion $((x + $4)) $((y + $5))" 0.2
	act "pointer-button left press mod" 0.3
	act "pointer-motion $2 $3" 0.4
	act "pointer-motion $2 $3" 0.2   # 再来一次同一个落点：预览该是幂等的
}
release() { act "pointer-button left release mod" 0.7; }

echo "== 准备：三条列、每条列一个窗口（378 宽 = 可用宽 1260 的 0.3）=="
spawn w1
spawn w2
spawn w3
if [ "$(cols)" = "3" ] && [ "$(field w1 4)" = "10" ] && [ "$(field w2 4)" = "398" ] &&
		[ "$(field w3 4)" = "786" ]; then
	pass "三条列并排在 10 / 398 / 786（各 378x700）"
else
	fail "布局不对：w1=$(geom w1) w2=$(geom w2) w3=$(geom w3) 列数=$(cols)"
fi
if [ "$(geom w1)" = "10,10,378,700" ]; then
	pass "w1 占满整条列高：$(geom w1)"
else
	fail "w1 的几何不对：$(geom w1)（期望 10,10,378,700）"
fi

echo
echo "== 右边那条缝也算数：拖到 w1 那条列的**右半边**（离右边那条缝更近） =="
# niri 的 vert_dist 量的是「到**最近一条列左边缘**的距离」（column_xs 里最近的
# 那个），左右两边的缝都算 —— 光标在 w1 那条列（10..388）的右半边时，最近的缝是
# w2 那条列的左边缘 398，也就是「插到 w1 和 w2 中间」，不是「并进 w1 那条列」。
# 光标 (350,200) → 到 398 只有 43，到 w1 的上边缘 195 → 43 < 195 → 新开一条列。
grim -o HEADLESS-1 "$SHOT/before-right-gap.png" 2>/dev/null
start_drag w3 350 200 20 20
grim -o HEADLESS-1 "$SHOT/right-gap.png" 2>/dev/null
release
grim -o HEADLESS-1 "$SHOT/after-right-gap.png" 2>/dev/null
if grep -aq "grab: w3 在 HEADLESS-1 的第 1 块工作区里挪到第 2 位" "$LOG"; then
	pass "落点是「新开一条列、插到第 2 位」（右边的缝也算）"
else
	fail "没有插到中间那条缝的日志（$(grep -a 'grab:' "$LOG" | tail -1)）"
fi
if [ "$(field w3 4)" = "398" ] && [ "$(field w2 4)" = "786" ]; then
	pass "w3 那条列插到了 w1 和 w2 中间：w3 x 786→398，w2 x 398→786"
else
	fail "顺序不对：w3 x=$(field w3 4)（期望 398），w2 x=$(field w2 4)（期望 786）"
fi

echo
echo "== 插进 w3 那条列：拖到那条列的**上边缘**（离窗口缝比离列左边缘近） =="
# 光标 (698,40)：目标列（中间那条，w3 在 398）宽 378 → 到左右两条缝都远（305 / 83），
# 列里只有 w3 一个窗口，缝在 y=10 和 y=720 → 到上缝 35。35 < 83 → 进列。
grim -o HEADLESS-1 "$SHOT/before-into-top.png" 2>/dev/null
start_drag w1 698 40 $GRAB_DX $GRAB_DY
grim -o HEADLESS-1 "$SHOT/into-top.png" 2>/dev/null
release
grim -o HEADLESS-1 "$SHOT/after-into-top.png" 2>/dev/null
if grep -aq "grab: w1 插进 HEADLESS-1 的一条列里的第 1 位" "$LOG"; then
	pass "落点是「插进那条列的第 1 位」（niri 的 InColumn）"
else
	fail "没有插进列的日志（$(grep -a 'grab:' "$LOG" | tail -1)）"
fi
# w1 原来那条列空了就没了，剩下的两条列并到 10 / 398：w1、w3 在同一条列里对半劈
if [ "$(geom w1)" = "10,10,378,345" ]; then
	pass "w1 插到了那条列的最上面（高度对半劈）：$(geom w1)"
else
	fail "w1 的几何不对：$(geom w1)（期望 10,10,378,345）"
fi
if [ "$(geom w3)" = "10,365,378,345" ]; then
	pass "w3 被挤到下半截：$(geom w3)"
else
	fail "w3 的几何不对：$(geom w3)（期望 10,365,378,345）"
fi
if [ "$(cols)" = "2" ]; then
	pass "列数 3 → 2（w1 原来那条空列拆掉了）"
else
	fail "列数不对：$(cols)（期望 2）"
fi

echo
echo "== 新开一条列：拖到列带最左边（外层 gap 里，离列左边缘最近） =="
# 从 w2（398,10 378x700）的左上角起拖，拖到 x=2 → 光标在列带左边那条之外 →
# niri 的 `x < 0 → NewColumn(0)`：w2 那条列插到最前面
start_drag w2 2 360 20 20
grim -o HEADLESS-1 "$SHOT/new-column.png" 2>/dev/null
release
grim -o HEADLESS-1 "$SHOT/after-new-column.png" 2>/dev/null
if grep -aq "grab: w2 在 HEADLESS-1 的第 1 块工作区里挪到第 1 位" "$LOG"; then
	pass "落点是「新开一条列、插到第 1 位」（niri 的 NewColumn）"
else
	fail "没有新列的日志（$(grep -a 'grab:' "$LOG" | tail -1)）"
fi
if [ "$(field w2 4)" = "10" ] && [ "$(field w1 4)" = "398" ]; then
	pass "那条列插到了最前面：w2 x=10，w1/w3 那条列让到 398"
else
	fail "顺序不对：w2 x=$(field w2 4)（期望 10），w1 x=$(field w1 4)（期望 398）"
fi

echo
echo "== 插进两个窗口的那条列：拖到那条列的**下边缘**（离下面的缝最近） =="
# 现在 w2 在 10（378x700），w1/w3 那条列在 398（两个 345 高）。光标 (698,700)：
# 目标列在 398 → 到左右两条缝 305 / 83；缝在 y=10、365、720 → 到最下面那个 15。
# 15 < 83 → 进列。
grim -o HEADLESS-1 "$SHOT/before-into-bottom.png" 2>/dev/null
start_drag w2 698 700 20 20
grim -o HEADLESS-1 "$SHOT/into-bottom.png" 2>/dev/null
release
grim -o HEADLESS-1 "$SHOT/after-into-bottom.png" 2>/dev/null
if grep -aq "grab: w2 插进 HEADLESS-1 的一条列里的第 3 位" "$LOG"; then
	pass "落点是「插进那条列的第 3 位」（贴下边缘 → 落最下面）"
else
	fail "插进列第 3 位的日志没出现（$(grep -a 'grab:' "$LOG" | tail -1)）"
fi
if [ "$(geom w2)" = "10,482,378,226" ]; then
	pass "被拖的那个落在最下面、三个窗口三等分（(700-2×10)/3 = 226）：w2 = $(geom w2)"
else
	fail "w2 的几何不对：$(geom w2)（期望 10,482,378,226）"
fi
if [ "$(cols)" = "1" ]; then
	pass "三个窗口都在同一条列里（列数 2 → 1）"
else
	fail "列数不对：$(cols)（期望 1）"
fi

exec 9>&-
echo
python3 - "$SHOT" <<'PY'
import sys
import numpy as np
from PIL import Image

shots = sys.argv[1] + '/'
ok = True

def say(cond, msg):
	global ok
	print('  %s  %s' % ('PASS' if cond else 'FAIL', msg))
	if not cond:
		ok = False

def px(name, x, y):
	img = np.asarray(Image.open(shots + name).convert('RGB')).astype(int)
	return img[y, x]

# 预览块的颜色 = niri 的默认 insert-hint 色 #ffc87f80（橙 255,200,127、alpha 128、
# 预乘）。半透明叠上去的结果 = 预乘色 + (1-alpha) × 底下那层：128+0.498×底。
# 判据就用这个式子对：拿**同一块地方、同一份布局、没拖之前**那张图当底，
# 算出来的值必须和拖动中量到的一样（容差 12）—— 一次把「画在哪儿」「多透明」
# 「颜色对不对（忘了预乘会差一大截）」都钉住，而且跟底下是什么颜色无关。
HINT_RGB = (128.0, 100.0, 63.0)
HINT_KEEP = 0.498

def blend_over(base):
	return tuple(HINT_RGB[i] + HINT_KEEP * base[i] for i in range(3))

def is_blend(during, base, tol=12):
	return all(abs(during[i] - blend_over(base)[i]) <= tol for i in range(3))

def pure_red(p):
	return p[0] > 200 and p[1] < 40 and p[2] < 40

def pure_blue(p):
	return p[2] > 200 and p[0] < 40

def pure_green(p):
	return p[1] > 200 and p[0] < 40

def dark(p):
	return max(p[0], p[1], p[2]) < 40

def plateau(p):
	"""窗口内容那种「纯色块」：一个通道到顶、另一个贴地。
	预览块叠上去之后三个通道都被抬起来（半透明橙），就不再是纯色块 ——
	所以「松手之后这一块回到纯色块（或者空成黑）」正好证明预览块收掉了。"""
	return max(p[0], p[1], p[2]) > 200 and min(p[0], p[1], p[2]) < 50

# 量点都挑在 pattern 窗口内容块里（避开标题栏 / 边框 / 阴影那些过渡带），并且避开
# 被拖的那个窗口和光标 —— 底下的颜色无所谓，混色那个式子跟它无关。

# 1) 插进中间那条列（贴那条列上边缘）：预览块是那条列顶上的一条横条
#    （398,10 378x150，niri 的 InColumn 第 0 位）。量 (480,120)：全程在窗口内容里
#    （标题栏 ~40px 之下）；被拖的 w1 这会儿在 598..976，够不着。
base = px('before-into-top.png', 480, 120)
during = px('into-top.png', 480, 120)
say(pure_red(base) and is_blend(during, base),
    '插进列时预览块画在那条列顶上：480,120 %s → %s（= 128,100,63 + 0.498×底）'
    % (tuple(base), tuple(during)))
# 松手之后预览块必须收掉：(480,120) 这时是右边那条列的窗口内容（同一块红）
after = px('after-into-top.png', 480, 120)
say(pure_red(after), '松手后预览块收掉了（480,120 回到纯红 → %s）' % (tuple(after),))

# 2) 插到**右边的缝**上（光标在左边那条列的右半边）：卡片骑在那条缝中间
#    （388 和 398 的中间 393 − 150 = 243，300x680）。量 (280,100) 在卡片里、且在
#    左边那条列上（被拖的 w3 这会儿在 330..708、y 180 以下，够不着）。
base = px('before-right-gap.png', 280, 100)
during = px('right-gap.png', 280, 100)
say(is_blend(during, base),
    '插到右边那条缝时预览块骑在缝上：280,100 %s → %s' % (tuple(base), tuple(during)))
# 卡片左边之外（150,100）不该有东西 —— 这一条钉住卡片的左边界
same = tuple(px('right-gap.png', 150, 100)) == tuple(px('before-right-gap.png', 150, 100))
say(same, '卡片从 243 起（150,100 跟没拖之前一模一样）')
after = px('after-right-gap.png', 280, 100)
say(plateau(after) or dark(after),
    '松手后卡片收掉了（280,100 回到纯色块/空 → %s）' % (tuple(after),))

# 3) 新开一条列（贴列带最左边）：预览块是那张整高的「卡片」（300 宽、整块可用高；
#    贴屏边被夹进来之后 x=-140，可见 0..160）。量 (60,200)：w1 窗口内容块的左边；
#    被拖的 w2 这会儿在 y 340 以下，盖不到。
base = px('after-right-gap.png', 60, 200)
during = px('new-column.png', 60, 200)
say(pure_red(base) and is_blend(during, base),
    '新开列时预览块是那张整高的卡片：60,200 %s → %s' % (tuple(base), tuple(during)))
# 卡片右边之外（250,60）是同一个窗口的绿块：不该有预览块 —— 这一条钉住卡片宽度
p = px('new-column.png', 250, 60)
say(pure_green(p), '卡片就到 160 为止（250,60 还是纯绿 → %s）' % (tuple(p),))
after = px('after-new-column.png', 60, 200)
say(pure_red(after), '松手后卡片收掉了（60,200 回到纯红 → %s）' % (tuple(after),))

# 4) 插到那条列最下面：预览块是那条列底部的一条横条（398,560 378x150）。
#    量 (480,620)：在下面那个窗口的内容块里（蓝）；被拖的 w2 这会儿在 y 680 以下。
base = px('before-into-bottom.png', 480, 620)
during = px('into-bottom.png', 480, 620)
say(pure_blue(base) and is_blend(during, base),
    '插到列最下面时预览块画在底部那截：480,620 %s → %s' % (tuple(base), tuple(during)))
# 落定之后三个窗口并成一条列（宽 378，贴着屏左边）→ (480,620) 上什么都没有
after = px('after-into-bottom.png', 480, 620)
say(dark(after), '松手后预览块收掉了（480,620 那块已经空了 → %s）' % (tuple(after),))

print()
if not ok:
	print('有判据没过（见上面的清单）')
	sys.exit(1)
PY
[ $? -ne 0 ] && FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-drag-insert.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
