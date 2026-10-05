#!/bin/bash
# 窗口打开动画：动画期间画出来的**必须是那个窗口本身**，而且得是「装饰过的那个
# 窗口」——不能是一块比窗口大一圈的硬边大矩形，也不能是一块不吃 window_opacity
# 的不透明大饼。
#
# 病症：动画着色器原来是喂**原始内容贴图**、却铺在「窗口 + 阴影留白」那么大一块
# 渲染目标上 —— 内容被拉伸到整块画布（每边多出 shadow_radius 那点留白），而且
# 那一趟根本不走装饰着色器：没有圆角、没有边框、没有模糊，也不吃 window_opacity。
# 于是动画期间看到的是一个比窗口大、纯色、直角边的大矩形，动画一结束装饰才接上，
# 看着就是「先冒一块大矩形，再缩回窗口」。
#
# 靶子：tools/wall.py 壁纸（渐变 + 硬边方块），tools/bluewin.py 一个纯蓝窗口，
# shader_open = zoom（0.8 → 1.0 放大淡入），window_opacity = 0.5，
# # shadow_radius = 24 且把阴影颜色设成全透明：留白要（这样子元素被拉伸出去的那
# 一圈看得见），阴影不要（不然软阴影会把包围盒撑大，量不准）。
#
# 取样时刻按日志里的 `anim open start` 对齐：客户端 map 要多久、快门要多久都不
# 影响判据落在动画的哪个进度上。
#
# 判据：
#   1) 窗口真的上屏了
#   2) 动画中段那帧比基准窗口窄（zoom 真在缩，不是一上来就满尺寸）
#   3) 动画后段那帧不比基准窗口大（大矩形会宽高各多出 2×pad = 56px）
#   4) 动画后段那帧窗口中心还是半透明的（不吃 window_opacity 的那条路是纯蓝）
#      先要确认取样真的落在后段（宽度 ≥ 基准的 85%），否则这条判据等于没测
#
# 用法：./test-openanim.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-openanim-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
CFG="$CFGDIR/.config/vidow/config.lua"
FRAMES="$RUNDIR/frames"
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
	{ echo "test-openanim.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }
command -v grim >/dev/null || { echo "test-openanim.sh: 需要 grim"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow/shaders" "$RUNDIR/run" "$FRAMES"
chmod 700 "$RUNDIR/run"
cp shaders/zoom.frag "$CFGDIR/.config/vidow/shaders/zoom.frag"

cat > "$CFG" <<'CFGEOF'
outputs = "HEADLESS-1:1"
scale = "1"
gaps = 10
window_opacity = 0.5
shader_open = "zoom"
window_open_ms = 4000
window_close_ms = 200
open_alpha_from = 0.0
open_slide_px = 0
easing = "linear"
decor_enabled = true
corner_radius = 12
border_width = 2
shadow_radius = 24
shadow_color = "#00000000"   -- 留白要，阴影不要：可见范围就只剩窗口自己
blur_enabled = true
blur_levels = 3
blur_offset = 2
blur_alpha_min = 0.15
CFGEOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
# debug：取样靠日志里的 `anim open start` 对时
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

BIN=${VIDOW_BIN:-./build/vidow}

setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "test-openanim.sh: vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

# 壁纸先上，拍一张「没有窗口」的底
setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map wall" "$LOG" && break; sleep 0.25; done
sleep 0.8
grim -o HEADLESS-1 "$FRAMES/base.png" 2>/dev/null || fail "grim 失败（base）"

# 窗口：等它真的开动画（日志里那一刻），再按动画进度取样
setsid nohup python3 ./tools/bluewin.py > "$RUNDIR/win.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
started=""
for _ in $(seq 1 100); do
	if grep -aq "anim open start" "$LOG"; then started=1; break; fi
	sleep 0.1
done
if [ -z "$started" ]; then
	fail "没等到入场动画开始（窗口没上屏？）"
	tail -3 "$RUNDIR/win.log"
else
	T0=$(python3 -c 'import time; print(time.monotonic())')
	# 0.5×时长 = 动画中段（zoom 缩着、也淡到看得见了），0.90×时长 = 后段（这里才是大矩形露馅
	# 的地方：缩放已经接近 1，拉伸多出来的那圈还全在）。快门自己也要几十毫秒，
	# 所以实际取样都再往后一点。
	wait_until() {
		python3 - "$1" "$T0" <<'PY'
import sys, time
goal = float(sys.argv[1]) - (time.monotonic() - float(sys.argv[2]))
time.sleep(goal if goal > 0 else 0)
PY
	}
	wait_until 1.8;  grim -o HEADLESS-1 "$FRAMES/early.png" 2>/dev/null
	wait_until 3.6;  grim -o HEADLESS-1 "$FRAMES/late1.png" 2>/dev/null
	wait_until 3.75; grim -o HEADLESS-1 "$FRAMES/late2.png" 2>/dev/null
	wait_until 3.9;  grim -o HEADLESS-1 "$FRAMES/late3.png" 2>/dev/null
	wait_until 5.6;  grim -o HEADLESS-1 "$FRAMES/final.png" 2>/dev/null
	grep -a "anim open" "$LOG" | tail -2
fi

python3 - "$RUNDIR" <<'PY'
import sys
import numpy as np
from PIL import Image

rundir = sys.argv[1] + '/frames/'
ok = True

def say(good, msg):
	global ok
	print(('  PASS  ' if good else '  FAIL  ') + msg)
	ok = ok and good

def rgb(name):
	return np.asarray(Image.open(rundir + name).convert('RGB')).astype(np.int16)

def box(a, base, thr=40, edge=8, frac=0.3):
	"""可见窗口的包围盒：跟壁纸那张比，差得够多的地方。

	两个坑都要躲：屏幕最外几行/列上壁纸自己跟后来重画的那遍差 70 多（GTK 的
	边），整条边会跳出来撑满包围盒；窗口边上也会顺出几个像素的碎点。所以外圈
	先裁掉，再要求整列/整行有 30% 的像素变了才算窗口 —— 窗口是几百像素高的实心
	东西，碎点和边都过不了这一关。"""
	m = np.abs(a - base).max(axis=2) > thr
	m[:edge, :] = m[-edge:, :] = False
	m[:, :edge] = m[:, -edge:] = False
	cols = np.where(m.sum(axis=0) > frac * m.shape[0])[0]
	rows = np.where(m.sum(axis=1) > frac * m.shape[1])[0]
	if len(cols) == 0 or len(rows) == 0:
		return None
	return (int(cols.min()), int(rows.min()), int(cols.max()), int(rows.max()))

base = rgb('base.png')
final = rgb('final.png')
fb = box(final, base)
if fb is None:
	say(False, '基准帧里没找到窗口（窗口没上屏？）')
	sys.exit(1)

fx0, fy0, fx1, fy1 = fb
fw, fh = fx1 - fx0 + 1, fy1 - fy0 + 1
say(fw > 200 and fh > 200, '窗口上屏了：基准帧里那块 %dx%d' % (fw, fh))

# 中段那帧只有一半透明（叠在深色壁纸上差 ~30），阈值放宽到 18；密度那一关还在，
# 碎点噪声过不去。
early = box(rgb('early.png'), base, thr=18)
if early is None:
	say(False, '动画中段那帧没抓到窗口')
else:
	ew = early[2] - early[0] + 1
	say(ew < fw * 0.95,
		'动画中段窗口是缩着的：宽 %d < 基准 %d 的 95%%（%.0f%%）'
		% (ew, fw, 100.0 * ew / fw))

late = []
for name in ('late1.png', 'late2.png', 'late3.png'):
	b = box(rgb(name), base)
	if b is not None:
		late.append((name, b))
if not late:
	say(False, '动画后段一帧都没抓到窗口')
else:
	# 取样确实落在后段：只要有一帧的宽度接近基准就说明对时是对的
	widest = max(b[2] - b[0] + 1 for _, b in late)
	say(widest >= fw * 0.85,
		'取样落在动画后段：最宽的一帧 %d ≥ 基准 %d 的 85%%（%.0f%%）'
		% (widest, fw, 100.0 * widest / fw))

	# 三帧都要「不比窗口大」：旧代码隔一帧才跑一次动画着色器，只取一帧有 50%
	# 概率正好落在没跑的那半帧上（看着就正常了），所以必须连拍几张一起判。
	over = []
	for name, (x0, y0, x1, y1) in late:
		w, h = x1 - x0 + 1, y1 - y0 + 1
		if w > fw + 3 or h > fh + 3:
			over.append('%s %dx%d' % (name[:-4], w, h))
	say(not over,
		'动画后段每一帧都不比窗口大（窗口 %dx%d）：%s'
		% (fw, fh, '、'.join(over) if over else '三帧都合格'))

	# 窗口中心那块：内容是纯蓝、window_opacity = 0.5，叠出来红通道 ≈ 27 以上；
	# 不吃 window_opacity 的那条路（动画着色器直接吞原始内容）中心是纯蓝，红 ≈ 0。
	reds = []
	for name, (x0, y0, x1, y1) in late:
		reds.append((name[:-4], int(rgb(name)[(y0 + y1) // 2, (x0 + x1) // 2][0])))
	worst = min(reds, key=lambda kv: kv[1])
	say(worst[1] >= 10,
		'动画后段每一帧窗口中心都还是半透明的（最低 %s = %d，壁纸红 %d；'
		'< 10 = 不吃 window_opacity 的纯蓝大饼）'
		% (worst[0], worst[1], base[(fy0 + fy1) // 2, (fx0 + fx1) // 2][0]))

sys.exit(0 if ok else 1)
PY
rc=$?
[ $rc -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-openanim.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
