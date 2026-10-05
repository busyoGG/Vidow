#!/bin/bash
# XWayland 弹出物（菜单 / 下拉框 / 工具提示）的正确识别与渲染。
#
# 判据（每条都能从日志或像素上量出来）：
#   1) 识别：override-redirect、以及 _NET_WM_WINDOW_TYPE_* 里的弹出类型，都归为
#      「弹出物」；属性晚到（late）也能改判；UTILITY 这种普通窗口照旧被布局接管
#      （反向对照）
#   2) 定位：位置尺寸完全按客户端给的绝对坐标（X 像素 ÷ xwayland_scale），
#      不居中、不夹工作区、不做移动补间
#   3) 不套 toplevel 的样式：不装饰（日志里没有对应尺寸的装饰离屏缓冲）、不画阴影
#      （像素级：弹出物周围那圈一个像素都没变）、不做开关动画
#      —— 开窗/关窗/移动动画全调成 2500ms，真做了的话 0.45 秒时一眼就看出来
#   4) 不抢焦点：弹出物出现前后，父窗口那一圈焦点边框的平均色不变
#   5) 层级：X11 里弹出物永远在所有托管窗口之上（后开的窗口也盖不住它）
#   6) 混合 DPI：X 坐标按全局缩放除回逻辑坐标；命中坐标按同一比例乘回去
#
# 全 headless，不碰宿主的 X / 合成器 / portal。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-popup-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
OWNER_PID=""
POPUP_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$POPUP_PID" ] && kill "$POPUP_PID" 2>/dev/null
	[ -n "$OWNER_PID" ] && kill "$OWNER_PID" 2>/dev/null
	if [ -n "${VIDOW_PGID:-}" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	rm -rf "$RUNDIR"
}
trap cleanup EXIT

if ! command -v grim >/dev/null; then
	echo "test-xwayland-popup.sh: 需要 grim（截图做像素断言）"
	exit 1
fi

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = true
-- 动画拉长：弹出物真做了动画的话，0.45 秒时一眼就能看出来
window_open_ms = 2500
window_close_ms = 2500
window_move_ms = 2500
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

start_vidow() {
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PID=$!
	VIDOW_PGID=$VIDOW_PID
	for _ in $(seq 1 40); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)
}
mark_line() { wc -l < "$LOG"; }
since() { tail -n +"$(( $1 + 1 ))" "$LOG"; }
shot() { grim "$RUNDIR/$1.png"; }

# ------------------------------------------------------------------ 像素探针
cat > "$RUNDIR/probe.py" <<'PY'
"""像素断言小工具：PASS/FAIL 打到 stdout，退出码给 bash 用。"""
import sys
from PIL import Image

def img(path):
    return Image.open(path).convert("RGB")

def near(a, b, tol):
    return all(abs(x - y) <= tol for x, y in zip(a, b))

def out(ok, msg):
    print(("  PASS  " if ok else "  FAIL  ") + msg)
    sys.exit(0 if ok else 1)

cmd = sys.argv[1]
if cmd == "pix":
    path, x, y = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    want = tuple(int(v) for v in sys.argv[5:8])
    tol = int(sys.argv[8]) if len(sys.argv) > 8 else 8
    got = img(path).getpixel((x, y))
    out(near(got, want, tol), "像素 (%d,%d)=%s，预期 %s" % (x, y, got, want))
elif cmd == "rect":
    path = sys.argv[2]
    x, y, w, h = (int(v) for v in sys.argv[3:7])
    want = tuple(int(v) for v in sys.argv[7:10])
    tol = int(sys.argv[10]) if len(sys.argv) > 10 else 8
    band = img(path).crop((x, y, x + w, y + h))
    bad = [p for p in band.getdata() if not near(p, want, tol)]
    out(not bad, "%s 的矩形 (%d,%d)+%dx%d 全是 %s（%d/%d 个像素不符）"
        % (path.split("/")[-1], x, y, w, h, want, len(bad), w * h))
elif cmd == "empty":
    path = sys.argv[2]
    x, y, w, h = (int(v) for v in sys.argv[3:7])
    want = tuple(int(v) for v in sys.argv[7:10])
    tol = int(sys.argv[10]) if len(sys.argv) > 10 else 8
    band = img(path).crop((x, y, x + w, y + h))
    hits = [p for p in band.getdata() if near(p, want, tol)]
    out(not hits, "%s 的矩形 (%d,%d)+%dx%d 里没有 %s（命中 %d 个像素）"
        % (path.split("/")[-1], x, y, w, h, want, len(hits)))
elif cmd == "strip":
    # 「rect 外扩 band、但不含 rect」这一圈里两图必须逐像素一致：一条断言盖住
    # 「没画阴影」「没把背景重画一遍」
    a, b = sys.argv[2], sys.argv[3]
    x, y, w, h = (int(v) for v in sys.argv[4:8])
    band = int(sys.argv[8])
    ia, ib = img(a), img(b)
    pa, pb = ia.load(), ib.load()
    count = worst = 0
    for yy in range(y - band, y + h + band):
        for xx in range(x - band, x + w + band):
            if x <= xx < x + w and y <= yy < y + h:
                continue
            ca, cb = pa[xx, yy], pb[xx, yy]
            d = max(abs(p - q) for p, q in zip(ca, cb))
            if d:
                count += 1
                worst = max(worst, d)
    out(count == 0, "弹出物外扩 %dpx 那一圈完全一致（%d 个像素不符，最大差 %d）"
        % (band, count, worst))
elif cmd == "edge":
    # rect 内侧 band 宽的一圈（焦点边框就在那儿）：两图的平均色必须几乎一样。
    # 弹出物抢焦点的话这一圈的强调色会整圈变暗，平均值要掉几十。
    a, b = sys.argv[2], sys.argv[3]
    x, y, w, h = (int(v) for v in sys.argv[4:8])
    band = int(sys.argv[8])
    def ring(path):
        pix = img(path).load()
        acc = [0, 0, 0]
        n = 0
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if band <= xx - x < w - band and band <= yy - y < h - band:
                    continue
                c = pix[xx, yy]
                acc = [acc[i] + c[i] for i in range(3)]
                n += 1
        return [v / n for v in acc]
    ma, mb = ring(a), ring(b)
    d = max(abs(p - q) for p, q in zip(ma, mb))
    out(d <= 4.0, "窗口边框那一圈平均色不变（无弹出物 %s，有弹出物 %s，最大差 %.1f）"
        % (["%.0f" % v for v in ma], ["%.0f" % v for v in mb], d))
else:
    print("  FAIL  未知探针 %s" % cmd)
    sys.exit(1)
PY
probe() { python3 -W ignore::DeprecationWarning "$RUNDIR/probe.py" "$@" || FAILED=1; }

# ------------------------------------------------------------------ 起合成器
start_vidow

# ------------------------------------------------------------------ 父窗口
mark "父窗口（一个普通 X11 窗口，弹出物要盖在它上面）"
setsid ./build/xpopup owner 100 100 400 300 > "$RUNDIR/owner.out" 2>&1 &
OWNER_PID=$!
for _ in $(seq 1 40); do
	grep -aq "place xp-owner" "$LOG" && break
	sleep 0.25
done
sleep 3    # 等父窗口的开窗动画（2500ms）走完
OWNER_ID=$(head -1 "$RUNDIR/owner.out")
read OX OY OW OH <<<"$(grep -a 'place xp-owner -> ' "$LOG" | tail -1 |
	sed 's/.*-> //; s/,/ /; s/x/ /')"
echo "  父窗口 place $OX,$OY ${OW}x${OH}（id $OWNER_ID）"
shot base
# 弹出物放在父窗口内部：这样「弹出物周围有没有阴影」才能拿父窗口的颜色当背板量
PX=200; PY=300; PW=200; PH=120

# ------------------------------------------------------------------ 四种弹出物
for KIND in or tooltip menu combo; do
	mark "弹出物 kind=$KIND（在 $PX,$PY 摆 ${PW}x${PH}）"
	EXTRA=""
	[ "$KIND" = combo ] && EXTRA="parent=$OWNER_ID"
	LINE=$(mark_line)
	setsid ./build/xpopup popup "$KIND" "$PX" "$PY" "$PW" "$PH" $EXTRA \
		> "$RUNDIR/popup.out" 2>&1 &
	POPUP_PID=$!
	for _ in $(seq 1 40); do
		since "$LINE" | grep -aq "place xp-popup" && break
		sleep 0.25
	done
	sleep 0.2
	shot "open-$KIND"
	SINCE=$(since "$LINE")

	# 1) 识别（override-redirect 的窗口在关联时就归好类了，不会再打「归类为」那行）
	echo "$SINCE" | grep -aq "弹出物" &&
		pass "$KIND：被识别成弹出物（不是普通窗口）" ||
		fail "$KIND：没被识别成弹出物"
	# 2) 定位：就在客户端说的绝对坐标上
	echo "$SINCE" | grep -aqF "place xp-popup -> $PX,$PY ${PW}x${PH}" &&
		pass "$KIND：摆到 $PX,$PY ${PW}x${PH}（客户端说的绝对坐标）" ||
		fail "$KIND：没摆到 $PX,$PY ${PW}x${PH}（日志：$(echo "$SINCE" |
			grep -a 'place xp-popup' | tail -1 | sed 's/.*\[DEBUG\] //')）"
	echo "$SINCE" | grep -aq "float xp-popup" &&
		fail "$KIND：被浮动布局接管了（居中/夹工作区）" ||
		pass "$KIND：没被浮动布局接管（不居中、不夹工作区）"
	# 3) 没有装饰离屏缓冲 = 没套 toplevel 的边框/圆角/阴影
	echo "$SINCE" | grep -aq "fx: target $((PW + 64))x$((PH + 64))" &&
		fail "$KIND：出现过 $((PW + 64))x$((PH + 64)) 的装饰缓冲（被套了 toplevel 样式）" ||
		pass "$KIND：没有装饰离屏缓冲（不画边框/圆角/阴影）"

	# 像素：弹出物正好那一块，四周那圈一个像素都没变（没阴影、没抢焦点、没动布局）
	probe rect "$RUNDIR/open-$KIND.png" "$PX" "$PY" "$PW" "$PH" 192 0 0 8
	probe pix "$RUNDIR/open-$KIND.png" "$((PX + PW - 1))" "$((PY + PH - 1))" 192 0 0 8
	probe pix "$RUNDIR/open-$KIND.png" "$((PX - 1))" "$((PY + PH / 2))" 0 0 192 24
	probe strip "$RUNDIR/base.png" "$RUNDIR/open-$KIND.png" "$PX" "$PY" "$PW" "$PH" 20
	probe edge "$RUNDIR/base.png" "$RUNDIR/open-$KIND.png" "$OX" "$OY" "$OW" "$OH" 6

	# 4) 关掉：不留残影（2500ms 的关闭动画真跑起来的话这里会拍到半透明的它）
	kill "$POPUP_PID" 2>/dev/null; POPUP_PID=""
	sleep 0.5
	shot "closed-$KIND"
	probe strip "$RUNDIR/base.png" "$RUNDIR/closed-$KIND.png" "$PX" "$PY" "$PW" "$PH" 20
	probe edge "$RUNDIR/base.png" "$RUNDIR/closed-$KIND.png" "$OX" "$OY" "$OW" "$OH" 6
done

# ------------------------------------------------------------------ 移动
mark "客户端自己挪位置（OR 走 ConfigureNotify，非 OR 走 ConfigureRequest）"
MX=$((PX + 300)); MY=$((PY + 200))
for KIND in or menu; do
	setsid ./build/xpopup popup "$KIND" "$PX" "$PY" "$PW" "$PH" move=300,200 \
		> "$RUNDIR/popup.out" 2>&1 &
	POPUP_PID=$!
	for _ in $(seq 1 60); do
		grep -aq "^moved " "$RUNDIR/popup.out" 2>/dev/null && break
		sleep 0.25
	done
	sleep 0.3   # 2500ms 的移动补间真跑起来的话，这会儿才走了 12%
	shot "move-$KIND"
	grep -aq "place xp-popup -> $MX,$MY ${PW}x${PH}" "$LOG" &&
		pass "$KIND：新位置 $MX,$MY 生效" ||
		fail "$KIND：挪了之后没摆到新位置（$(grep -a 'place xp-popup' "$LOG" |
			tail -1 | sed 's/.*\[DEBUG\] //')）"
	probe rect "$RUNDIR/move-$KIND.png" "$MX" "$MY" "$PW" "$PH" 192 0 0 8
	probe empty "$RUNDIR/move-$KIND.png" "$PX" "$PY" "$PW" "$PH" 192 0 0 8
	kill "$POPUP_PID" 2>/dev/null; POPUP_PID=""
	sleep 0.4
done

# ------------------------------------------------------------------ 层级
mark "层级：弹出物永远在所有托管窗口之上"
setsid ./build/xpopup popup or "$PX" "$PY" "$PW" "$PH" > "$RUNDIR/popup.out" 2>&1 &
POPUP_PID=$!
for _ in $(seq 1 40); do
	grep -aq "place xp-popup" "$LOG" && break
	sleep 0.25
done
sleep 0.3
# 之后才开的窗口会盖住它原来待的那块地方（vidow 把它平铺到右边一列）
setsid ./build/xpopup owner 0 0 400 300 > "$RUNDIR/owner2.out" 2>&1 &
OWNER2_PID=$!
sleep 1.5
shot stack
probe rect "$RUNDIR/stack.png" "$PX" "$PY" "$PW" "$PH" 192 0 0 8
kill "$POPUP_PID" 2>/dev/null; POPUP_PID=""
kill "$OWNER2_PID" 2>/dev/null
sleep 0.6

# ------------------------------------------------------------------ 反向对照
mark "反向对照：UTILITY 是普通窗口，必须照旧被布局接管"
LINE=$(mark_line)
setsid ./build/xpopup popup utility 100 100 200 120 > "$RUNDIR/popup.out" 2>&1 &
POPUP_PID=$!
sleep 2
SINCE=$(since "$LINE")
echo "$SINCE" | grep -aq "弹出物" &&
	fail "UTILITY 被误判成弹出物" ||
	pass "UTILITY 没被误判成弹出物"
echo "$SINCE" | grep -aq "place xp-popup -> 100,100" &&
	fail "UTILITY 被当成弹出物按绝对坐标摆了" ||
	pass "UTILITY 被布局接管（位置尺寸由 vidow 定，走的是 toplevel 那条管线）"
kill "$POPUP_PID" 2>/dev/null; POPUP_PID=""
sleep 0.5

# ------------------------------------------------------------------ 属性晚到
mark "属性晚到：先当普通窗口/浮动窗口，类型随后才到，改判之后听客户端的"
LINE=$(mark_line)
setsid ./build/xpopup popup menu "$PX" "$PY" "$PW" "$PH" fixed late=600 \
	move=300,200,2000 > "$RUNDIR/popup.out" 2>&1 &
POPUP_PID=$!
sleep 3.2
SINCE=$(since "$LINE")
echo "$SINCE" | grep -aq "归类为弹出物" &&
	pass "晚到的 _NET_WM_WINDOW_TYPE 触发了改判" ||
	fail "晚到的 _NET_WM_WINDOW_TYPE 没触发改判"
echo "$SINCE" | grep -aq "float mode xp-popup: 0 -> 1" &&
	pass "改判之后退出了窗口布局（0 -> 1 = 不再平铺）" ||
	fail "改判之后还留在窗口布局里"
# 改判之后客户端自己挪的位置必须原样生效（说明位置尺寸真的听它了）
echo "$SINCE" | grep -aqF "place xp-popup -> $MX,$MY ${PW}x${PH}" &&
	pass "改判之后客户端挪到 $MX,$MY ${PW}x${PH} 原样生效" ||
	fail "改判之后的位置没生效（$(echo "$SINCE" | grep -a 'place xp-popup' |
		tail -1 | sed 's/.*\[DEBUG\] //')）"
# 只看改判「之后」的日志：改判前它还是个普通浮动窗口，那时候它本来就该被装饰
AFTER=$(echo "$SINCE" | sed -n '/归类为弹出物/,$p')
echo "$AFTER" | grep -aq "fx: target $((PW + 64))x$((PH + 64))" &&
	fail "改判之后仍在画装饰缓冲（弹出物被套了 toplevel 样式）" ||
	pass "改判之后不再画装饰缓冲"
shot late
probe rect "$RUNDIR/late.png" "$MX" "$MY" "$PW" "$PH" 192 0 0 8
probe empty "$RUNDIR/late.png" "$PX" "$PY" "$PW" "$PH" 192 0 0 8
kill "$POPUP_PID" 2>/dev/null; POPUP_PID=""
[ -n "$OWNER_PID" ] && kill "$OWNER_PID" 2>/dev/null
OWNER_PID=""
sleep 0.5

pgrep -x vidow >/dev/null && pass "合成器还活着（没有 abort）" || fail "合成器挂了"
grep -aq "Assertion" "$LOG" && fail "日志里有 assert" || true

# ================================================================== 混合 DPI
mark "混合 DPI：X 坐标按全局缩放除回逻辑坐标"
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "auto"
outputs = "HEADLESS-1:1 HEADLESS-2:2"
xwayland = true
window_open_ms = 2500
window_close_ms = 2500
window_move_ms = 2500
EOF
rm -f "$LOG"
export WLR_HEADLESS_OUTPUTS=2
start_vidow
grep -ao "xwayland: 全局缩放 [0-9.]* -> [0-9.]*" "$LOG" | tail -1 |
	grep -q "2.00$" &&
	pass "X 全局缩放 = 2（最密那块屏）" ||
	fail "X 全局缩放不是 2：$(grep -ao 'xwayland: 全局缩放.*' "$LOG" | tail -1)"

# 2 倍屏在逻辑 x = 1280..1920；X 像素 = 逻辑 × 2，所以让客户端把菜单放在
# X (2800,400) —— 它该落在逻辑 (1400,200)，尺寸 200x120 X 像素 -> 100x60 逻辑。
LINE=$(mark_line)
setsid ./build/xpopup popup or 2800 400 200 120 > "$RUNDIR/popup.out" 2>&1 &
POPUP_PID=$!
for _ in $(seq 1 40); do
	since "$LINE" | grep -aq "place xp-popup" && break
	sleep 0.25
done
sleep 0.4
shot dpi
SINCE=$(since "$LINE")
echo "$SINCE" | grep -aqF "place xp-popup -> 1400,200 100x60" &&
	pass "X (2800,400) 200x120 -> 逻辑 (1400,200) 100x60（按 2 倍除）" ||
	fail "X 坐标没按全局缩放换算：$(echo "$SINCE" | grep -a 'place xp-popup' |
		tail -1 | sed 's/.*\[DEBUG\] //')"
echo "$SINCE" | grep -aq "fx: target $(((100 + 64) * 2))x$(((60 + 64) * 2))" &&
	fail "弹出物在 2 倍屏上被套了装饰" ||
	pass "弹出物在 2 倍屏上也没装饰"
# grim 在混合 DPI 下给的是**设备像素**图（这里 3840x1440 = 布局 1920x720 × 2），
# 所以逻辑 (1400,200)+100x60 在图上就是 (2800,400)+200x120
probe rect "$RUNDIR/dpi.png" 2804 404 192 112 192 0 0 8
probe empty "$RUNDIR/dpi.png" 2760 360 36 36 192 0 0 8
# 命中：光标到逻辑 (1420,220)。
#   局部坐标是**表面自己的逻辑坐标系**里的数（Xwayland 声明了 buffer scale = 2，
#   所以 (1420-1400) / 2 = 10 … 见下面：vidow 给的局部 × 表面倍数 = X 坐标），
#   X 客户端那侧看到的是「布局偏移 × 全局缩放」= 40,40 —— 点哪儿就是哪儿。
./build/vdrag 7396 3056 7396 3056 >/dev/null 2>&1
sleep 0.4
HIT=$(grep -a "x11: 指针 .* -> xp-popup 局部" "$LOG" | tail -1)
SFACE=$(grep -a "x11: xp-popup 缓冲 .*（表面 \([0-9]*\) 倍" "$LOG" | tail -1 |
	sed 's/.*（表面 \([0-9]*\) 倍.*/\1/')
[ -n "$SFACE" ] || SFACE=1
echo "  命中行：$(echo "$HIT" | sed 's/.*\[DEBUG\] //')  表面倍数：$SFACE"
echo "$HIT" | grep -aq "局部 $((20 * 2 / SFACE)),$((20 * 2 / SFACE))" &&
	pass "命中局部坐标 = 布局偏移 20,20 × 2 / 表面倍数 $SFACE" ||
	fail "命中坐标不对：$(echo "$HIT" | sed 's/.*\[DEBUG\] //')"
LOC=$(echo "$HIT" | sed 's/.*局部 //')
python3 -c "
import sys
lx, ly = [float(v) for v in '$LOC'.split(',')]
s = float('$SFACE')
ok = abs(lx * s - 40) <= 1 and abs(ly * s - 40) <= 1
print('  PASS  X 客户端拿到的坐标 %g,%g = 布局偏移 20,20 × 2' % (lx * s, ly * s)
      if ok else '  FAIL  X 客户端坐标 %g,%g 不是 40,40' % (lx * s, ly * s))
sys.exit(0 if ok else 1)
" || FAILED=1
kill "$POPUP_PID" 2>/dev/null; POPUP_PID=""

pgrep -x vidow >/dev/null && pass "合成器还活着" || fail "合成器挂了"
grep -aq "Assertion\|abort" "$LOG" && fail "日志里有 assert / abort" || true

echo
if [ $FAILED -eq 0 ]; then
	echo "test-xwayland-popup.sh: OK"
else
	echo "test-xwayland-popup.sh: 有失败项"
fi
exit $FAILED
