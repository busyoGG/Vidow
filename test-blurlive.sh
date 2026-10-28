#!/bin/bash
# 「屏幕另一头的客户端一直在交帧，不该让别处的模糊底衬每帧重抓」。
#
# 场景：壁纸 + 两个互相重叠的静止浮窗（其中一个的私有底衬里画着另一个，它每帧都
# 要重抓）+ 屏幕另一头一个**不停重画**的窗口（tools/spin.py，模拟视频 / 浏览器）。
#
# 判据（VIDOW_RENDER_STATS=1 那行统计，只看 spin 跑起来之后的那几行）：
#   1) 不停重画的那个窗口真的在重画（两张截图的它那块区域不一样）
#   2) 重叠的那两个窗口的私有底衬**没有**跟着重抓：own < 30（约 2.5 秒）
#      —— 修之前「世界代次一涨就全重抓」，实测每帧 2 次（≈ 250+）
#   3) vidow 全程活着
#
# 用法：./test-blurlive.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurlive-test
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
	{ echo "test-blurlive.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

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

mark "屏幕另一头有个不停重画的窗口时，重叠窗口的模糊底衬不该每帧重抓"

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

float_at() { # <标题> <x> <y> <宽> <高>
	local t="$1" x="$2" y="$3" w="$4" h="$5"
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
	./build/vidowmsg set-window-size --title "$t" "$w" "$h" >/dev/null 2>&1
	sleep 0.3
	./build/vidowmsg set-window-position --title "$t" "$x" "$y" >/dev/null 2>&1
	sleep 0.6
}

# 两个互相重叠的静止浮窗：它们在彼此的私有底衬里，谁动谁就得让对方重抓。
setsid nohup python3 ./tools/pattern.py wback > "$RUNDIR/pat.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6
float_at wback 100 100 600 400
setsid nohup python3 ./tools/sheet.py wtop 0 0 0 0.5 > "$RUNDIR/sheet.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6
float_at wtop 200 200 600 400
sleep 1.0

# 屏幕另一头：不停重画的窗口（跟上面两个都不重叠）
setsid nohup python3 ./tools/spin.py spin 1 0 0 > "$RUNDIR/spin.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6
float_at spin 900 80 300 200
sleep 1.0

# 到这里重叠窗口的私有底衬都建好了：从这里开始数
OFF=$(wc -c < "$LOG")
grim -o HEADLESS-1 "$RUNDIR/a.png" 2>/dev/null
sleep 1.2
grim -o HEADLESS-1 "$RUNDIR/b.png" 2>/dev/null
sleep 1.2

# 1) spin 真的在重画吗（它那块区域两张图不一样）
if python3 - "$RUNDIR/a.png" "$RUNDIR/b.png" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
d = np.abs(a - b).max(axis=2)
n = int((d > 8).sum())
ys, xs = np.where(d > 8)
if n:
    print("    变化像素 %d，范围 x %d..%d y %d..%d" %
          (n, xs.min(), xs.max(), ys.min(), ys.max()))
# 这个场景里唯一会动的东西就是那个不停重画的窗口：整图比，变了就说明它真的在重画
sys.exit(0 if n >= 500 else 1)
PY
then
	pass "不停重画的那个窗口真的在重画（它那块区域两张截图不一样）"
else
	fail "那个窗口没在重画 —— 这条测不到东西（截图那块区域没变化）"
fi

# 2) 重叠窗口的私有底衬不该跟着重抓
tail -c +$((OFF + 1)) "$LOG" | grep -a "渲染统计" > "$RUNDIR/stats.txt"
SUM=$(python3 - "$RUNDIR/stats.txt" <<'PY'
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
)
set -- $SUM
OWN=$1; FRAMES=$2
if [ "$FRAMES" -ge 20 ] 2>/dev/null; then
	pass "这 2.4 秒里渲染了 $FRAMES 帧（说明确实在跑）"
else
	fail "只渲染了 $FRAMES 帧 —— 时间窗太短，下面那条判据不算数"
fi
if [ "$OWN" -lt 30 ] 2>/dev/null; then
	pass "重叠窗口的私有底衬没跟着重抓：$OWN 次 / $FRAMES 帧（修前每帧 2 次 ≈ $((FRAMES * 2)) 次）"
else
	fail "重叠窗口的私有底衬重抓了 $OWN 次 / $FRAMES 帧 —— 屏幕另一头的重画把别处的底衬也作废了"
fi

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
	echo "全部通过 / test-blurlive.sh: OK"
else
	echo "test-blurlive.sh: 有失败"
fi
exit "$FAILED"
