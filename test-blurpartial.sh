#!/bin/bash
# 「只重跑受损的那一小块」和「整块重跑」必须**逐像素一样**（只是省了算）。
#
# 场景：壁纸 + 半透明浮窗 A（背后是模糊，模糊里画着 B）+ 压在 A 上的浮窗 B
# （tools/spin.py step：每帧只让窗口里 80x40 那一小块失效，内容什么时候变由测试
# 控制 —— SPIN_STEP_FILE 一出现就变一次，之后一直不动）。
#
# **同一个进程、同一个场景**里比对：先让 A 一直走局部重跑，截一张；然后建出
# VIDOW_BLUR_NO_PARTIAL_FILE 指的文件（下一次重抓就整块重跑），再截一张。两张图
# 必须是同一幅画面 —— 客户端、位置、内容一个字都没变，变的只有重抓的方式。
#
# 判据：
#   1) 前提：切换之前 A 每帧都在重抓，而且日志写着「局部重跑」
#   2) 画面静止后局部重跑动的块确实小（远小于整块区域 1152x1024）
#   3) 切换之后日志里全是「整块重跑」，一次局部重跑都没有（开关真的生效）
#   4) 两张截图逐像素比对：差 > 2 的像素 **0 个**（局部重跑漏了哪一块、新旧模糊值
#      之间留了缝、或者哪块被叠了第二次，这里当场差出来）
#   5) vidow 活着
#
# 用法：./test-blurpartial.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurpartial-test
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
	{ echo "test-blurpartial.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR/home/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$RUNDIR/home/.config/vidow/config.lua" <<'CFG'
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
export HOME="$RUNDIR/home" VIDOW_LOG=debug VIDOW_RENDER_STATS=1 VIDOW_BLUR_TRACE=1
export VIDOW_BLUR_NO_PARTIAL_FILE="$RUNDIR/nopartial"
export DISPLAY= WAYLAND_DISPLAY=

mark "「只重跑受损那一小块」和「整块重跑」得逐像素一样"

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

float_at() { # <标题> <x> <y>
	local t="$1" x="$2" y="$3"
	for _ in $(seq 1 30); do
		./build/vidowmsg windows 2>/dev/null | grep -q "\"$t\"" && break
		sleep 0.3
	done
	for _ in $(seq 1 12); do
		./build/vidowmsg toggle-floating --title "$t" >/dev/null 2>&1
		local f=$(./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
print(next((w["floating"] for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), "?"))' "$t")
		[ "$f" = "True" ] && break
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
# B：每帧只动窗口里 80x40 那一小块（局部 20,20），内容由测试控制变一次
SPIN_STEP_FILE="$RUNDIR/step" \
	setsid nohup python3 ./tools/spin.py spin 1 0 0 step 20 20 > "$RUNDIR/spin.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6
float_at spin 150 150
sleep 1.5
touch "$RUNDIR/step"      # 内容变一次（就这一小块）
sleep 2.0                 # 让这一小块重跑几遍、画面彻底静止

# 阶段一：局部重跑
OFF1=$(wc -c < "$LOG")
grim -o HEADLESS-1 "$RUNDIR/partial.png" 2>/dev/null
sleep 1.2
# 阶段二：切到整块重跑（同一个进程、同一个场景、同一个客户端）
touch "$RUNDIR/nopartial"
sleep 0.2
OFF2=$(wc -c < "$LOG")
grim -o HEADLESS-1 "$RUNDIR/full.png" 2>/dev/null
sleep 0.5

slice() { # <起点> <终点> <正则> -> 行数
	{ tail -c +$(($1 + 1)) "$LOG" | head -c $(($2 - $1)) | grep -ac "$3"; } 2>/dev/null
}

NPART=$(slice "$OFF1" "$OFF2" '局部重跑')
NREC=$(slice "$OFF1" "$OFF2" 'blur: own region wtop')
if [ "$NPART" -ge 20 ] && [ "$NREC" -ge 20 ]; then
	pass "前提：切换之前 A 每帧都在重抓（$NREC 次），其中 $NPART 次走局部重跑"
else
	fail "切换之前 A 只重抓了 $NREC 次（局部 $NPART 次）—— 场景不对，下面几条不算数"
fi

BIG=$(tail -c +$((OFF1 + 1)) "$LOG" | head -c $(($OFF2 - $OFF1)) |
	grep -a "局部重跑" | tail -200 | python3 -c '
import re, sys
n = big = mx = tot = 0
for line in sys.stdin:
    m = re.search(r"局部重跑 (\d+)x(\d+)", line)
    if m:
        n += 1
        w, h = int(m.group(1)), int(m.group(2))
        mx = max(mx, w, h)
        tot += w * h
        if w > 400 or h > 400:
            big += 1
print(n, big, mx, tot // n if n else 0)')
set -- $BIG
N=$1; NBIG=$2; NMAX=$3; NAVG=$4
if [ "$N" -ge 50 ] && [ "$NBIG" = "0" ]; then
	pass "画面静止后 $N 次局部重跑：动的块最大 $NMAX 像素、平均 $NAVG 像素（整块区域 1152x1024 = 118 万像素）"
else
	fail "画面静止后 $N 次局部重跑里有 $NBIG 次动的块 > 400（最大 $NMAX）—— 没真省下来"
fi

END=$(wc -c < "$LOG")
NPART2=$(slice "$OFF2" "$END" '局部重跑')
NFULL2=$(slice "$OFF2" "$END" '整块重跑')
if [ "$NFULL2" -ge 10 ] && [ "$NPART2" = "0" ]; then
	pass "切到整块之后：$NFULL2 次全是整块重跑、局部重跑 0 次"
else
	fail "切到整块之后还有 $NPART2 次局部重跑（整块 $NFULL2 次）—— 开关没生效"
fi

if python3 - "$RUNDIR/partial.png" "$RUNDIR/full.png" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
d = np.abs(a - b).max(axis=2)
n = int((d > 2).sum())
print("    最大差 %d，差 > 2 的像素 %d 个（整幅 %d 像素）" % (int(d.max()), n, d.size))
sys.exit(0 if n == 0 else 1)
PY
then
	pass "同一场景下局部重跑和整块重跑的画面逐像素一样（差 > 2 的像素 0 个）"
else
	fail "两种方式画出来的画面不一样 —— 局部重跑漏了哪一块、留了缝、或者叠了第二次"
fi

grep -a "blur: own region wtop" "$LOG" | tail -2 | sed 's/^/    /'

# 只看**测试自己起的那个**：以前这里是 `pgrep -x vidow`，而用户桌面会话也叫
# 这个名字 —— 测试自己的 vidow 早死了，这条照样 PASS（永远测不出来的假绿）。
if { [ -n "${VIDOW_PID:-}" ] && kill -0 "$VIDOW_PID" 2>/dev/null; } ||
{ [ -n "${VIDOW_PGID:-}" ] && kill -0 -- "-$VIDOW_PGID" 2>/dev/null; }; then
	pass "vidow 还活着"
else
	fail "vidow 死了（$(tail -3 "$LOG" | tr '\n' '|')）"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-blurpartial.sh: OK"
else
	echo "test-blurpartial.sh: 有失败"
fi
exit "$FAILED"
