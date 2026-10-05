#!/bin/bash
# 关闭动画的两条底线：**看得见**、**不带窗口装饰**。
#
# 1) 窗口：默认的关闭曲线是 out_elastic（q 弹），它会冲过 1（t≈0.26 到 1、t≈0.45
#    冲到 1.09）。把这条曲线喂给「1 → close_alpha_to」那条透明度斜坡，透明度在动画
#    头 1/4 就掉到 0 并被夹住 —— 剩下 3/4 只在动位移和缩放，屏幕上什么都没有，看着
#    就是「窗口没有关闭动画」（默认 140ms 时实际可见时间只有 35ms）。所以透明度必须
#    走**线性进度**，位移和缩放照旧走曲线。这条只能看屏幕发现：曲线本身、日志、
#    「anim close start」全都在，窗口却已经没了。
#
# 2) layer surface：面板的快照曾经被套上**窗口**装饰（边框 + 阴影 + 圆角），而 layer
#    自己就是内容、形状由客户端画（开动画那边也没装饰）。淡出的尾巴上面板自己那点半
#    透明在模糊底衬上本来就没什么对比、先看不见了，边框和阴影压在没糊过的壁纸上反而
#    更显眼 —— 屏幕上就是「比面板大一点的空心矩形」。所以 layer 的快照不装饰。
#
# 靶子：tools/dot.py 纯平 0.12 灰壁纸（亮点挪到右下角不碍事）、tools/bluewin.py 纯蓝
# 窗口（盖住左半边，正中那个像素一定在窗口里，缩到 0.82 也还在）、tools/panel.py 一块
# 400x200 的 top 层 50% 面板贴在左上角。底是纯平的，所以「面板外面那几个像素比背景暗
# 了多少」直接就是装饰留下的痕迹。
#
# 判据：
#   1) 窗口关闭动画中段还看得见：t=0.3 时 α > 0.5、t=0.5 时 α > 0.25（修之前两处都是 0）
#   2) 透明度随线性进度往下走（t=0.3 比 t=0.5 亮），动画结束后窗口彻底没了
#   3) 面板关闭动画中段面板还看得见（快照真的在淡出，不是「啪」一下没了）
#   4) 面板外面（右边 4~14px 那条带）不比壁纸暗 —— 没有阴影圈，也没有比面板大的框
#   5) 动画结束后面板和面板外都是干净的壁纸
#
# 用法：./test-closeanim.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-closeanim-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FRAMES="$RUNDIR/frames"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import numpy, PIL" 2>/dev/null || { echo "test-closeanim.sh: 需要 python3-numpy + pillow"; exit 1; }
python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-closeanim.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }
command -v grim >/dev/null || { echo "test-closeanim.sh: 需要 grim"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run" "$FRAMES"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

# 关闭曲线 / 缩放 / 位移 / 透明度全是**默认值**，只有时长拉长了好量（快门有几十
# 毫秒延迟，140ms 的动画量不准）。拉长不改判据：回归点是「曲线和透明度怎么配」。
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1 }
decor_enabled = true
blur_enabled = true
window_close_ms = 2000
close_easing = "out_elastic"
close_alpha_to = 0
close_scale_to = 0.82
close_slide_px = 14
layer_close_ms = 1200
CFG

BIN=${VIDOW_BIN:-./build/vidow}
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-closeanim.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

# 按日志对时：客户端 map 多久、快门多久都不影响判据落在曲线的哪一段
wait_until() {
	python3 - "$1" "$2" <<'PY'
import sys, time
goal = float(sys.argv[1]) - (time.monotonic() - float(sys.argv[2]))
time.sleep(goal if goal > 0 else 0)
PY
}
# 等某条日志**新增**一条，返回它出现的时刻（monotonic 秒）
wait_log_after() { # <grep 模式> <基线条数>
	local i n
	for i in $(seq 1 500); do
		n=$(grep -ac "$1" "$LOG" 2>/dev/null || true)
		if [ "$n" -gt "$2" ]; then python3 -c 'import time; print(time.monotonic())'; return 0; fi
		sleep 0.02
	done
	return 1
}
count() { grep -ac "$1" "$LOG" 2>/dev/null || true; }

setsid nohup python3 ./tools/dot.py 1200 700 > "$RUNDIR/dot.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "layer map dot" "$LOG" && break; sleep 0.25; done
sleep 0.6

mark "1/2 窗口关闭动画：默认曲线下还看得见吗"
setsid nohup python3 ./tools/bluewin.py > "$RUNDIR/win.log" 2>&1 </dev/null &
WIN_PID=$!
CLIENT_PIDS="$CLIENT_PIDS $WIN_PID"
sleep 2.5
grim -o HEADLESS-1 "$FRAMES/win-before.png" 2>/dev/null

n0=$(count "anim close start")
kill "$WIN_PID" 2>/dev/null
T=$(wait_log_after "anim close start" "$n0") || fail "没等到窗口的关闭动画开始"
if [ -n "${T:-}" ]; then
	wait_until 0.60 "$T"; grim -o HEADLESS-1 "$FRAMES/win-close-30.png" 2>/dev/null
	wait_until 1.00 "$T"; grim -o HEADLESS-1 "$FRAMES/win-close-50.png" 2>/dev/null
	wait_until 2.40 "$T"; grim -o HEADLESS-1 "$FRAMES/win-after.png" 2>/dev/null
fi

mark "2/2 面板关闭动画：有没有比面板大一圈的框"
setsid nohup python3 ./tools/panel.py 400 200 tl > "$RUNDIR/panel.log" 2>&1 </dev/null &
PANEL_PID=$!
CLIENT_PIDS="$CLIENT_PIDS $PANEL_PID"
for _ in $(seq 1 40); do grep -aq "layer map testpanel" "$LOG" && break; sleep 0.25; done
sleep 2.0
grim -o HEADLESS-1 "$FRAMES/panel-before.png" 2>/dev/null

n1=$(count "anim close start")
kill "$PANEL_PID" 2>/dev/null
P=$(wait_log_after "anim close start" "$n1") || fail "没等到面板的关闭动画开始"
if [ -n "${P:-}" ]; then
	wait_until 0.40 "$P"; grim -o HEADLESS-1 "$FRAMES/panel-close-33.png" 2>/dev/null
	wait_until 0.75 "$P"; grim -o HEADLESS-1 "$FRAMES/panel-close-62.png" 2>/dev/null
	wait_until 1.60 "$P"; grim -o HEADLESS-1 "$FRAMES/panel-after.png" 2>/dev/null
fi

python3 - "$FRAMES" <<'PY'
import os, sys
import numpy as np
from PIL import Image

frames = sys.argv[1] + '/'
ok = True

def say(cond, msg):
	global ok
	print('  %s  %s' % ('PASS' if cond else 'FAIL', msg))
	if not cond:
		ok = False

def load(name):
	return np.asarray(Image.open(frames + name).convert('RGB')).astype(float)

def alpha_at(img):
	"""窗口正中那个像素的蓝通道 → 透明度（壁纸 31，纯蓝窗口 ≈ 239）"""
	return (img[359, 322, 2] - 31.0) / (239.0 - 31.0)

def bg_mean(img):
	"""离面板很远的壁纸（右下角），当每一帧自己的基准"""
	return img[300:400, 900:1100].mean()

def panel_blueness(img):
	"""面板内部「比壁纸蓝多少」：面板是 0.1/0.1/0.15 的半透明，蓝分量比红高"""
	px = img[95:105, 195:205]
	return (px[:, :, 2] - px[:, :, 0]).mean()

def outside_mean(img):
	"""紧贴面板外面那圈：右边 400~406px、下边 200~206px 的带 —— 窗口装饰的
	阴影圈就落在这几个像素上（再往外就淡没了，量不到）"""
	return min(img[60:140, 400:406].mean(), img[200:206, 60:340].mean())

def edge_brightness(img):
	"""面板自己最右那 4px（边框环画在这儿）"""
	return img[95:105, 396:400].mean()

def inner_brightness(img):
	return img[95:105, 190:200].mean()

before = load('win-before.png')
a_before = alpha_at(before)
say(a_before > 0.9, '窗口上屏了：关闭前 α ≈ %.2f' % a_before)

a30 = alpha_at(load('win-close-30.png'))
a50 = alpha_at(load('win-close-50.png'))
a_after = alpha_at(load('win-after.png'))
say(a30 > 0.5, '关闭动画 30%% 处窗口还看得见：α ≈ %.2f（> 0.5）' % a30)
say(a50 > 0.25, '关闭动画 50%% 处窗口还看得见：α ≈ %.2f（> 0.25）' % a50)
say(a30 > a50, '透明度随进度单调往下走：%.2f → %.2f' % (a30, a50))
say(abs(a_after) < 0.05, '动画结束后窗口彻底没了：α ≈ %.2f' % a_after)

panel = load('panel-before.png')
c33 = load('panel-close-33.png')
c62 = load('panel-close-62.png')
after = load('panel-after.png')

b0 = panel_blueness(panel)
b33 = panel_blueness(c33)
b62 = panel_blueness(c62)
b_end = panel_blueness(after)
say(b0 > 3.0, '面板上屏了：面板内比壁纸蓝 %.1f' % b0)
say(b33 > 1.5, '关闭动画 33%% 处面板还看得见：比壁纸蓝 %.1f（快照真的在淡出）' % b33)
say(b33 < b0 and b62 < b33, '面板在淡出：%.1f → %.1f → %.1f' % (b0, b33, b62))

# 面板外那一圈：窗口装饰的阴影圈就落在这儿（修之前是 -4 到 -7 那种暗带）
o33 = outside_mean(c33) - bg_mean(c33)
o62 = outside_mean(c62) - bg_mean(c62)
o_end = outside_mean(after) - bg_mean(after)
say(o33 > -2.0, '关闭动画 33%% 处面板外没有阴影圈：比壁纸暗 %.1f（< 2）' % -o33)
say(o62 > -2.0, '关闭动画 62%% 处面板外没有阴影圈：比壁纸暗 %.1f（< 2）' % -o62)

# 面板边缘那一圈：窗口装饰的边框环（比面板自己亮，2px 画在面板最边上）
e33 = edge_brightness(c33) - inner_brightness(c33)
e62 = edge_brightness(c62) - inner_brightness(c62)
say(e33 < 3.0, '关闭动画 33%% 处面板边上没有边框环：边比里面亮 %.1f（< 3）' % e33)
say(e62 < 3.0, '关闭动画 62%% 处面板边上没有边框环：边比里面亮 %.1f（< 3）' % e62)

say(b_end < 0.5 and o_end > -1.0,
	'动画结束后面板内外都是干净的壁纸：蓝 %.1f、外面 %.1f' % (b_end, o_end))

sys.exit(0 if ok else 1)
PY
rc=$?

echo
if [ "$rc" = 0 ] && [ "$FAILED" = 0 ]; then
	echo "test-closeanim.sh: 全部通过"
else
	echo "test-closeanim.sh: 有失败项"
	FAILED=1
fi
exit $FAILED
