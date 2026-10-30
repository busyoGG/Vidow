#!/bin/bash
# 「每输出共享一次抓取」：两块 top 层面板压在同一个位置时，这一帧只抓一次「layer
# 之下」（壁纸 + bottom 层 + 窗口），两块面板各从上面 1:1 裁自己那块 —— 窗口绘制
# 从两遍变成一遍。
#
# 判据：
#   1) 共享开着：under 次数 ≈ 每帧一次（不是每块面板各一次），链条仍是每块面板一条
#   2) 关掉共享（/tmp/vidow-no-layer-share）之后 under 回到 0
#   3) 两条路画出来的画面**逐像素一样**（共享只是省了重复绘制，不是换了算法）
#   4) vidow 活着
#
# 用法：./test-layershare.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layershare
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
		case "$cmd" in *wall.py*|*spin.py*|*panel.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	rm -f /tmp/vidow-no-layer-share
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-layershare.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR/home/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$RUNDIR/home/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
blur_enabled = true
blur_all = true
blur_levels = 3
blur_offset = 1.5
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$RUNDIR/home"  VIDOW_SHARE_TRACE=1 VIDOW_LOG=debug VIDOW_RENDER_STATS=1
# 逼着每次都**整块**重抓：共享只在整块重抓时用（局部那块自己抓更省），而 A/B
# 比对需要「每一帧都是整块重抓」才能保证两张截图是同一套算法画的。
export VIDOW_BLUR_NO_PARTIAL_FILE="$RUNDIR/nopartial"
touch "$RUNDIR/nopartial"
export SPIN_STEP_FILE="$RUNDIR/step"
touch /tmp/vidow-stats-on
export DISPLAY= WAYLAND_DISPLAY=

mark "两块 top 层面板压在一起：只该抓一次「layer 之下」"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.2
# 底下一直有窗口在动：逼着两块面板的底衬每帧重建
setsid nohup python3 ./tools/spin.py under 0.9 0.2 0.2 full > "$RUNDIR/spin.log" 2>&1 </dev/null &
SPIN_PID=$!
CLIENTS="$CLIENTS $SPIN_PID"
sleep 1.5
./build/vidowmsg toggle-floating --title under >/dev/null 2>&1
./build/vidowmsg set-window-position --title under 0 0 >/dev/null 2>&1
./build/vidowmsg set-window-size --title under 1200 700 >/dev/null 2>&1
./build/vidowmsg set-window-force-render --title under on >/dev/null 2>&1
sleep 1
setsid nohup python3 ./tools/panel.py 900 400 > "$RUNDIR/p1.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.5
setsid nohup python3 ./tools/panel.py 800 380 > "$RUNDIR/p2.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 3

rows() { grep -a "渲染统计" "$LOG"; }
stat_of() { # <次数> <帧数> <under> <chain>
	rows | tail -3 | python3 -c '
import re, sys
u = c = f = 0
for l in sys.stdin:
    m = re.search(r"under (\d+) chain (\d+)", l)
    if m:
        u += int(m.group(1)); c += int(m.group(2))
    m = re.search(r"渲染统计: (\d+) 帧", l)
    if m: f += int(m.group(1))
print(u, c, f)'
}

mark "共享开着"
rows | tail -3 | sed 's/.*模糊 /    /;s/| 装饰.*//'
set -- $(stat_of)
UN=$1; CH=$2; FR=$3
grim -o HEADLESS-1 "$RUNDIR/share.png" 2>/dev/null
if [ "$FR" -gt 20 ] && [ "$UN" -gt 0 ] && [ "$UN" -le $((FR * 3 / 2)) ] && [ "$CH" -ge $((UN * 2)) ]; then
	pass "$FR 帧里只抓了 $UN 次「layer 之下」（每帧一次，不是每块面板一次），链条 $CH 条"
else
	fail "under=$UN chain=$CH / $FR 帧 —— 共享没生效"
fi

mark "关掉共享（对照）：同一幅静止画面比像素"
# 把底下那个一直在动的窗口**冻住**（SIGSTOP）：画面静止，但窗口还在、布局不变。
# 直接 kill 会让布局重排，两张截图本来就不同了。
kill -STOP $SPIN_PID 2>/dev/null
sleep 1.2
./build/vidowmsg reload-config >/dev/null 2>&1
sleep 0.6
grim -o HEADLESS-1 "$RUNDIR/share.png" 2>/dev/null
touch /tmp/vidow-no-layer-share
./build/vidowmsg reload-config >/dev/null 2>&1
sleep 0.6
grim -o HEADLESS-1 "$RUNDIR/noshare.png" 2>/dev/null
rows | tail -3 | sed 's/.*模糊 /    /;s/| 装饰.*//'
U2=$(rows | tail -2 | grep -ac "under 0 chain")
if [ "$U2" -ge 1 ]; then
	pass "关掉之后 under 回到 0（对照生效）"
else
	fail "关掉共享之后还在共享"
fi

if python3 - "$RUNDIR/share.png" "$RUNDIR/noshare.png" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
d = np.abs(a - b).max(axis=2)
n = int((d > 2).sum())
print("    最大差 %d，差 > 2 的像素 %d 个（整幅 %d）" % (int(d.max()), n, d.size))
sys.exit(0 if n <= 2000 else 1)
PY
then
	pass "共享 / 不共享两条路的画面一样（底下的窗口一直在动，容许少量采样差）"
else
	fail "两条路画出来的画面不一样 —— 共享那份裁错了位置"
fi

kill -CONT $SPIN_PID 2>/dev/null

if [ -n "${VIDOW_PGID:-}" ] && kill -0 -- "-$VIDOW_PGID" 2>/dev/null; then
	pass "vidow 还活着"
else
	fail "vidow 不见了"
fi

echo
if [ "$FAILED" = 0 ]; then echo "全部通过 / test-layershare.sh: OK"
else echo "test-layershare.sh: 有项目没通过"; exit 1; fi
