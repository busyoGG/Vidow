#!/bin/bash
# 摇晃找光标：快速来回晃几下指针，指针就地放大 —— 晃一下大一圈，接着晃接着大
# （config 的 cursor_shake / cursor_shake_scale）。
#
# 判据（headless 1280x720 一块屏，截图用 grim 当裁判 —— 「进没进截图」由像素说了算）：
#   1) cursor 命令报得出光标状态：image（当前光标图的物理尺寸）/ hardware / hw_max
#   2) 慢慢往一个方向移动**不**触发（不然正常用鼠标就会乱放大）
#   3) 来回晃（800ms 内 5 次换向、每段 ≥16px、而且跨 ≥250ms）触发：光标图放大 2 倍
#   3b) **太容易触发**的反面：同一串换向挤在几十毫秒里（顺手甩两下）不放大 ——
#      门槛从「3 次换向」抬到「5 次换向 + 晃够 250ms」（用户报的「太容易触发」）
#   4) 到点自己**动画**缩回去（700ms 后开始缩，200ms 线性补间走完，中途能量到
#      1 和 2 之间的中间值 —— 不是「啪」地跳回原尺寸）
#   5) 接着晃接着大，而且是**线性加**不是乘：2→3→4→5→6（每一圈只 +1），
#      放大本身也是补间（晃完 50ms 量到的倍数在起点和目标之间），
#      最后被「屏幕只有这么大」封顶
#   6) **不进截图**：grim 不带 -c 抓，画面里一个光标像素都没有（放大中也一样）
#   7) **显式指定才进截图**：grim -c 抓，光标在，外接矩形正好是放大后的尺寸、
#      箭头尖还钉在原来那个点上（热点按倍数缩放）
#   8) **软光标那条路**（真机上放大超出硬件光标平面就是它）：普通抓帧里照样一个
#      光标像素都没有（vidow 自己渲染抓帧，场景里本来就没光标），而 grim -c 能抓到
#      放大图 —— 屏幕上那个光标一直是放大的，不用为抓帧缩回去
#   9) 客户端自己设的光标（wl_pointer.set_cursor 的 surface）也能放大，而且还原的是
#      **客户端那张**（不是主题箭头）
#   9b) 放大中焦点切到**别的窗口**（新客户端 set_cursor）不许把放大缩回去 ——
#      用户报的「光标放大在焦点切换的时候会直接重置」
#  10) cursor_shake = false：晃不放大、cursor-shake 被拒且理由点得到配置
#  11) cursor_shake_scale：1.5 → 1.5 倍；20 → 被屏幕尺寸封顶
#  12) 日志里没有 assert / abort
#
# 判据 7 / 8 里「拍到的光标图」走 magnify_and_grab：放大的保持期只有 700ms，而
# grim 从启动到跟合成器握完手要 200~600ms —— 晃完随手抓，抓到的是「保持期过了、
# 正在缩回去」的中间尺寸（实测 36x57 / 44x70 / 52x83 / 23x37 都出现过）。这是
# **测试自己的时序问题**，不是合成器的：屏幕上那个光标本来就是活的。
#
# 用法：./test-cursor-shake.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py / frames / wev。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-cursor-shake-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
VIDOW_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*build/frames*|*build/xpopup*|*/wev*|*grim*|*cursorwin.py*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

cleanup() {
	stop_clients
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -TERM -- "-$pid" 2>/dev/null
	done
	sleep 0.4
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -KILL -- "-$pid" 2>/dev/null
	done
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v grim >/dev/null || { echo "test-cursor-shake.sh: 需要 grim"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "test-cursor-shake.sh: 需要 python3-PIL"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-cursor-shake.sh: 先构建"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

write_config() { # <额外配置行>
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
scale = "1"
window_open_ms = 60
window_close_ms = 60
window_move_ms = 60
$1
CFG
}

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

start_vidow() { # <额外配置行>
	export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
	write_config "$1"
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	VIDOW_PIDS="$VIDOW_PIDS $VIDOW_PGID"
	for _ in $(seq 1 60); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		echo "vidow 没起来"; tail -5 "$LOG"; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	sleep 0.5
}

stop_vidow() {
	stop_clients
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.5
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
		VIDOW_PGID=""
	fi
}

msg() { ./build/vidowmsg --raw "$@" 2>&1; }

# cursor 命令里某个字段（字符串原样、布尔输出 true/false）
cfield() { # <键>
	msg cursor | python3 -c '
import json,sys
try:
    v = json.loads(sys.stdin.read())["result"].get(sys.argv[1])
except Exception:
    v = None
print("" if v is None else (str(v).lower() if isinstance(v, bool) else v))
' "$1"
}

# 摇晃一次：6 段、每段 40px、段间 70ms —— 800ms 窗口内 5 次换向，而且这一串
# 摇晃跨了 ~280ms（新门槛要求「来回晃一会儿」，见 cursorfx.c 的 SHAKE_*）。
shake() {
	local d=40
	for _ in 1 2 3 4 5 6; do
		msg move-cursor "$d" 0 --relative >/dev/null
		d=$((-d))
		sleep 0.07
	done
}

# 顺手甩两下：同样 6 段（5 次换向），但挤在几十毫秒里 —— 跨时不够，不许放大。
flick() {
	local d=40
	for _ in 1 2 3 4 5 6; do
		msg move-cursor "$d" 0 --relative >/dev/null
		d=$((-d))
	done
}

# 画面里的「墨迹」：非黑像素个数 + 外接矩形（黑底 = headless 的默认背景）
ink() { # <png>
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
w, h = im.size
px = im.load()
xs = []
ys = []
n = 0
for y in range(h):
    for x in range(w):
        r, g, b = px[x, y]
        if r + g + b > 24:
            n += 1
            xs.append(x)
            ys.append(y)
if n == 0:
    print("0 0 0 0 0")
else:
    print(n, min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
PY
}

# 按倍数算尺寸（和 C 里的 lround 对齐）
scale_size() { # <w> <h> <倍数>
	python3 -c 'import sys
w,h,f = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
print("%dx%d" % (round(w*f), round(h*f)))' "$1" "$2" "$3"
}

# 晃到 <倍数> 倍，**当场**抓一张图，返回抓的那一下的光标尺寸。
#
# 为什么不能「晃完随手抓」：放大的保持期只有 700ms（cursorfx.c 的 SHAKE_HOLD_MS），
# 而 grim 从启动到跟合成器握完手要 200~600ms（随机器负载飘）—— 稍微慢一点，抓到
# 的就是「保持期过了、正在缩回去」的中间尺寸（实测 36x57 / 44x70 / 52x83 / 23x37
# 都出现过）。这是**测试自己的时序问题**，不是合成器的：屏幕上那个光标本来就是
# 活的，grim 抓到的就是它抓的那一刻的尺寸。
#
# 所以：等到 1x → 晃到目标倍数 → 等它到位 → 抓 → 抓完再对一次尺寸；两次不一样
# 说明抓的过程中它在动，等它缩回去从头再来（最多 5 轮）。
# 用法：magnify_and_grab <png> <倍数> <基础尺寸> [grim 参数…]
magnify_and_grab() { # <png> <倍数> <基础尺寸> [grim 参数…]
	local png=$1 want=$2 base=$3
	shift 3
	local img target i attempt
	for attempt in 1 2 3 4 5; do
		for _ in $(seq 1 60); do
			[ "$(cfield image)" = "$base" ] && break
			sleep 0.1
		done
		for i in $(seq 2 "$want"); do
			msg cursor-shake >/dev/null
			sleep 0.05
		done
		target=$(scale_size "${base%x*}" "${base#*x}" "$want")
		for _ in $(seq 1 40); do
			[ "$(cfield image)" = "$target" ] && break
			sleep 0.05
		done
		img=$(cfield image)
		grim "$@" "$png" >/dev/null 2>&1
		[ "$(cfield image)" = "$img" ] && { echo "$img"; return 0; }
	done
	echo "$img"
	return 1
}

# ------------------------------------------------------------ 1) 基本状态
mark "1. cursor 命令报得出光标状态"
start_vidow ""
msg move-cursor 300 100 >/dev/null
sleep 0.3
IMG0=$(cfield image)
HWMAX=$(cfield hw_max)
[ -n "$IMG0" ] && [ "$IMG0" != "0x0" ] && pass "光标图尺寸 image=$IMG0" \
	|| fail "image 没报出来（拿到 '$IMG0'）"
[ "$(cfield magnified)" = "false" ] && pass "初始 magnified=false" \
	|| fail "初始就是放大状态"
[ "$(cfield scale)" = "1" ] && pass "初始 scale=1" || fail "初始 scale 不是 1"
[ -n "$HWMAX" ] && pass "硬件光标上限 hw_max=$HWMAX" || fail "hw_max 没报出来"
case "$(cfield hardware)" in
true)  pass "光标在硬件平面上（不进渲染通道）" ;;
false) pass "光标是软光标（截图那条判据会自己说明）" ;;
*)     fail "hardware 字段没报出来" ;;
esac

# ------------------------------------------------------------ 2) 慢速不触发
mark "2. 慢慢移动不触发"
msg move-cursor 200 0 --relative >/dev/null
sleep 0.1
msg move-cursor 0 120 --relative >/dev/null
sleep 0.1
msg move-cursor -180 0 --relative >/dev/null
sleep 0.2
[ "$(cfield magnified)" = "false" ] && pass "单方向大范围移动不放大" \
	|| fail "慢慢移动也放大了（误触发）"
[ "$(cfield image)" = "$IMG0" ] && pass "尺寸没变（$IMG0）" \
	|| fail "尺寸被改了：$(cfield image)"

# ------------------------------------------------------------ 2b) 顺手甩两下不算
mark "2b. 一瞬间的来回甩不触发（要晃一会儿才算）"
msg move-cursor 300 100 >/dev/null
flick
sleep 0.35
[ "$(cfield magnified)" = "false" ] && pass "几十毫秒内 5 次换向不放大" \
	|| fail "甩一下就放大了（太容易触发）"
[ "$(cfield image)" = "$IMG0" ] && pass "尺寸还是 $IMG0" \
	|| fail "尺寸被改了：$(cfield image)"

# ------------------------------------------------------------ 3) 摇晃触发
mark "3. 来回晃触发放大"
msg move-cursor 300 100 >/dev/null
shake
sleep 0.35
[ "$(cfield magnified)" = "true" ] && pass "摇晃之后 magnified=true" \
	|| fail "摇晃没触发放大"
BIG=$(scale_size "${IMG0%x*}" "${IMG0#*x}" 2)
[ "$(cfield image)" = "$BIG" ] && pass "光标图放大 2 倍：$IMG0 → $BIG" \
	|| fail "放大后的尺寸是 $(cfield image)，期望 $BIG"
grep -aq "摇晃: 光标放大 2.00x（$IMG0 → $BIG" "$LOG" \
	&& pass "日志记了放大（$IMG0 → $BIG）" \
	|| fail "日志里没有放大记录"
grep -aq "摇晃: 光标放大.*硬件光标，不进截图" "$LOG" \
	&& pass "日志说明它待在硬件光标上（不进截图）" \
	|| fail "日志没说硬件光标"

# ------------------------------------------------------------ 4) 到点缩回（动画）
mark "4. 到点自己缩回（200ms 线性补间，不是跳）"
# 保持期 700ms：晃完 0.75s 开始缩，缩的过程 200ms —— 这 200ms 里密集采样，
# 应该能量到 1 和 2 之间的中间倍数（跳回原尺寸的话只能量到 2 和 1）。
msg cursor-shake >/dev/null
sleep 0.75
SEEN=$(python3 - <<'PYS'
import json, subprocess, time
def scale():
    out = subprocess.run(["./build/vidowmsg", "--raw", "cursor"],
                         capture_output=True, text=True).stdout
    try:
        return float(json.loads(out)["result"]["scale"])
    except Exception:
        return 1.0
vals = []
t0 = time.time()
while time.time() - t0 < 0.45:
    vals.append(scale())
    time.sleep(0.02)
mid = [v for v in vals if 1.05 < v < 1.95]
print("%d %d %.3f" % (len(mid), len(vals), mid[0] if mid else 1.0))
PYS
)
read -r NMID NTOT FIRST <<< "$SEEN"
if [ "$NMID" -ge 2 ]; then
	pass "缩回是补间：采样到 $NMID 个中间倍数（共 $NTOT 次采样，第一个 $FIRST）"
else
	fail "缩回看着是跳的：$NTOT 次采样里只有 $NMID 个中间值"
fi
grep -aq "摇晃: 光标开始缩回" "$LOG" && pass "日志记了缩回开始" \
	|| fail "日志里没有缩回记录"
sleep 0.4
[ "$(cfield magnified)" = "false" ] && pass "缩完 magnified=false" \
	|| fail "一直没还原"
[ "$(cfield image)" = "$IMG0" ] && pass "尺寸回到 $IMG0" \
	|| fail "还原后的尺寸是 $(cfield image)"
grep -aq "摇晃: 光标还原" "$LOG" && pass "日志记了还原" || fail "日志里没有还原记录"

# ------------------------------------------------------------ 5) 接着晃接着大
mark "5. 接着晃接着大：线性加（2→3→4→5→6），不是乘（2→4→8→16）"
W0=${IMG0%x*}
H0=${IMG0#*x}
OK=1
for i in 1 2 3 4 5; do
	msg cursor-shake >/dev/null
	sleep 0.3
	WANT=$(python3 -c "print('%dx%d' % (round($W0*($i+1)), round($H0*($i+1))))")
	GOT=$(cfield image)
	if [ "$GOT" = "$WANT" ]; then
		pass "第 $i 圈：$GOT（$(python3 -c "print('%gx' % ($i+1))")）"
	else
		fail "第 $i 圈是 $GOT，期望 $WANT"
		OK=0
	fi
done
[ "$OK" = "1" ] && pass "每一圈只 +1（线性加）" || fail "某一圈没长对"
[ "$(cfield target)" = "6" ] && pass "target=6（六圈的目标倍数）" \
	|| fail "target 是 $(cfield target)，期望 6"

mark "5b. 放大本身也是补间（不是一步到位）"
msg cursor-shake >/dev/null
sleep 0.05
read -r NOW TGT <<< "$(python3 -c '
import json, subprocess
out = subprocess.run(["./build/vidowmsg", "--raw", "cursor"],
                     capture_output=True, text=True).stdout
r = json.loads(out)["result"]
print("%.3f %.3f" % (r["scale"], r["target"]))')"
if python3 -c "import sys; sys.exit(0 if 6.05 < $NOW < 6.95 else 1)"; then
	pass "晃完 50ms 量到 $NOW（在 6 和 $TGT 之间，正在补间）"
else
	fail "晃完 50ms 就是 $NOW（期望在 6 和 7 之间）"
fi
sleep 0.3
[ "$(cfield image)" = "$(scale_size "$W0" "$H0" 7)" ] \
	&& pass "补间走完到 $(cfield image)（7x）" \
	|| fail "补间走完是 $(cfield image)，期望 7x"

mark "5c. 一路晃到屏幕封顶"
for _ in $(seq 1 40); do
	msg cursor-shake >/dev/null
	sleep 0.02
done
sleep 0.3
CAP=$(python3 -c "print(min(1280.0/$W0, 720.0/$H0))")
WANT=$(python3 -c "print('%dx%d' % (round($W0*$CAP), round($H0*$CAP)))")
[ "$(cfield image)" = "$WANT" ] && pass "封顶在 $WANT（屏幕 1280x720 装得下的最大倍数）" \
	|| fail "封顶后是 $(cfield image)，期望 $WANT"
grep -aq "摇晃: 屏幕只有 1280x720.*放不下，到 .* 为止" "$LOG" \
	&& pass "日志说明了被屏幕尺寸封顶" || fail "日志没说明封顶"
sleep 1.0

# ------------------------------------------------------------ 6) 不进截图
mark "6. 不进截图（grim 不带 -c）"
msg move-cursor 300 100 >/dev/null
sleep 0.3
grim "$RUNDIR/plain.png" >/dev/null 2>&1
read -r N X Y W H <<< "$(ink "$RUNDIR/plain.png")"
[ "$N" = "0" ] && pass "普通截图里没有光标（0 个非背景像素）" \
	|| fail "普通截图里有 $N 个非背景像素（$W x $H @ $X,$Y）"
msg cursor-shake >/dev/null
sleep 0.3
grim "$RUNDIR/plain-mag.png" >/dev/null 2>&1
read -r N X Y W H <<< "$(ink "$RUNDIR/plain-mag.png")"
[ "$N" = "0" ] && pass "**放大中**的普通截图里也没有光标" \
	|| fail "放大中的普通截图里出现了 $N 个像素（$W x $H @ $X,$Y）"
sleep 1.2

# ------------------------------------------------------------ 7) 显式指定才进
mark "7. 显式要光标才进截图（grim -c）"
grim -c "$RUNDIR/cur.png" >/dev/null 2>&1
read -r N1 X1 Y1 W1 H1 <<< "$(ink "$RUNDIR/cur.png")"
[ "$N1" -gt 0 ] && pass "grim -c 抓到了光标（$N1 个像素，$W1 x $H1 @ $X1,$Y1）" \
	|| fail "grim -c 也没抓到光标"
[ "$W1" = "${IMG0%x*}" ] && [ "$H1" = "${IMG0#*x}" ] \
	&& pass "没放大时外接矩形就是光标图大小（$IMG0）" \
	|| fail "外接矩形是 $W1 x $H1，期望 $IMG0"
# 放大后拍一张：等补间走到位、而且拍的过程中尺寸不许变（见 magnify_and_grab）。
IMG_MAG=$(magnify_and_grab "$RUNDIR/cur-mag.png" 2 "$IMG0" -c)
[ "$IMG_MAG" = "$BIG" ] && pass "放大后光标图是 $BIG（正好 2 倍）" \
	|| fail "放大后光标图是 $IMG_MAG，期望 $BIG"
read -r N2 X2 Y2 W2 H2 <<< "$(ink "$RUNDIR/cur-mag.png")"
[ "$W2" = "${IMG_MAG%x*}" ] && [ "$H2" = "${IMG_MAG#*x}" ] \
	&& pass "放大后外接矩形是 $IMG_MAG（和拍到的光标图一样大）" \
	|| fail "放大后外接矩形是 $W2 x $H2，期望 $IMG_MAG"
[ "$N2" -gt "$N1" ] && pass "像素更多了（$N1 → $N2）" \
	|| fail "放大后像素没变多（$N1 → $N2）"
EXP_X=$(( 2 * X1 - 300 ))
EXP_Y=$(( 2 * Y1 - 100 ))
[ "$X2" = "$EXP_X" ] && [ "$Y2" = "$EXP_Y" ] \
	&& pass "热点没跑（左上角 $X1,$Y1 → $X2,$Y2，箭头尖还在 300,100）" \
	|| fail "热点跑了：左上角 $X1,$Y1 → $X2,$Y2，期望 $EXP_X,$EXP_Y"
sleep 1.2

# ------------------------------------------------------------ 8) 抓帧让位（软光标）
mark "8. 抓帧让位：软光标那条路（真机上放大超出硬件平面就是它）"
stop_vidow
export VIDOW_TEST_SOFT_CURSOR=1   # 钩子：把这块屏钉死在软光标上
start_vidow ""
msg move-cursor 300 100 >/dev/null
sleep 0.3
[ "$(cfield hardware)" = "false" ] && pass "光标现在是软光标（会画进屏幕）" \
	|| fail "钩子没生效，光标还是硬件光标"
grim "$RUNDIR/soft.png" >/dev/null 2>&1
read -r N0 X0 Y0 W0B H0B <<< "$(ink "$RUNDIR/soft.png")"
[ "$N0" = "0" ] && pass "普通抓帧里没有光标（抓帧是 vidow 自己渲染的一帧，本来就不含光标）" \
	|| fail "普通抓帧里有 $N0 个非背景像素（$W0B x $H0B）"
# 放大到 6 倍再拍（晃 5 下）。两张图都要在保持期内拍完，所以都用
# magnify_and_grab：等补间到位 + 拍的过程中尺寸不许变。
IMG_S=$(magnify_and_grab "$RUNDIR/soft-mag.png" 6 "$IMG0")
grep -aq "摇晃: 光标放大.*软光标" "$LOG" \
	&& pass "日志说明是软光标（$IMG_S）" || fail "日志没说软光标"
read -r N1 X1 Y1 W1 H1 <<< "$(ink "$RUNDIR/soft-mag.png")"
[ "$N1" = "0" ] && pass "**放大中的普通抓帧里也没有光标**（软光标也一样）" \
	|| fail "放大中的抓帧里还有 $N1 个像素（$W1 x $H1）"
IMG_S=$(magnify_and_grab "$RUNDIR/soft-c.png" 6 "$IMG0" -c)
read -r N2 X2 Y2 W2 H2 <<< "$(ink "$RUNDIR/soft-c.png")"
[ "$W2" = "${IMG_S%x*}" ] && [ "$H2" = "${IMG_S#*x}" ] \
	&& pass "grim -c（显式要）抓到的是放大图 $IMG_S" \
	|| fail "grim -c 抓到的是 $W2 x $H2，期望 $IMG_S"
sleep 1.2
unset VIDOW_TEST_SOFT_CURSOR

# ------------------------------------------------------------ 9) 客户端设的光标
mark "9. 客户端自己设的光标（wl_pointer.set_cursor）也能放大并还原"
stop_vidow
start_vidow ""
python3 tools/cursorwin.py > "$RUNDIR/cursorwin.log" 2>&1 &
CURWIN_PID=$!
for _ in $(seq 1 40); do
	grep -aq "place cursorwin" "$LOG" && break
	sleep 0.25
done
PLACE=$(grep -a "place cursorwin" "$LOG" | tail -1)
if [ -z "$PLACE" ]; then
	fail "cursorwin 没起来"
else
	pass "客户端窗口起来了：${PLACE##*place }"
	WX=$(echo "$PLACE" | sed 's/.*-> \([0-9-]*\),[0-9-]* .*/\1/')
	WY=$(echo "$PLACE" | sed 's/.*-> [0-9-]*,\([0-9-]*\) .*/\1/')
	msg move-cursor $((WX + 120)) $((WY + 120)) >/dev/null
	sleep 0.4
	IMG_C=$(cfield image)
	[ "$IMG_C" != "$IMG0" ] \
		&& pass "指针进了客户端窗口，光标图换成它设的（$IMG0 → $IMG_C）" \
		|| fail "客户端设的光标没生效（还是 $IMG_C）"
	msg cursor-shake >/dev/null
	sleep 0.35
	BIG_C=$(scale_size "${IMG_C%x*}" "${IMG_C#*x}" 2)
	[ "$(cfield image)" = "$BIG_C" ] \
		&& pass "客户端的光标照样放大 2 倍（$IMG_C → $BIG_C）" \
		|| fail "客户端光标没放大（$(cfield image)，期望 $BIG_C）"
	sleep 1.2
	[ "$(cfield image)" = "$IMG_C" ] \
		&& pass "还原成客户端那张（$IMG_C），没变成主题箭头" \
		|| fail "还原后是 $(cfield image)，期望客户端那张 $IMG_C"
fi

	# 9b) 放大中焦点切到另一个「自己设光标」的窗口：新客户端 set_cursor 不许把
	#     放大缩回去（老行为：vidow_cursor_fx_set_surface 直接把放大整个收掉）。
	python3 tools/cursorwin.py second > "$RUNDIR/second.log" 2>&1 &
	SECOND_PID=$!
	for _ in $(seq 1 40); do
		grep -aq "place second" "$LOG" && break
		sleep 0.25
	done
	SPLACE=$(grep -a "place second" "$LOG" | tail -1)
	if [ -z "$SPLACE" ]; then
		fail "第二个客户端窗口没起来（9b 测不到东西）"
	else
		SX=$(echo "$SPLACE" | sed 's/.*-> \([0-9-]*\),[0-9-]* .*/\1/')
		SY=$(echo "$SPLACE" | sed 's/.*-> [0-9-]*,\([0-9-]*\) .*/\1/')
		msg move-cursor $((WX + 120)) $((WY + 120)) >/dev/null
		sleep 0.2
		# 保持期内（700ms）先晃着、再切过去，窗口不等人
		msg cursor-shake >/dev/null
		sleep 0.05
		msg cursor-shake >/dev/null
		sleep 0.05
		BIGTARGET=$(cfield target)
		msg move-cursor $((SX + 120)) $((SY + 120)) >/dev/null
		sleep 0.3
		[ "$(cfield magnified)" = "true" ] \
			&& pass "切到另一个窗口后还放大着（magnified=true）" \
			|| fail "切窗口把放大缩回去了（用户报的那个）"
		[ "$(cfield target)" = "$BIGTARGET" ] \
			&& pass "放大目标没变：$BIGTARGET 倍" \
			|| fail "放大目标从 $BIGTARGET 变成 $(cfield target)"
		grep -aq "光标图换了，放大图跟着换" "$LOG" \
			&& pass "日志记下了「光标图换了，放大图跟着换」（新图被重新放大）" \
			|| fail "没有「光标图换了」的日志（新图没被放大）"
	fi
	kill "$SECOND_PID" 2>/dev/null
kill "$CURWIN_PID" 2>/dev/null

# ------------------------------------------------------------ 10) 关掉
mark "10. cursor_shake = false"
stop_vidow
start_vidow "cursor_shake = false"
msg move-cursor 300 100 >/dev/null
shake
sleep 0.3
[ "$(cfield magnified)" = "false" ] && pass "关掉之后晃不放大" \
	|| fail "关掉了还是放大了"
msg cursor-shake | grep -aq 'cursor_shake' \
	&& pass "cursor-shake 被拒，理由里点了 cursor_shake" \
	|| fail "cursor-shake 的拒绝理由没说清楚"
grep -aq "config: cursor_shake=off" "$LOG" && pass "启动日志里写着 off" \
	|| fail "启动日志没写 cursor_shake=off"

# ------------------------------------------------------------ 11) 倍数
mark "11. cursor_shake_scale"
stop_vidow
start_vidow "cursor_shake_scale = 1.5"
msg move-cursor 300 100 >/dev/null
IMG=$(cfield image)
SHAKE15=$(scale_size "${IMG%x*}" "${IMG#*x}" 1.5)
shake
sleep 0.35
[ "$(cfield image)" = "$SHAKE15" ] && pass "1.5 倍：$IMG → $SHAKE15" \
	|| fail "1.5 倍的尺寸是 $(cfield image)，期望 $SHAKE15"
sleep 1.2

stop_vidow
start_vidow "cursor_shake_scale = 20"
msg move-cursor 300 100 >/dev/null
IMG=$(cfield image)
W0=${IMG%x*}
H0=${IMG#*x}
WANT=$(python3 -c "print('%dx%d' % (round($W0*min(20.0, 1280.0/$W0, 720.0/$H0)), round($H0*min(20.0, 1280.0/$W0, 720.0/$H0))))")
shake
sleep 0.35
[ "$(cfield image)" = "$WANT" ] && pass "20 倍（没到屏幕上限就是 20 倍）：$IMG → $WANT" \
	|| fail "20 倍时是 $(cfield image)，期望 $WANT"
# 晃到封顶之后仍然在硬件平面上（不然就进截图了，正是要避免的那件事）
for _ in 1 2 3 4 5; do msg cursor-shake >/dev/null; sleep 0.2; done
sleep 0.3
case "$(cfield hardware)" in
true) pass "晃到屏幕封顶也还在硬件平面上（$(cfield image)）" ;;
*)    fail "晃到封顶掉成软光标了（$(cfield image)）" ;;
esac

# ------------------------------------------------------------ 12) 日志干净
mark "12. 日志干净"
grep -aqE 'assert|abort|Assertion' "$LOG" && fail "日志里有 assert/abort" \
	|| pass "日志里没有 assert / abort"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
