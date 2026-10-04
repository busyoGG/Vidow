#!/bin/bash
# 模糊和高 DPI 倍率的对位。
#
# 靶子：tools/dot.py 在背景层画两个亮点（一个在右下角、会被下面的面板盖住，一个
# 在左上角、始终看得见），tools/panel.py 在右下角画一块 50% 透明的 top 层面板。
#
# 为什么值得单跑一个测试：场景图里的一切（节点坐标、目标尺寸）都是**逻辑像素**，
# 而模糊的底衬缓冲是**设备像素**。少了倍率换算时，1.7 / 2 倍屏上底衬只在左上角
# 画了 1/倍率 那么大的、缩小的一块，其余全空 —— 于是：
#   * 模糊采到的是空像素，级数越多越透明（看起来「越糊越没模糊感」）
#   * layer 背后的模糊采样位置对不上，整块错位
# 1 倍屏上这两件事都不会发生，所以只有拿倍率跑才发现。
#
# 判据：
#   1) 底衬铺满整块屏（alpha 全覆盖）—— 这条就是那个 bug 的直接回归
#   2) 高倍率下面板背后的亮点真的被糊开了（比未被盖住的那个亮斑宽）
#   3) 两个亮斑的屏上位置差 == 它们真实位置差（面板背后那份没被平移）
#   4) 模糊金字塔里亮斑的位置 == 它的物理位置（半分辨率坐标）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurscale-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
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
	rm -f /tmp/vidow-bd-*.raw /tmp/vidow-blur-*.raw /tmp/vidow-bd-*.txt /tmp/vidow-blur-*.txt
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi" 2>/dev/null || { echo "test-blurscale.sh: 需要 python3-gobject"; exit 1; }
python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-blurscale.sh: 需要 gtk-layer-shell"; exit 1; }
command -v grim >/dev/null || { echo "test-blurscale.sh: 需要 grim"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY= VIDOW_BLUR_DUMP=12

# 物理坐标（设备像素）固定，逻辑坐标按倍率换算 —— 1 倍和 2 倍的靶子是同一块
PROBE_X=1000; PROBE_Y=620   # 面板背后那个亮斑
REF_X=200;    REF_Y=200     # 面板外那个亮斑（参照）
PANEL_W=400;  PANEL_H=200   # 右下角面板（物理尺寸）

run_once() {
	local scale="$1" tag="$2"
	local px=$(python3 -c "print(round($PROBE_X/$scale))")
	local py=$(python3 -c "print(round($PROBE_Y/$scale))")
	local rx=$(python3 -c "print(round($REF_X/$scale))")
	local ry=$(python3 -c "print(round($REF_Y/$scale))")
	local pw=$(python3 -c "print(round($PANEL_W/$scale))")
	local ph=$(python3 -c "print(round($PANEL_H/$scale))")
	cat > "$CFGDIR/.config/vidow/config.lua" <<EOF
outputs = "HEADLESS-1:$scale"
blur_enabled = true
blur_levels = 4
blur_offset = 1.5
blur_alpha_min = 0.15
window_opacity = 0.5
EOF
	rm -f "$LOG" /tmp/vidow-bd-*.raw /tmp/vidow-blur-*.raw /tmp/vidow-bd-*.txt /tmp/vidow-blur-*.txt
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	if ! grep -aq "vidow ready" "$LOG"; then
		echo "test-blurscale.sh: vidow 没起来（scale=$scale）"; grep -a "ERROR" "$LOG" | head -5; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)

	setsid nohup python3 ./tools/dot.py "$px" "$py" "$rx" "$ry" > "$RUNDIR/dot.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do grep -aq "layer map dot" "$LOG" && break; sleep 0.25; done
	sleep 0.7
	setsid nohup python3 ./tools/panel.py "$pw" "$ph" > "$RUNDIR/panel.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do grep -aq "layer map testpanel" "$LOG" && break; sleep 0.25; done
	sleep 1.4
	grep -aq "layer map testpanel" "$LOG" || fail "scale=$scale：面板没上屏"
	grim -o HEADLESS-1 "$RUNDIR/shot-$tag.png" 2>/dev/null || fail "scale=$scale：grim 失败"
	# 最后一份底衬 / 模糊金字塔快照（重建过好几轮，取编号最大的那份）
	local last=$(ls /tmp/vidow-blur-*.raw 2>/dev/null |
		sed 's/.*-\([0-9]*\)\.raw/\1/' | sort -n | tail -1)
	if [ -n "${last:-}" ]; then
		cp "/tmp/vidow-bd-$last.raw" "$RUNDIR/bd-$tag.raw"
		cp "/tmp/vidow-bd-$last.txt" "$RUNDIR/bd-$tag.txt"
		cp "/tmp/vidow-blur-$last.raw" "$RUNDIR/blur-$tag.raw"
		cp "/tmp/vidow-blur-$last.txt" "$RUNDIR/blur-$tag.txt"
	else
		fail "scale=$scale：没拿到底衬快照"
	fi
	echo "$scale" > "$RUNDIR/scale-$tag.txt"

	kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	CLIENT_PIDS=""
	VIDOW_PID=""
	sleep 0.3
}

mark "两轮：1 倍屏（对照组）和 2 倍屏（bug 只在倍率 ≠ 1 时露出来）"
run_once 1 "s1"
run_once 2 "s2"
run_once 1.7 "s17"

python3 - "$RUNDIR" "$PROBE_X" "$PROBE_Y" "$REF_X" "$REF_Y" <<'PY'
import sys
import numpy as np
from PIL import Image

rundir, px, py, rx, ry = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), \
	float(sys.argv[4]), float(sys.argv[5])

def load_raw(path):
	with open(path[:-4] + '.txt') as f:
		w, h, _fmt = f.read().split()
	w, h = int(w), int(h)
	a = np.fromfile(path, dtype=np.uint8)[:w * h * 4].reshape(h, w, 4)
	return a.astype(np.float32) / 255.0

def centroid(g, cx, cy, r, base):
	cx, cy, r = int(round(cx)), int(round(cy)), int(r)
	sub = g[cy - r:cy + r, cx - r:cx + r]
	w = np.clip(sub - base, 0, None)
	tot = w.sum()
	if tot <= 0:
		return None
	yy, xx = np.mgrid[cy - r:cy + r, cx - r:cx + r]
	return ((xx * w).sum() / tot, (yy * w).sum() / tot)

def sigma(g, cx, cy, r):
	cx, cy, r = int(round(cx)), int(round(cy)), int(r)
	sub = g[cy - r:cy + r, cx - r:cx + r]
	base = np.median(sub[:5, :])
	w = np.clip(sub - base, 0, None)
	tot = w.sum()
	yy, xx = np.mgrid[-r:r, -r:r]
	mx = (xx * w).sum() / tot
	my = (yy * w).sum() / tot
	return float(np.sqrt(((xx - mx) ** 2 * w).sum() / tot)), float(tot)

ok = True
for tag in ('s1', 's2', 's17'):
	scale = float(open(f"{rundir}/scale-{tag}.txt").read())
	bd = load_raw(f"{rundir}/bd-{tag}.raw")
	bl = load_raw(f"{rundir}/blur-{tag}.raw")
	shot = np.asarray(Image.open(f"{rundir}/shot-{tag}.png").convert('RGB')
		).astype(np.float32).mean(axis=2)
	alpha = bd[..., 3]
	print(f"\n  scale={scale}  底衬 {bd.shape[1]}x{bd.shape[0]}  "
		f"alpha 均值 {alpha.mean():.4f}")

	# 1) 底衬铺满整块屏：模糊的采样范围就是它，缺一块就是「采到空像素」
	cover = float((alpha >= 0.99).mean())
	if cover >= 0.99:
		print(f"  PASS  scale={scale}：底衬铺满整块屏（alpha≥0.99 的像素 {cover * 100:.1f}%）")
	else:
		print(f"  FAIL  scale={scale}：底衬只盖住 {cover * 100:.1f}%（模糊在空像素上采样）")
		ok = False

	# 2) 面板背后的亮斑真的被糊开了：比没被盖住的那个宽
	pc = centroid(shot, px, py, 70, 20)
	rc = centroid(shot, rx, ry, 50, 32)
	ps, pt = sigma(shot, pc[0], pc[1], 60)
	rs, rt = sigma(shot, rc[0], rc[1], 50)
	if ps > rs * 1.25:
		print(f"  PASS  scale={scale}：面板背后的亮斑糊开了（σ {ps:.1f} vs 未被盖住 {rs:.1f}）")
	else:
		print(f"  FAIL  scale={scale}：面板背后没糊（σ {ps:.1f} vs 未被盖住 {rs:.1f}）")
		ok = False

	# 3) 错位：两个亮斑在屏上的差 == 它们真实物理位置差
	dx = pc[0] - rc[0] - (px - rx)
	dy = pc[1] - rc[1] - (py - ry)
	if abs(dx) <= 2.0 and abs(dy) <= 2.0:
		print(f"  PASS  scale={scale}：面板背后的底衬没错位（偏差 {dx:+.2f},{dy:+.2f} 像素）")
	else:
		print(f"  FAIL  scale={scale}：面板背后的底衬错位（偏差 {dx:+.2f},{dy:+.2f} 像素）")
		ok = False

	# 4) 模糊金字塔里亮斑的位置 == 它的物理位置（半分辨率）
	g = bl[..., :3].mean(axis=2)
	base = float(np.median(g[10:20, 300:400]))
	bc = centroid(g, px / 2, py / 2, 40, base)
	if bc is None:
		print(f"  FAIL  scale={scale}：模糊金字塔里找不到亮斑"); ok = False
	elif abs(bc[0] - px / 2) <= 3 and abs(bc[1] - py / 2) <= 3:
		print(f"  PASS  scale={scale}：模糊金字塔里亮斑在正确位置"
			f"（偏差 {bc[0] - px / 2:+.2f},{bc[1] - py / 2:+.2f}）")
	else:
		print(f"  FAIL  scale={scale}：模糊金字塔里亮斑错位"
			f"（偏差 {bc[0] - px / 2:+.2f},{bc[1] - py / 2:+.2f}）")
		ok = False

sys.exit(0 if ok else 1)
PY
[ $? = 0 ] || FAILED=1

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-blurscale.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
