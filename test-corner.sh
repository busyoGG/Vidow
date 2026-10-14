#!/bin/bash
# 圆角的**形状**：超椭圆（squircle）不是圆弧 —— 全按像素量。
#
# 圆角由装饰着色器（src/fx.c 的 sd_round_box）一处画出来，内容遮罩 / 边框环 /
# 阴影都吃它；这个脚本量的就是它：`corner_n` = 2 是圆弧（原来那种），默认 4 是
# 超椭圆（和 Alt+Tab 的 MRU 卡片、portal 选源对话框同一种）。
#
# 判据（scale 1、corner_radius = 32、gaps = 0、窗口顶到屏幕左上角，只看它左上角
# 32x32；窗口是纯蓝画在纯黑底上，所以每个像素的覆盖率 = B/255）：
#   1) 角区面积（覆盖率之和）：圆弧 πr²/4 = 804px²；超椭圆 n=4
#      = r²·Γ(1+1/n)²/Γ(1+2/n) = 949px²（正方形上界 1024）。±3%
#   2) 对角线：从窗口角沿 45° 往里第一颗覆盖率 ≥ 0.5 的像素 —— 圆弧在
#      r(1-1/√2) = 0.293r = 9.4px（第 9 颗）、n=4 在 r(1-2^(-1/4)) = 0.159r
#      = 5.1px（第 5 颗）。两条判据互相印证：面积涨 18%，对角线要短 4 颗。
#   3) corner_n = 2.0 时两个数都回到圆弧那一组 —— 形状是唯一的变量。
#   4) 边框环：沿直边的粗细 = border_width；沿对角线的内边界落在**同一形状的
#      平行曲线**上（圆弧 15.0 / 超椭圆 10.8 个对角步，差 4 步）。后者顺带证明
#      梯度归一化生效：不归一化的话超椭圆沿对角线的环会粗一圈（内边界跑到 11.8）。
#   5) corner_n 的四个入口：config.lua 键 / VIDOW_CORNER_N 环境变量 / IPC
#      set-config / 窗口规则（vidow.rule{ corner_n = … }，压过全局）；越界夹到 2 ~ 8。
#   6) 直边的 AA 斜坡仍是 ~2px（部分覆盖的像素行 = 1 ~ 2）：换成超椭圆之后角上
#      不该多出硬边 / 锯齿。
#
# 背景：把圆弧换成超椭圆只改了「角」，直边和内侧逐像素没动 —— 这条判据（4 里
# 的直边粗细 + 6 的斜坡宽度）就是拿来盯这个的。
set -u
cd "$(dirname "$0")"

RUNDIR=${VIDOW_CORNER_RUNDIR:-/tmp/vidow-corner-test}
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PID=""
R=32          # corner_radius
BOX=32        # 只看角区 32x32（= r）
BW=8          # 边框环那一轮的 border_width

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

for c in grim python3; do
	command -v "$c" >/dev/null || { echo "test-corner.sh: 需要 $c"; exit 1; }
done
python3 -c "from PIL import Image" 2>/dev/null ||
	{ echo "test-corner.sh: 需要 python3-pil"; exit 1; }
[ -x ./build/vidow ] || { echo "test-corner.sh: 先 ninja -C build"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
export XDG_RUNTIME_DIR="$RUNDIR/run"

# ---------------------------------------------------------------- 量像素
# 一次量四样东西，打一行 KV（bash 那边好取值）：
#   AREA          角区覆盖率之和（= 形状在角区的面积）
#   DIAG          对角线第一颗覆盖率 ≥ 0.5 的像素下标（0 = 正好顶在角上）
#   RING_STRAIGHT 沿直边（窗口横向中点那一列）的边框环粗细（像素）
#   RING_INNER    沿对角线的环内边界下标
#   AA            直边（窗口横向中点那一列）上覆盖率 0.9 → 0.1 跨几个像素
cat > "$RUNDIR/measure.py" <<'PY'
import sys, math
from PIL import Image

png, bx, by, box = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
mid = int(sys.argv[5])          # 直边取样那一列（窗口横向中点）
im = Image.open(png).convert('RGB')
px = im.load()
# 覆盖率：窗口是纯蓝（0,0,255）画在纯黑底（background_color = #000000）上 ——
# 预乘合成，外面的 AA 像素就是 蓝 × 覆盖率，所以 α = B/255。边框那一轮是红的，
# 覆盖率换算成「红」：α_border = (R - B)/255（蓝底 → 红环，B 掉了 R 上来了）。
cov = lambda i, j: px[bx + i, by + j][2] / 255.0
border = lambda i, j: (px[bx + i, by + j][0] - px[bx + i, by + j][2]) / 255.0

area = sum(cov(i, j) for i in range(box) for j in range(box))
diag = [cov(i, i) for i in range(box)]
first = next((i for i, a in enumerate(diag) if a >= 0.5), -1)
# 对角线上的 AA：部分覆盖（0.05 ~ 0.95）跨了几个对角步
d05 = next((i for i, a in enumerate(diag) if a >= 0.05), -1)
d95 = next((i for i, a in enumerate(diag) if a >= 0.95 and i >= d05), -1)
aa_diag = (d95 - d05) if d05 >= 0 and d95 >= 0 else -1

col = [cov(mid, j) for j in range(0, 64)]
# AA 斜坡 = 上边缘那一段里「部分覆盖（0.02 ~ 0.98）」的行数。2px 的
# smoothstep(-1, 1, d) 斜坡 → 2（边界正好落在两行之间时 1，落在行中心时 2；
# 所以判据给 ±1）。窗口的边界可能在 geom 之上偏半个像素，扫描从 -6 行起。
col_top = [cov(mid, j) for j in range(-6, 12)]
aa = sum(1 for a in col_top if 0.02 < a < 0.98)
# 环：从外边界往里数「红 ≥ 0.5」的像素。外边界得用「有没有墨」判（红环的蓝
# 分量是 0，拿蓝找会直接跳到环里面去）—— 背景是纯黑，R + B > 20 就算有墨。
ink = lambda j: px[bx + mid, by + j][0] + px[bx + mid, by + j][2]
outer = next((j for j in range(64) if ink(j) > 20), -1)
ring_straight = 0
if outer >= 0:
    for j in range(outer, 64):
        if border(mid, j) >= 0.5:
            ring_straight += 1
        elif ring_straight > 0:
            break
# 对角线上的环内边界：第一颗「红 < 0.5 而且蓝 ≥ 0.5」（回到内容）的像素
ring_inner = -1
seen_ring = False
for i in range(box + 16):
    if border(i, i) >= 0.5:
        seen_ring = True
    elif seen_ring and cov(i, i) >= 0.5:
        ring_inner = i
        break
print(f"AREA={area:.1f} DIAG={first} RING_STRAIGHT={ring_straight} "
      f"RING_INNER={ring_inner} AA={aa} AA_DIAG={aa_diag}")
PY

# ---------------------------------------------------------------- harness
start_vidow() { # start_vidow <config.lua 额外行…>
	rm -f "$LOG"
	cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
-- 留一圈 gap：窗口别贴着屏幕边，形状的外边缘才有左右背景，AA 斜坡量得到
gaps = 40
-- 客户端别画自己的 CSD：不然 GTK 会在左上角画它自己的标题栏和圆角，
-- 这块像素就不是「vidow 的圆角」了。
prefer_no_csd = true
corner_radius = 32
corner_n = 4
border_width = 2
border_color = "0000ffff"
border_color_focus = "0000ffff"
-- 阴影关掉：这块角区要只有「形状 + 底」，掺进阴影就量不准面积
shadow_radius = 0
blur_enabled = false
background_color = "#000000ff"
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
EOF
	printf '%s\n' "$@" >> "$CFGDIR/.config/vidow/config.lua"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	for _ in $(seq 1 40); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
}

stop_vidow() {
	[ -n "$CLIENT_PID" ] && { kill "$CLIENT_PID" 2>/dev/null; CLIENT_PID=""; }
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.5
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
		VIDOW_PGID=""
	fi
}

# 纯蓝窗口：一整个窗口都是同一个颜色，覆盖率 = B/255（见 measure.py）
open_client() {
	python3 tools/bluewin.py >/dev/null 2>&1 &
	CLIENT_PID=$!
	for _ in $(seq 1 40); do
		grep -aq "place bluewin" "$LOG" && break
		sleep 0.25
	done
	sleep 0.6
}

# 窗口几何（逻辑像素）→ 全局四个变量
geom() {
	./build/vidowmsg windows > "$RUNDIR/windows.json" 2>/dev/null
	read -r GX GY GW GH <<<"$(python3 - "$RUNDIR/windows.json" <<'PY'
import json, sys
ws = json.load(open(sys.argv[1]))
ws = ws if isinstance(ws, list) else ws.get("windows", [])
w = next((w for w in ws if w.get("app_id", "").startswith("bluewin")), ws[0])
g = w["geometry"]
print(g["x"], g["y"], g["width"], g["height"])
PY
)"
}

# 窗口「现在长什么样」里的 corner_n（vidowmsg windows 的 look 那一块）
look_n() {
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
ws = json.load(sys.stdin)
ws = ws if isinstance(ws, list) else ws.get("windows", [])
print(ws[0]["look"]["corner_n"])'
}

shot() { # shot <名字> [--ring]
	[ "${2:-}" = "--ring" ] && {
		./build/vidowmsg set-config border_width $BW >/dev/null
		./build/vidowmsg set-config border_color ff0000 >/dev/null
		./build/vidowmsg set-config border_color_focus ff0000 >/dev/null
		sleep 0.4
	}
	grim "$RUNDIR/$1.png"
	geom
	MEASURE[$1]=$(python3 "$RUNDIR/measure.py" "$RUNDIR/$1.png" \
		"$GX" "$GY" "$BOX" "$((GX + GW / 2))")
	echo "    $1: ${MEASURE[$1]}（窗口 $GX,$GY ${GW}x${GH}）"
}
declare -A MEASURE
kv() { echo " ${MEASURE[$1]}" | sed -n "s/.*[ ]$2=\([^ ]*\).*/\1/p"; }

check() { # check <描述> <实际> <期望> <容差>
	local d=$1 a=$2 e=$3 t=$4
	if python3 -c "import sys; sys.exit(0 if abs($a - $e) <= $t else 1)"; then
		pass "$d（$a，期望 $e ±$t）"
	else
		fail "$d（实际 $a，期望 $e ±$t）"
	fi
}

# 面积 / 对角线两个期望值都从公式来，不在脚本里写死：
#   面积 = r²·Γ(1+1/n)²/Γ(1+2/n)（n=2 → πr²/4；n→∞ → r²）
#   对角线 = r·(1 - 2^(-1/n))（即 c 的两分量相等那条线上 (|x|^n+|y|^n)^(1/n) = r）
exp_area() { python3 -c "
import math
n = $1; r = $R
print('%.1f' % (r * r * math.gamma(1 + 1 / n) ** 2 / math.gamma(1 + 2 / n)))"; }
# 环的内边界（对角步）：形状边界 + border_width 的欧氏厚度换算成对角步
# （一个对角步 = √2 像素），再取「第一颗中心过了它的像素」的下标
exp_ring_inner() { python3 -c "
import math
n = $1; r = $R; bw = $BW
print('%d' % math.ceil(r * (1 - 2 ** (-1.0 / n)) + bw / math.sqrt(2) - 0.5))"; }
exp_diag() { python3 -c "
import math
n = $1; r = $R
print('%d' % int(r * (1 - 2 ** (-1.0 / n))))"; }

export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

# ------------------------------------------------------------------ 1) n=4
mark "1) 默认（corner_n = 4）：角区面积 ≈ 949、对角线第 5 颗"
start_vidow
open_client
shot n4
A4=$(kv n4 AREA); D4=$(kv n4 DIAG)
check "corner_n = 4 的角区面积 = r²·Γ(1+1/4)²/Γ(1+1/2)" "$A4" "$(exp_area 4)" 28
check "corner_n = 4 的对角线第一颗像素下标" "$D4" "$(exp_diag 4)" 1
check "corner_n = 4 的角区面积确实比圆弧那一档大（+17%）" "$(python3 -c "print('%.1f' % ($A4 - $(exp_area 2)))")" \
	"$(python3 -c "print('%.1f' % ($(exp_area 4) - $(exp_area 2)))")" 28

# ------------------------------------------------------------------ 2) n=2
mark "2) IPC 改 corner_n = 2：回到圆弧那一组数（面积 804、对角线第 9 颗）"
./build/vidowmsg set-config corner_n 2 >/dev/null
sleep 0.3
shot n2
A2=$(kv n2 AREA); D2=$(kv n2 DIAG)
check "corner_n = 2 的角区面积 = πr²/4" "$A2" "$(exp_area 2)" 24
check "corner_n = 2 的对角线第一颗像素下标" "$D2" "$(exp_diag 2)" 1
check "corner_n = 2 时的窗口 look 也报 2" "$(look_n)" 2 0

# ------------------------------------------------------------------ 3) 边框 / 阴影跟形状
mark "3) 边框环跟着同一个形状（沿对角线落在同一形状的平行曲线上）"
shot ring-n2 --ring
./build/vidowmsg set-config corner_n 4 >/dev/null
sleep 0.3
shot ring-n4
# 沿对角线的环内边界（下标）：形状边界 + border_width（像素）→ 换算成对角步
#   n=4：边界 5.09 + 8/√2 = 10.75；n=2：9.37 + 8/√2 = 15.03
for n in 2 4; do
	RS=$(kv ring-n$n RING_STRAIGHT); RI=$(kv ring-n$n RING_INNER)
	check "corner_n = $n 沿直边的环粗细" "$RS" "$BW" 1
	check "corner_n = $n 沿对角线环的内边界（同一形状的平行曲线）" "$RI" \
		"$(exp_ring_inner $n)" 1
done

# ------------------------------------------------------------------ 4) AA
mark "4) 边缘过渡还是 ~2px（超椭圆不该在边上多出硬边 / 锯齿）"
for n in 2 4; do
	check "corner_n = $n 直边的 AA 斜坡（部分覆盖的像素行数）" "$(kv n$n AA)" 2 1
	check "corner_n = $n 对角线的 AA 斜坡（归一化后不该比直边宽）" "$(kv n$n AA_DIAG)" 1 1
done

# ------------------------------------------------------------------ 5) 四个入口 + 夹住
mark "5) corner_n 的四个入口（config.lua / 环境变量 / IPC / 窗口规则）与越界夹住"
./build/vidowmsg set-config corner_n 99 >/dev/null
V=$(./build/vidowmsg config corner_n | python3 -c 'import json,sys;print(json.load(sys.stdin)["value"])')
check "IPC set-config 99 → 夹到上界" "$V" 8 0
./build/vidowmsg set-config corner_n 1 >/dev/null
V=$(./build/vidowmsg config corner_n | python3 -c 'import json,sys;print(json.load(sys.stdin)["value"])')
check "IPC set-config 1 → 夹到下界" "$V" 2 0
check "config.lua 里写的 corner_n = 4 一开始就生效（本轮起手那一份）" "$(kv n4 AREA)" \
	"$(exp_area 4)" 28
stop_vidow

# 环境变量 > 配置文件；窗口规则 > 全局（和 corner_radius 一个优先级）
export VIDOW_CORNER_N=3
start_vidow 'vidow.rule{ app_id = "bluewin", corner_n = 8 }'
open_client
V=$(./build/vidowmsg config corner_n | python3 -c 'import json,sys;print(json.load(sys.stdin)["value"])')
check "VIDOW_CORNER_N 环境变量压过 config.lua（这一轮 env = 3）" "$V" 3 0
check "窗口规则 corner_n = 8 压过全局（窗口 look）" "$(look_n)" 8 0
shot rule8
A8=$(kv rule8 AREA)
check "规则给的 n = 8 在像素上生效（面积比 n=4 更接近正方形）" \
	"$(python3 -c "print('%.1f' % ($A8 - $A4))")" \
	"$(python3 -c "print('%.1f' % ($(exp_area 8) - $(exp_area 4)))")" 40
stop_vidow

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-corner.sh: OK"
else
	echo "有失败 / test-corner.sh: FAIL"
fi
exit "$FAILED"
