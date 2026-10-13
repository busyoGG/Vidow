#!/bin/bash
# 底衬里的**内容**是按设备像素 1:1 画的，不是「逻辑像素直接当设备像素」、也不是被
# 放大了一倍。
#
# 靶子：tools/cells.py（16 逻辑像素的非周期随机块 + 三条彩色横带，横带在逻辑
# 80/360/640）。为什么必须用非周期壁纸：周期图案放大 2 倍之后看着还是同一个图案
# （16px 的块变 32px 的块），拿互相关量位移/缩放峰值全是噪声 —— 上一轮就是这么得出
# 「模糊正常」的错误结论的。随机块没有周期，「同坐标比大小」才有意义；三条横带是
# 各向同性模糊的**重心不变量**，糊得多厉害重心都钉在原处，所以它能当尺子。
#
# 判据（scale=2 那条就是「被放大一倍」的直接回归）：
#   1) 底衬缓冲 == 壁纸纹理 × scale（同坐标，逐像素）—— 内容在设备像素网格上 1:1
#   2) 底衬缓冲里块边界的间距 == scale × 16 设备像素（放大一倍会变成 32 / 64）
#   3) 窗口那块底衬里的横带行 == 同一张截图上**屏上原样**的横带行（±4 设备像素）
#      —— 这一条不需要任何参考图，是「同一坐标、同一张图」的对照
#   4) 底衬里三条横带的间距 == scale × 280 设备像素（放大一倍会变成 560 / 1120）
#   5) 底衬缓冲尺寸 == 这块输出的设备尺寸
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurtexture-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
	rm -f /tmp/vidow-blur-*.raw /tmp/vidow-blur-*.txt \
		/tmp/vidow-bd-*.raw /tmp/vidow-bd-*.txt
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-blurtexture.sh: 需要 gtk-layer-shell"; exit 1; }
command -v grim >/dev/null || { echo "test-blurtexture.sh: 需要 grim"; exit 1; }
command -v kitty >/dev/null || { echo "test-blurtexture.sh: 需要 kitty"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER_ALLOW_SOFTWARE=1
export HOME="$CFGDIR" VIDOW_LOG=debug DISPLAY= WAYLAND_DISPLAY=
export VIDOW_BLUR_DUMP=12

# 参考纹理（逻辑像素，1:1）：判据 1 拿它乘 scale 去比底衬
python3 ./tools/cells.py --png "$RUNDIR/cells.png" > /dev/null ||
	{ echo "test-blurtexture.sh: 生成参考纹理失败"; exit 1; }

# 两块屏的**逻辑**尺寸都是 1280x720（mode 按倍率给），所以壁纸铺满整块屏、
# 窗口落在同一个逻辑位置 —— 变的只有设备像素。
run_once() {
	local scale="$1" tag="$2" mode="$3"
	cat > "$CFGDIR/.config/vidow/config.lua" <<EOF
vidow.output{ name = "HEADLESS-1", scale = $scale, mode = "$mode" }
blur_enabled = true
blur_all = true
blur_xray = false
blur_levels = 3
blur_offset = 1.5
blur_strength = 1
window_opacity = 0.55
window_move_ms = 0
background_color = "#000000ff"
EOF
	rm -f "$LOG" /tmp/vidow-bd-*.raw /tmp/vidow-blur-*.raw \
		/tmp/vidow-bd-*.txt /tmp/vidow-blur-*.txt
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	if ! grep -aq "vidow ready" "$LOG"; then
		echo "test-blurtexture.sh: vidow 没起来（scale=$scale）"; grep -a "ERROR" "$LOG" | head -5; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)

	setsid nohup python3 ./tools/cells.py > "$RUNDIR/cells.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do grep -aq "layer map cells" "$LOG" && break; sleep 0.25; done
	sleep 1.0
	# 空白终端（不画内容，只有半透明的底），压在壁纸左边
	setsid nohup kitty -o background_opacity=0.55 -o font_size=18 \
		-o initial_window_width=40c -o initial_window_height=20c \
		sh -c 'sleep 600' > "$RUNDIR/kitty.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do
		./build/vidowmsg windows 2>/dev/null | grep -aq '"title": "sh"' && break
		sleep 0.25
	done
	sleep 2.0
	./build/vidowmsg windows | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["title"].startswith("sh"):
        g = w["geometry"]
        print("%d %d %d %d" % (g["x"], g["y"], g["width"], g["height"]))
' > "$RUNDIR/geom-$tag.txt"
	[ -s "$RUNDIR/geom-$tag.txt" ] || fail "scale=$scale：没找到窗口"
	grep -a "blur:" "$LOG" | sed 's/.*blur:/blur:/' | sort | uniq -c |
		sed "s/^/    scale=$scale /"
	grim -o HEADLESS-1 "$RUNDIR/shot-$tag.png" 2>/dev/null ||
		fail "scale=$scale：grim 失败"
	local last=$(ls /tmp/vidow-bd-*.raw 2>/dev/null |
		sed 's/.*-\([0-9]*\)\.raw/\1/' | sort -n | tail -1)
	if [ -n "${last:-}" ]; then
		cp "/tmp/vidow-bd-$last.raw" "$RUNDIR/bd-$tag.raw"
		cp "/tmp/vidow-bd-$last.txt" "$RUNDIR/bd-$tag.txt"
	else
		fail "scale=$scale：没拿到底衬快照"
	fi
	echo "$scale $mode" > "$RUNDIR/scale-$tag.txt"

	kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	CLIENT_PIDS=""
	VIDOW_PID=""
	sleep 0.3
}

mark "两轮：1 倍屏（对照组，逻辑==设备）和 2 倍屏（用户报的那块）"
run_once 1 "s1" "1280x720"
run_once 2 "s2" "2560x1440"

python3 - "$RUNDIR" <<'PY'
import sys
import numpy as np
from PIL import Image

rundir = sys.argv[1]
tex = (np.asarray(Image.open(f"{rundir}/cells.png").convert('RGB'))
	.astype(np.float32) / 255.0)
th, tw = tex.shape[:2]
ok = True

def load_raw(path):
	with open(path[:-4] + '.txt') as f:
		w, h, _fmt = f.read().split()
	w, h = int(w), int(h)
	a = np.fromfile(path, dtype=np.uint8)[:w * h * 4].reshape(h, w, 4)
	# 转储是 BGRA（0x34325241）
	return a[..., [2, 1, 0, 3]].astype(np.float32) / 255.0

def upscale_nearest(img, s):
	return np.repeat(np.repeat(img, s, axis=0), s, axis=1)

def cell_pitch(row):
	"""一行像素里块边界的间距（众数）：取 RGB 均值，找 |diff| 的峰。"""
	p = row.mean(axis=1)
	d = np.abs(np.diff(p))
	# 阈值取固定值：相邻块的差是随机的（均值 ~0.28），「均值 + 3σ」会被稀疏的尖峰
	# 抬到 0.2 以上，反而一条边界都挑不出来。
	thr = 0.07
	cols = [i for i in range(len(d)) if d[i] > thr]
	merged = []
	for c in cols:
		if merged and c - merged[-1] <= 4:
			continue
		merged.append(c)
	sp = np.diff(merged)
	sp = sp[(sp > 4) & (sp < 200)]
	if len(sp) == 0:
		return None, 0
	vals, cnts = np.unique(sp, return_counts=True)
	return int(vals[cnts.argmax()]), len(sp)

def band_centroid(prof, around, half=70):
	"""横带的行重心（重心是模糊不变量，糊了也不动）+ 峰值强度。

	强度是必须的：底衬里要是**根本没画**这条带，光看重心会拿噪声的重心凑出一个
	「差不多对」的行号（假通过）。"""
	lo, hi = max(around - half, 0), min(around + half, len(prof))
	sub = prof[lo:hi]
	base = np.median(sub)
	w = np.clip(sub - base, 0, None)
	if w.sum() <= 0:
		return None, 0.0
	c = float((np.arange(lo, hi) * w).sum() / w.sum())
	return c, float(w.max())

for tag in ('s1', 's2'):
	scale, mode = open(f"{rundir}/scale-{tag}.txt").read().split()
	scale = float(scale)
	# mode 就是**设备**像素（wlroots 的 headless 输出按设备像素给模式）
	dev_w, dev_h = (int(v) for v in mode.split('x'))
	bd = load_raw(f"{rundir}/bd-{tag}.raw")
	shot = np.asarray(Image.open(f"{rundir}/shot-{tag}.png").convert('RGB')
		).astype(np.float32)
	gx, gy, gw, gh = (int(v) for v in open(f"{rundir}/geom-{tag}.txt").read().split())
	WX, WY = int(round(gx * scale)), int(round(gy * scale))
	WW, WH = int(round(gw * scale)), int(round(gh * scale))
	print(f"\n  scale={scale}  输出设备 {dev_w}x{dev_h}  窗口设备 "
		f"{WX},{WY} {WW}x{WH}  底衬缓冲 {bd.shape[1]}x{bd.shape[0]}")

	# 5) 底衬缓冲尺寸 == 输出的设备尺寸
	if bd.shape[1] == dev_w and bd.shape[0] == dev_h:
		print(f"  PASS  scale={scale}：底衬缓冲 = 输出设备尺寸 {dev_w}x{dev_h}")
	else:
		print(f"  FAIL  scale={scale}：底衬缓冲 {bd.shape[1]}x{bd.shape[0]} "
			f"!= 输出设备尺寸 {dev_w}x{dev_h}")
		ok = False

	# 1) 底衬 == 壁纸纹理 × scale（同坐标逐像素）
	ref = upscale_nearest(tex, int(scale))
	n = min(ref.shape[0], bd.shape[0]), min(ref.shape[1], bd.shape[1])
	diff = np.abs(ref[:n[0], :n[1]] - bd[:n[0], :n[1], :3]).mean()
	if diff < 0.006:
		print(f"  PASS  scale={scale}：底衬内容 == 壁纸 ×{int(scale)}"
			f"（同坐标平均差 {diff * 255:.2f}/255）")
	else:
		print(f"  FAIL  scale={scale}：底衬内容 != 壁纸 ×{int(scale)}"
			f"（同坐标平均差 {diff * 255:.2f}/255，> 1.5/255）")
		ok = False

	# 2) 底衬里块边界间距 == scale × 16
	# 取一条**不在横带上**的行（横带是平的，量不出块宽）：窗口顶边往下 100 设备像素
	row = bd[(WY + 100), WX:WX + WW, :3]
	pitch, cnt = cell_pitch(row)
	want = int(16 * scale)
	if pitch == want and cnt >= 3:
		print(f"  PASS  scale={scale}：底衬里块宽 = {pitch} 设备像素"
			f"（= {int(scale)} × 16 逻辑像素，{cnt} 条边界）")
	else:
		print(f"  FAIL  scale={scale}：底衬里块宽 = {pitch}（期望 {want} 设备像素，"
			f"{cnt} 条边界）")
		ok = False

	# 3) 窗口那块底衬里的横带行 == 屏上原样的横带行（同一张图、同一坐标）
	sharp = shot[:, WX + WW + 40:, :]          # 窗口右边露出来的壁纸（原样）
	blur = shot[:, WX + 60:WX + WW - 60, :]    # 窗口里面（底衬，糊过）
	def prof(img, ch):
		other = img.mean(axis=2)
		return (img[:, :, ch] - other).mean(axis=1)
	bands = ((80, 0, '红'), (360, 1, '绿'), (640, 2, '蓝'))
	rows_s, rows_b = [], []
	for ly, ch, name in bands:
		around = int(ly * scale)
		cs, ps = band_centroid(prof(sharp, ch), around)
		cb, pb = band_centroid(prof(blur, ch), around)
		if cs is None or cb is None:
			print(f"  FAIL  scale={scale}：{name}带找不到（屏上 {cs} / 底衬 {cb}）")
			ok = False
			continue
		rows_s.append(cs); rows_b.append(cb)
		if ps < 0.30 or pb < 0.10:
			print(f"  FAIL  scale={scale}：{name}带在底衬里没画出来"
				f"（强度 屏上 {ps:.2f} / 底衬 {pb:.2f}，阈值 0.30 / 0.10）")
			ok = False
		elif abs(cb - cs) <= 4.0:
			print(f"  PASS  scale={scale}：{name}带同坐标（屏上 {cs:.1f} vs "
				f"底衬 {cb:.1f}，差 {cb - cs:+.1f} 设备像素，"
				f"强度 {ps:.2f}/{pb:.2f}）")
		else:
			print(f"  FAIL  scale={scale}：{name}带错位（屏上 {cs:.1f} vs "
				f"底衬 {cb:.1f}，差 {cb - cs:+.1f} 设备像素）")
			ok = False
	# 4) 底衬里三条横带的间距 == scale × 280
	if len(rows_b) == 3:
		sp = np.diff(rows_b)
		want = 280 * scale
		if all(abs(s - want) <= 6 for s in sp):
			print(f"  PASS  scale={scale}：底衬里横带间距 = {sp[0]:.1f} / {sp[1]:.1f}"
				f" 设备像素（= {int(scale)} × 280 逻辑像素）")
		else:
			print(f"  FAIL  scale={scale}：底衬里横带间距 = {sp[0]:.1f} / {sp[1]:.1f}"
				f"（期望 {want:.0f} 设备像素，放大一倍会是 {2 * want:.0f}）")
			ok = False

sys.exit(0 if ok else 1)
PY
[ $? = 0 ] || FAILED=1

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-blurtexture.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
