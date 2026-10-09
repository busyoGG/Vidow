#!/bin/bash
# 全屏的窗口在**混合 DPI 两块屏**上：占自己那块**整块屏**（逻辑整屏、2 倍屏上就是
# 整个 4K），滚出去那半块不许画到隔壁屏上，滚回来又严丝合缝。
#
# 这是用户那台机器的几何（一块 1 倍屏在 0,0，一块 2 倍屏摆在 x=1280）—— 单屏 headless
# 测不出「占整个屏」在缩放屏上的换算，也测不出越界那半块落在隔壁屏范围里会不会画过去。
#
# 判据：
#   1) 聚焦 HEADLESS-2 上的第一条列、Mod+F → 盒子 1280,0 1280x720（**整块逻辑屏**，
#      不是工作区 1290,10 1260x700）—— 2 倍屏的物理尺寸 2560x1440 就是这个盒子的两倍
#   2) 2 倍屏的截图（grim -o HEADLESS-2，物理 2560x1440）里，贴着屏幕边那几颗像素是
#      全屏窗口自己的内容 —— 没有 gap 那圈缝（逻辑 6px 的 gap 在 2 倍屏上就是 12px）
#   3) 焦点右移两列 → 全屏那块滑到 252（左边缘落到 HEADLESS-1 的范围里）：
#      它自己的盒子还是 1280x720，日志里 `clip: … 裁到自己那块屏里（屏框 1280,0 1280x720）`
#   4) **HEADLESS-1 的截图**里，越界那块该在的地方（1260,200）仍然是它自己的窗口内容
#      —— 全屏窗口那半块没画到隔壁屏上
#   5) 焦点移回第一条列 → 又严丝合缝盖住整屏（1280,0 1280x720）
#
# 提一句「贴边那两颗像素」为什么不直接判颜色：窗口自己那圈边框 / 焦点环 / 阴影也在屏幕
# 边上（2 倍屏上逻辑几 px 就是物理十几 px，边上那几颗是混合出来的），所以判据是「那儿
# **不是背景黑**」—— 有 gap 的时候那儿就是纯背景。内容本身另外在逻辑 20,200 那点判。
#
# 用法：./test-fullscreen-2out.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py / grim。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-fullscreen-2out-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*grim*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

cleanup() {
	stop_clients
	for pid in $CLIENT_PIDS; do
		[ -n "$pid" ] || continue
		kill "$pid" 2>/dev/null
	done
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi" 2>/dev/null || { echo "test-fullscreen-2out.sh: 需要 python3-gobject"; exit 1; }
python3 -c "from PIL import Image" 2>/dev/null || { echo "test-fullscreen-2out.sh: 需要 python3-pil"; exit 1; }
command -v grim >/dev/null || { echo "test-fullscreen-2out.sh: 需要 grim"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 两块屏位置 / 缩放钉死：这就是用户那台机器的几何（4K 缩放 2 摆在 x=1280，1 倍屏在 0,0）。
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", x = 0, y = 0, scale = 1, mode = "1280x720" }
vidow.output{ name = "HEADLESS-2", x = 1280, y = 0, scale = 2, mode = "2560x1440" }
gaps = 10
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
sleep 1

msg() { ./build/vidowmsg "$@" >/dev/null 2>&1; }

geo() { # <标题> -> "x y w h"
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
ws = json.load(sys.stdin)
if isinstance(ws, dict):
    ws = ws.get("windows", [])
for w in ws:
    if (w.get("title") or "") == sys.argv[1]:
        g = w["float_geometry"] if w.get("floating") else w["geometry"]
        print(g["x"], g["y"], g["width"], g["height"])
        break
else:
    print("0 0 0 0")
' "$1"
}

visible() { # <标题> -> true/false
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
ws = json.load(sys.stdin)
if isinstance(ws, dict):
    ws = ws.get("windows", [])
for w in ws:
    if (w.get("title") or "") == sys.argv[1]:
        print("true" if w.get("visible") else "false")
        break
else:
    print("?")
' "$1"
}

expect_geo() { # <标题> <期望盒子> <说明>
	local got
	got=$(geo "$1")
	if [ "$got" = "$2" ]; then
		pass "$3（$1 = $got）"
	else
		fail "$3（$1 = $got，期望 $2）"
	fi
}

# 这一小块里最多的那个颜色叫什么（pattern.py 的四象限：左上红 / 右上绿 / 左下蓝 / 右下白）
dominant() { # <png> <x> <y> [边长]
	python3 - "$1" "$2" "$3" "${4:-20}" <<'PY'
import collections, sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y, n = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
c = collections.Counter()
for px in range(max(0, x - n // 2), min(im.width, x + n // 2)):
    for py in range(max(0, y - n // 2), min(im.height, y + n // 2)):
        c[im.getpixel((px, py))] += 1
r, g, b = c.most_common(1)[0][0]
if r < 80 and g < 80 and b > 180:
    print("蓝")
elif r > 180 and g < 80 and b < 80:
    print("红")
elif g > 180 and r < 80 and b < 80:
    print("绿")
else:
    print("其它(%d,%d,%d)" % (r, g, b))
PY
}

shot_out() { # <输出> <名字> <x> <y> [边长] -> 抓这块屏，回那一小块的主色
	grim -o "$1" "$RUNDIR/$2.png" >/dev/null 2>&1
	dominant "$RUNDIR/$2.png" "$3" "$4" "${5:-20}"
}

expect_out_color() { # <输出> <名字> <x> <y> <期望颜色> <说明> [边长]
	local got
	got=$(shot_out "$1" "$2" "$3" "$4" "${7:-20}")
	if [ "$got" = "$5" ]; then
		pass "$6（$3,$4 是$got）"
	else
		fail "$6（$3,$4 是$got，期望$5）"
	fi
}

expect_not_color() { # <输出> <名字> <x> <y> <不该是> <说明> [边长]
	local got
	got=$(shot_out "$1" "$2" "$3" "$4" "${7:-20}")
	if [ "$got" != "$5" ]; then
		pass "$6（$3,$4 是$got）"
	else
		fail "$6（$3,$4 是$got，不该是$5）"
	fi
}

start_client() { # <窗口标题> <挪到哪块屏>
	python3 ./tools/pattern.py "$1" >/dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do
		[ "$(geo "$1")" != "0 0 0 0" ] && break
		sleep 0.25
	done
	msg move-window-to-output "$2" --title "$1"
	sleep 0.6
}

# --- 1) A 在 1 倍屏上，B/C/D 在 2 倍屏上（三条 0.4 的列，装不下要滚动）-------
mark "1) A 在 HEADLESS-1，B/C/D 三条 0.4 的列在 HEADLESS-2（条带装不下）"
start_client A HEADLESS-1
start_client B HEADLESS-2
start_client C HEADLESS-2
start_client D HEADLESS-2
for t in B C D; do msg set-column-width 0.4 --title "$t"; done
msg set-column-width 1 --title A   # A 拉满：它右边缘那块要用来判「越界那半块画过来没」
sleep 0.8
msg focus-window --title B
sleep 0.8
expect_geo B "1290 10 504 700" "B 在 2 倍屏的可用区左上角、0.4 宽的列里"
expect_geo A "10 10 1260 700" "A 在 1 倍屏上占着工作区（10..1270）"

# --- 2) B 全屏：占**整块逻辑屏**（2 倍屏的物理 2560x1440 就是它的两倍）------
mark "2) B 全屏：占整块逻辑屏 1280x720（不是工作区 1260x700）"
msg toggle-fullscreen --title B
sleep 0.8
expect_geo B "1280 0 1280 720" "全屏的 B 占满 HEADLESS-2 那块整屏"
expect_out_color HEADLESS-2 content 40 400 红 "2 倍屏上是 B 的内容（逻辑 20,200）"
expect_not_color HEADLESS-2 flush-left 4 400 "其它(0,0,0)" "2 倍屏上贴着屏幕左边那颗不是背景（逻辑 2px 处没有 gap 的缝）" 4
expect_not_color HEADLESS-2 flush-bottom 2000 1436 "其它(0,0,0)" "2 倍屏上贴着屏幕下边缘那颗不是背景（逻辑 1000,718）" 4
[ "$(visible A)" = "true" ] && pass "隔壁那块屏上的 A 照旧" ||
	fail "A 不见了（visible=$(visible A)）"

# --- 3) 焦点右移一列：全屏那块滑进 1 倍屏的范围里 ---------------------------
mark "3) 焦点右移一列：B 滑到 1008（左边缘落进 HEADLESS-1 的范围），D 并排"
msg focus-column 1
sleep 0.4
msg focus-column 1
sleep 0.8
expect_geo B "252 0 1280 720" "B 滑到 252（还是 1280x720）—— 已经在 1 倍屏的范围里了"
expect_geo D "2046 10 504 700" "D 在 2 倍屏的右半边（2046）"
[ "$(visible B)" = "true" ] && pass "滑出去半块的 B 还画着" ||
	fail "B 被藏了（visible=$(visible B)）"
grep -aq "clip: B 裁到自己那块屏里（屏框 1280,0 1280x720" "$LOG" &&
	pass "日志里写着 B 裁到自己那块屏里（屏框 1280,0 1280x720）" ||
	fail "日志里没有那条裁剪：$(grep -a 'clip: B' "$LOG" | tail -2 | tr '\n' '|')"

# --- 4) 越界那半块没画到隔壁屏上 -------------------------------------------
mark "4) 越界那半块没画到 HEADLESS-1 上"
expect_out_color HEADLESS-1 spill 1260 200 绿 "1 倍屏右边缘那点还是 A 自己的内容（不是 B 的）"

# --- 5) 滚回来又严丝合缝 ----------------------------------------------------
mark "5) 焦点移回第一条列：B 又严丝合缝盖住整屏"
msg focus-column -1
sleep 0.4
msg focus-column -1
sleep 0.8
expect_geo B "1280 0 1280 720" "回到 B 那条列，它又占满整块屏"
expect_out_color HEADLESS-2 back-left 4 400 红 "2 倍屏左边还是 B 的内容"

# --- 6) 收尾 -----------------------------------------------------------------
mark "6) 收尾"
grep -aq "assert\|abort" "$LOG" && fail "日志里有 assert / abort" || pass "日志里没有 assert / abort"
[ -n "$VIDOW_PGID" ] && kill -0 "$VIDOW_PGID" 2>/dev/null && pass "合成器还活着" ||
	fail "合成器没了"

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
