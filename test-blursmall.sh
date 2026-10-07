#!/bin/bash
# 「客户端只动了窗口里的一小块，压着它的那片模糊不该整块重抓」。
#
# 场景：壁纸 + 一个半透明浮窗 A（它背后是模糊，模糊里画着 B）+ 一个跟它重叠的
# 浮窗 B（tools/spin.py small：每帧只让窗口里 80x40 那一小块失效，模拟「只有一条
# 进度条 / 一个光标在动」）。
#
# 判据（VIDOW_RENDER_STATS=1 那行统计，只看标记之间的那几行）：
#   1) 前提：A 真的建过私有底衬（说明两个窗口真的重叠了）
#   2) B 真的只动了那一小块（两张截图的变化像素全在 B 的窗口框里，而且很小）
#   3) 那一小块在 A 的区域外时：A 的私有底衬**没有**重抓（own < 10）
#      —— 修之前按整块窗口算损伤，B 的窗口压着 A，每帧都要重抓（≈ 帧数）
#   4) 同一小块挪到 A 的区域里时：确实重抓（own ≥ 20）—— 别「优化」成漏账
#   5) vidow 全程活着
#
# 用法：./test-blursmall.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blursmall-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENTS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENTS; do kill "$p" 2>/dev/null; done
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in *sheet.py*|*pattern.py*|*spin.py*|*wall.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-blursmall.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 2, mode = "2560x1440" }
blur_enabled = true
blur_all = true
blur_levels = 4
blur_offset = 1.5
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug VIDOW_RENDER_STATS=1
export DISPLAY= WAYLAND_DISPLAY=

mark "客户端只动一小块时，压着它的模糊底衬不该整块重抓"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
grep -aq "vidow ready" "$LOG" || {
	echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.2

geom_of() { # <标题> -> "x y w h floating"（布局逻辑坐标）
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
w = next((w for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), None)
if w is None:
    print("? ? ? ? ?")
else:
    g = w["geometry"]
    print(g["x"], g["y"], g["width"], g["height"], w["floating"])' "$1"
}

float_at() { # <标题> <x> <y>
	local t="$1" x="$2" y="$3"
	for _ in $(seq 1 30); do
		./build/vidowmsg windows 2>/dev/null | grep -q "\"$t\"" && break
		sleep 0.3
	done
	for _ in $(seq 1 12); do
		./build/vidowmsg toggle-floating --title "$t" >/dev/null 2>&1
		[ "$(geom_of "$t" | awk '{print $5}')" = "True" ] && break
		sleep 0.3
	done
	./build/vidowmsg set-window-position --title "$t" "$x" "$y" >/dev/null 2>&1
	sleep 0.6
}

# A：半透明浮窗（它背后那片模糊里画着 B）
setsid nohup python3 ./tools/sheet.py wtop 0 0 0 0.5 > "$RUNDIR/sheet.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6
float_at wtop 100 100

# B：每帧只让窗口里那一小块失效（80x40，窗口局部 280,240）
setsid nohup python3 ./tools/spin.py spin 1 0 0 small > "$RUNDIR/spin.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6
float_at spin 350 380
sleep 1.0

echo "    A(wtop) $(geom_of wtop)    B(spin) $(geom_of spin)"

# 阶段一：那一小块（布局 ≈ 610,600 起，见 spin.py 的 BX/BY）在 A 的区域
# （≈ 0..576 x 0..512，见日志里那行「own region wtop ...」）外
OFF1=$(wc -c < "$LOG")
grim -o HEADLESS-1 "$RUNDIR/a1.png" 2>/dev/null
sleep 1.2
grim -o HEADLESS-1 "$RUNDIR/b1.png" 2>/dev/null
sleep 1.2
OFF2=$(wc -c < "$LOG")   # 阶段一到此为止（下面开始摆第二个客户端）

# 阶段二：再开一个同样的客户端，但那一小块挪到窗口左上角（局部 20,20），整个
# 窗口摆在 A 上面 -> 那一小块落在 A 的区域里，A 就该重抓了。
setsid nohup python3 ./tools/spin.py spin2 0 0 1 small 20 20 > "$RUNDIR/spin2.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6
float_at spin2 150 150
sleep 1.0
echo "    B2(spin2) $(geom_of spin2)"
OFF3=$(wc -c < "$LOG")   # 阶段二从这儿开始（第二个客户端已经就位）
sleep 1.2
sleep 1.2

# 1) 前提：A 真的建过私有底衬吗（没建的话下面几条都在测空气）
if [ "$(grep -ac 'blur: own region wtop' "$LOG")" -ge 1 ]; then
	pass "A 建过私有底衬（说明它跟别的窗口真的重叠了，下面几条才算数）"
else
	fail "A 从来没建过私有底衬 —— 两个窗口没重叠，这条测不到东西"
fi

# 2) B 真的只动了那一小块吗：变化像素必须全在 B 的窗口框里，而且很小
set -- $(geom_of spin)
GX=$1; GY=$2; GW=$3; GH=$4
if python3 - "$RUNDIR/a1.png" "$RUNDIR/b1.png" "$GX" "$GY" "$GW" "$GH" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
gx, gy, gw, gh = (int(v) * 2 for v in sys.argv[3:7])   # scale = 2
d = np.abs(a - b).max(axis=2)
ys, xs = np.where(d > 8)
n = len(xs)
print("    变化像素 %d" % n)
if n == 0:
    sys.exit(1)
print("    变化范围 x %d..%d y %d..%d（B 的窗口框 %d..%d x %d..%d）"
      % (xs.min(), xs.max(), ys.min(), ys.max(),
         gx, gx + gw - 1, gy, gy + gh - 1))
ok = (n <= 60000 and
      xs.min() >= gx and xs.max() < gx + gw and
      ys.min() >= gy and ys.max() < gy + gh)
sys.exit(0 if ok else 1)
PY
then
	pass "那个客户端真的只重画了窗口里的一小块（变化像素全在它窗口框里，且只有几千个）"
else
	fail "变化像素不像「只动一小块」—— 这条测不到东西"
fi

# <起点字节> <终点字节> -> "own frames"（这段日志里的渲染统计）
stats() {
	{ tail -c +$(($1 + 1)) "$LOG" | head -c $(($2 - $1)) | grep -a "渲染统计"; } \
		2>/dev/null > "$RUNDIR/stats.txt"
	python3 - "$RUNDIR/stats.txt" <<'PY'
import re, sys
own = frames = 0
for line in open(sys.argv[1]):
    m = re.search(r'渲染统计: (\d+) 帧', line)
    if m:
        frames += int(m.group(1))
    m = re.search(r'own (\d+)', line)
    if m:
        own += int(m.group(1))
print(own, frames)
PY
}

# <起点字节> <终点字节> <正则> -> 这段日志里匹配的行数
slice_grep() {
	{ tail -c +$(($1 + 1)) "$LOG" | head -c $(($2 - $1)) | grep -ac "$3"; } 2>/dev/null
}

set -- $(stats "$OFF1" "$OFF2")
OWN1=$1; FRAMES1=$2
if [ "$FRAMES1" -ge 20 ] 2>/dev/null; then
	pass "阶段一渲染了 $FRAMES1 帧（说明确实在跑）"
else
	fail "阶段一只渲染了 $FRAMES1 帧 —— 时间窗太短，下面那条判据不算数"
fi
END=$(wc -c < "$LOG")
WTOP1=$(slice_grep "$OFF1" "$OFF2" 'own region wtop')
if [ "$WTOP1" -lt 10 ] 2>/dev/null; then
	pass "区域外的一小块损伤没让 A 重抓：A 重抓 $WTOP1 次 / $FRAMES1 帧（修前按整块窗口算 ≈ $FRAMES1 次）"
else
	fail "区域外的一小块损伤让 A 重抓了 $WTOP1 次 / $FRAMES1 帧 —— 损伤还是按整块窗口算的"
fi

set -- $(stats "$OFF3" "$END")
OWN2=$1; FRAMES2=$2
WTOP2=$(slice_grep "$OFF3" "$END" 'own region wtop.*150,150')
if [ "$WTOP2" -ge 20 ] 2>/dev/null; then
	pass "损伤落在 A 的区域里时确实重抓：A 重抓 $WTOP2 次带着那一笔（150,150）/ $FRAMES2 帧"
else
	fail "损伤落在 A 的区域里却只重抓了 $WTOP2 次 —— 漏账（模糊会停在旧内容上）"
fi

# 日志里那行「损伤 WxH」是这套判断的全部依据，留两行做证据
grep -a "blur: own region" "$LOG" | tail -2 | sed 's/^/    /'

if pgrep -x vidow >/dev/null 2>&1; then
	pass "vidow 还活着"
else
	fail "vidow 死了（$(tail -3 "$LOG" | tr '\n' '|')）"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-blursmall.sh: OK"
else
	echo "test-blursmall.sh: 有失败"
fi
exit "$FAILED"
