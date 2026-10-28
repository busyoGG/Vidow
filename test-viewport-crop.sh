#!/bin/bash
# 客户端只声明了源缓冲的一部分时（wp_viewport 的 src 框），装饰必须按那一块采。
#
# 用户报的：「chromium 系的应用」窗口框是到位了，里面的画面只占左上角一块，
# 右边/下边一条黑边（截图 /tmp/chromium.png）。根因在装饰管线换算「源缓冲里哪一块
# 是窗口」那一步：它拿**逻辑尺寸**的比例去采**客户端缓冲像素**，只有在「缓冲 =
# 逻辑尺寸 × 整数倍、而且整块都有效」时才等价。chromium 在分数缩放下会把缓冲开得
# 比窗口大一圈（headless 实测：窗口 954x1068、缓冲 2304x2560、viewport src 0,0
# 1908x2136 —— 它先按更大的尺寸把缓冲开好，窗口改小之后接着用），有效区只占缓冲
# 宽度的 82.8%：按整块缓冲采，那圈没画过的像素（黑/透明）就一起铺进窗口，屏幕上
# 正是那 17% 的黑边，而且一直那样（不是过渡态）。wlroots 自己的场景节点手里就有
# 正确的源区域（wlr_scene_buffer.src_box，缓冲像素），装饰按它换算（见 view.c 的
# fx_geo_tex_map）。
#
# 判据（headless 一块 scale=2 的屏 + chromium，没装 chromium 就跳过）：
#   1) 真的出现了「缓冲比窗口大一圈」的客户端（vidowmsg windows 的 surface
#      buffer_width/buffer_height > 逻辑尺寸 × 2）—— 没出现就 SKIP，不当通过
#   2) 这时候装饰采的区域必须是缓冲的一部分（surface.tex_du/tex_dv 落在 0..1 内）
#   3) 屏幕像素：窗口右边缘、下边缘那两条（内容区最后 5%）的亮度要跟窗口中心一个
#      量级 —— 采错区域时那里是纯黑的边（实测均值 0），修好后是页面自己的底色
#   4) vidow 全程活着
#
# 用法：./test-viewport-crop.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-viewport-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CHROME_PGID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
skip() { echo "  SKIP  $1"; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	if [ -n "$CHROME_PGID" ]; then
		kill -TERM -- "-$CHROME_PGID" 2>/dev/null
	fi
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	if [ -n "$CHROME_PGID" ]; then
		sleep 0.3
		kill -KILL -- "-$CHROME_PGID" 2>/dev/null
	fi
	# 别按进程名全杀 vidow：用户桌面会话也叫这个名字（2026-10-28 实测
	# 在 test-screencast.sh 上把整台桌面端掉过）。上面按 VIDOW_PGID 杀自己那个。
	sleep 0.3
	# 别按进程名全杀 vidow：用户桌面会话也叫这个名字（2026-10-28 实测
	# 在 test-screencast.sh 上把整台桌面端掉过）。上面按 VIDOW_PGID 杀自己那个。
	if [ -z "${VIDOW_TEST_KEEP:-}" ]; then
		rm -rf "$RUNDIR"
	fi
}
trap cleanup EXIT

command -v chromium >/dev/null || { echo "test-viewport-crop.sh: 没装 chromium，跳过"; exit 0; }
command -v grim >/dev/null || { echo "test-viewport-crop.sh: 需要 grim"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "test-viewport-crop.sh: 需要 python3-PIL"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

# scale=2：chromium 才会走分数缩放那条路（把缓冲开大 + viewport src 框）
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 2, mode = "1920x1080" }
blur_enabled = true
blur_levels = 4
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

mark "客户端的 wp_viewport 有效区域：装饰按它采，窗口里不能有黑边"

export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
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

# chromium：新鲜的 profile（会先按自己的默认尺寸开缓冲，再被 vidow 收成贴图大小）
setsid nohup chromium --ozone-platform=wayland --no-sandbox \
	--user-data-dir="$RUNDIR/chromium" --window-size=700,700 about:blank \
	> "$RUNDIR/chromium.log" 2>&1 </dev/null &
CHROME_PGID=$!

OK=""
for _ in $(seq 1 60); do
	if ./build/vidowmsg windows 2>/dev/null | grep -q '"width"'; then
		OK=1
		break
	fi
	sleep 0.5
done
if [ -z "$OK" ]; then
	fail "chromium 的窗口没映射上来（$(tail -2 "$RUNDIR/chromium.log" | tr '\n' '|')）"
	echo; echo "结果：$([ "$FAILED" = 0 ] && echo 全过 || echo 有失败)"; exit 1
fi
sleep 3
./build/vidowmsg windows > "$RUNDIR/win.json" 2>&1

# 挑一个「缓冲比窗口大一圈」的窗口 —— 这就是复现条件
# 屏的逻辑尺寸（上面的 mode 1920x1080 / scale 2）—— 只挑**整块都在屏上**的窗口，
# 不然像素判据量到的是窗口外面的东西，等于没量
SCREEN_W=960
SCREEN_H=540
read -r WID WX WY WW WH FW FH < <(python3 - "$RUNDIR/win.json" "$SCREEN_W" "$SCREEN_H" <<'PY'
import json, sys
try:
    ws = json.load(open(sys.argv[1]))
except Exception:
    ws = []
SW, SH = int(sys.argv[2]), int(sys.argv[3])
best = None
for w in ws:
    s = w.get("surface") or {}
    g = w.get("geometry") or {}
    if not s.get("mapped") or not g.get("width"):
        continue
    if s.get("width", 0) <= 0 or s.get("height", 0) <= 0:
        continue
    big = (s.get("buffer_width", 0) > s["width"] * 2 * 1.02 or
           s.get("buffer_height", 0) > s["height"] * 2 * 1.02)
    # 判据量的是「装饰画出来的那块框」（frame_*），不是布局槽位（geometry）：
    # 客户端不接受 configure 时框比槽位大
    fw = s.get("frame_width", 0) or g["width"]
    fh = s.get("frame_height", 0) or g["height"]
    inside = (g["x"] >= 0 and g["y"] >= 0 and
              g["x"] + fw <= SW and g["y"] + fh <= SH)
    if big and inside and (best is None or fw * fh > best[6] * best[7]):
        best = (w["id"], g["x"], g["y"], g["width"], g["height"], s, fw, fh)
if best is None:
    print("0 0 0 0 0 0 0")
else:
    print("%s %s %s %s %s %s %s" % (best[:5] + (best[6], best[7])))
    json.dump(best[5], open(sys.argv[1] + ".surf", "w"))
PY
)

if [ "$WID" = "0" ] || [ -z "$WID" ]; then
	skip "chromium 这次没把缓冲开得比窗口大、或者那个窗口没整块在屏上（没有复现条件，没测到东西）"
	echo; echo "结果：$([ "$FAILED" = 0 ] && echo 全过 || echo 有失败)"; exit 0
fi
	pass "复现条件在：窗口 $WID 框 ${FW}x${FH} @${WX},${WY}（$(cat "$RUNDIR/win.json.surf") ）"

TEX=$(python3 - "$RUNDIR/win.json.surf" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
print("%.4f %.4f %.4f %.4f" % (s.get("tex_u0", 0), s.get("tex_v0", 0),
                               s.get("tex_du", 0), s.get("tex_dv", 0)))
PY
)
DU=$(echo "$TEX" | cut -d' ' -f3)
DV=$(echo "$TEX" | cut -d' ' -f4)
if python3 -c "import sys; sys.exit(0 if 0 < $DU < 0.99 and 0 < $DV < 0.99 else 1)"; then
	pass "装饰按有效区域采（tex_du/tex_dv = $DU / $DV，不是整块缓冲）"
else
	fail "装饰采的还是整块缓冲（tex_du/tex_dv = $DU / $DV，应当是 0..1 之间）"
fi

grim -o HEADLESS-1 "$RUNDIR/shot.png" >/dev/null 2>&1
if [ ! -s "$RUNDIR/shot.png" ]; then
	fail "grim 没截到图"
else
	RES=$(python3 - "$RUNDIR/shot.png" "$WX" "$WY" "$FW" "$FH" <<'PY'
import sys, warnings
warnings.filterwarnings("ignore")
import numpy as np
from PIL import Image
shot, x, y, w, h = sys.argv[1], *(int(v) for v in sys.argv[2:6])
S = 2  # 输出 scale
im = np.asarray(Image.open(shot).convert("RGB")).astype(float)
lum = im.mean(axis=2)
# 逻辑 -> 设备像素，再往里收 8px（边框 / 圆角）
x0, y0 = x * S + 8, y * S + 8
x1, y1 = (x + w) * S - 8, (y + h) * S - 8
if x1 - x0 < 40 or y1 - y0 < 40:
    print("TOOSMALL 0 0")
    raise SystemExit
mid = lum[y0 + (y1 - y0) // 3:y0 + 2 * (y1 - y0) // 3,
          x0 + (x1 - x0) // 3:x0 + 2 * (x1 - x0) // 3].mean()
right = lum[y0 + (y1 - y0) // 3:y0 + 2 * (y1 - y0) // 3,
            x1 - (x1 - x0) // 20:x1].mean()
bottom = lum[y1 - (y1 - y0) // 20:y1,
             x0 + (x1 - x0) // 3:x0 + 2 * (x1 - x0) // 3].mean()
print("%.1f %.1f %.1f" % (mid, right, bottom))
PY
)
	set -- $RES
	if [ "$1" = "TOOSMALL" ]; then
		skip "窗口太小，像素判据量不了（${FW}x${FH}）"
	else
		MID=$1; RIGHT=$2; BOTTOM=$3
		# 窗口中心得有内容（别拿一块全黑的窗口当通过）
		if python3 -c "import sys; sys.exit(0 if $MID > 20 else 1)"; then
			pass "窗口中心有内容（亮度 $MID）"
		else
			fail "窗口中心是黑的（亮度 $MID）—— 窗口没画上东西"
		fi
		# 右边、下边那两条不能是黑边
		if python3 -c "import sys; sys.exit(0 if $RIGHT >= 0.5*$MID and $BOTTOM >= 0.5*$MID else 1)"; then
			pass "右边缘 / 下边缘不是黑边（$RIGHT / $BOTTOM，中心 $MID）"
		else
			fail "右边缘 / 下边缘一条黑边（$RIGHT / $BOTTOM，中心 $MID）—— 装饰采到缓冲外面去了"
		fi
	fi
fi

if pgrep -x vidow >/dev/null 2>&1; then
	pass "vidow 还活着"
else
	fail "vidow 死了（$(tail -3 "$LOG" | tr '\n' '|')）"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "结果：全过"
else
	echo "结果：有失败"
fi
exit "$FAILED"
