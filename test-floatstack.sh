#!/bin/bash
# 层级：**浮动窗口永远在平铺窗口之上**，浮窗之间靠**点击**换层级（点哪个哪个上来）。
#
# 为什么值得单开一个测试：层级以前是「谁最后被动过谁在上面」——
#   * 新窗口是 `wlr_scene_tree_create()` 建的 = 挂到场景树最后 = 永远在最上面，
#     所以新开一个平铺窗口会盖住已有的浮窗；
#   * 聚焦那边是无差别 `raise_to_top`，点一下（或者用键盘焦点切到）平铺窗口，
#     浮窗就被压下去了。
# 两条都不该发生，而屏幕上看「谁盖着谁」是最容易悄悄坏掉的东西（没人会注意）。
#
# 判据（headless 1280x720 一块屏 + pattern.py 平铺 + sheet.py 纯色浮窗）：
#   1) 基线：浮窗摆上去之前，取样点是**红**的（= 那儿真有一个平铺窗口在画）
#   2) 浮窗摆上去 → 取样点变**蓝**（浮窗压在平铺之上）
#   3) 用 IPC 把焦点给平铺窗口 → 还是**蓝**（焦点变化不许把浮窗压下去）
#   4) 鼠标点平铺窗口（没点到浮窗身上）→ 还是**蓝**
#   5) 平铺窗口进全屏 → 取样点不再是蓝（全屏压一切，浮窗也不行）；退出来 → 又变蓝
#   6) 新开一个平铺窗口（新建的场景节点）→ 还是**蓝**（新窗口不许盖住浮窗）
#   7) 再开一个纯色浮窗 B 压在 A 身上 → 取样点变 B 的颜色（**红**）
#   8) 点 A 露出来的那一块 → 变回**蓝**（浮窗之间靠点击换层级）
#   9) 点 B 露出来的那一块 → 又变**红**（两边都点得动，不是一次性的）
#
# 取样点 P 是浮窗 A 的中心（也是 A / B 的重叠处）：那里最上面的是谁，颜色就是谁的。
# 颜色按「一小块里最多的那个颜色」判（sheet.py 上有一层 1px 白网格，单点取样会踩线）。
#
# 用法：./test-floatstack.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py / sheet.py / grim。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-floatstack-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	stop_clients
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*sheet.py*|*grim*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

python3 -c "import gi" 2>/dev/null || { echo "test-floatstack.sh: 需要 python3-gobject"; exit 1; }
python3 -c "from PIL import Image" 2>/dev/null || { echo "test-floatstack.sh: 需要 python3-pil"; exit 1; }
command -v grim >/dev/null || { echo "test-floatstack.sh: 需要 grim"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
gaps = 10
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
sleep 0.5

msg() { ./build/vidowmsg "$@" >/dev/null 2>&1; }

# 窗口的盒子：浮动的用 float_geometry，平铺的用 geometry（和 IPC 报的一致）
box() { # <标题> -> "x y w h"
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["title"] == sys.argv[1]:
        g = w["float_geometry"] if w["floating"] else w["geometry"]
        print(g["x"], g["y"], g["width"], g["height"])
        break
else:
    print("0 0 0 0")
' "$1"
}

# 这一小块里最多的那个颜色叫什么（sheet.py 有 1px 白网格，单点会踩线）
dominant() { # <png> <x> <y> [边长]
	python3 - "$1" "$2" "$3" "${4:-40}" <<'PY'
import collections, sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y, n = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
c = collections.Counter()
for px in range(x - n // 2, x + n // 2):
    for py in range(y - n // 2, y + n // 2):
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

shot() { # <名字> -> 存成 $RUNDIR/<名字>.png，打印它在取样点上的颜色
	grim "$RUNDIR/$1.png" >/dev/null 2>&1
	dominant "$RUNDIR/$1.png" "$PX" "$PY"
}

expect_color() { # <名字> <期望颜色> <说明>
	local got
	got=$(shot "$1")
	if [ "$got" = "$2" ]; then
		pass "$3（取样点 $PX,$PY 是$got）"
	else
		fail "$3（取样点 $PX,$PY 是$got，期望$2）"
	fi
}

start_client() { # <python 文件> <参数…>
	python3 "./tools/$1" "${@:2}" >/dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 1.6
}

# ------------------------------------------------------------ 1) 平铺窗口
mark "基线：一个平铺窗口"
start_client pattern.py tile1
msg set-column-width 1 --title tile1
sleep 0.5
read -r TX TY TW TH <<< "$(box tile1)"
if [ "$TW" -lt 1000 ]; then
	fail "平铺窗口没铺满工作区（$TX,$TY ${TW}x${TH}）—— 后面的取样点会落在空处"
else
	pass "平铺窗口 tile1 占住工作区（$TX,$TY ${TW}x${TH}）"
fi

# 取样点：平铺窗口内容的左上四分之一处（pattern.py 那个象限是**纯红**）
PX=$(( TX + TW / 4 ))
PY=$(( TY + TH / 4 ))
expect_color base 红 "基线：取样点上是平铺窗口的纯红象限"

# ------------------------------------------------------------ 2) 浮窗压在平铺之上
mark "浮窗在平铺之上"
start_client sheet.py A 0 0 1 1
msg set-floating true --title A
msg set-window-position $(( PX - 200 )) $(( PY - 150 )) --title A
sleep 0.8
read -r AX AY AW AH <<< "$(box A)"
if [ "$AW" -lt 100 ] || [ "$AH" -lt 100 ]; then
	fail "浮窗 A 没有盒子（$AX,$AY ${AW}x${AH}）"
fi
expect_color float 蓝 "浮窗 A 压在平铺窗口之上"

# ------------------------------------------------------------ 3) 焦点 / 点击平铺窗口
mark "焦点与点击都不许把浮窗压下去"
msg focus-window --title tile1
sleep 0.5
expect_color focus-tile 蓝 "IPC 把焦点给平铺窗口之后，浮窗还在上面"

msg move-cursor 1000 650
sleep 0.2
msg pointer-button click left
sleep 0.6
expect_color click-tile 蓝 "鼠标点平铺窗口（没点到浮窗）之后，浮窗还在上面"

# ------------------------------------------------------------ 4) 全屏
mark "全屏压一切"
msg toggle-fullscreen --title tile1
sleep 0.8
C=$(shot fullscreen)
if [ "$C" = "蓝" ]; then
	fail "全屏的平铺窗口没压住浮窗（取样点还是蓝）"
else
	pass "全屏的平铺窗口压住了浮窗（取样点是$C）"
fi
msg toggle-fullscreen --title tile1
sleep 0.8
expect_color unfullscreen 蓝 "退出全屏之后浮窗又回到上面"

# ------------------------------------------------- 4b) 全屏的窗口按 Mod+V 变浮窗
mark "全屏的窗口 Mod+V 变浮窗：全屏先退掉（用户报「一直叠在最上面，不管平铺还是浮动」）"
# 用户报的现场：他那个窗口是全屏状态（层级 平铺 < 浮动 < 全屏，全屏永远压在最上面），
# Mod+V 只换了浮动标志、全屏标志还在 —— 于是「不管平铺还是浮动都压着别人」。
msg toggle-fullscreen --title tile1
sleep 0.8
C=$(shot fs-before-float)
if [ "$C" != "蓝" ]; then
	pass "全屏的平铺窗口又压住了浮窗（取样点是$C）"
else
	fail "全屏没压住浮窗（取样点是蓝）—— 这一节测不到东西"
fi
msg toggle-floating --title tile1
sleep 0.8
read -r FM FS <<< "$(./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["title"] == "tile1":
        print(w["float_mode"], "true" if w["fullscreen"] else "false")
        break
else:
    print("? ?")')"
if [ "$FM" = "fixed" ] || [ "$FM" = "follow" ]; then
	pass "Mod+V 之后 tile1 是浮窗了（float_mode=$FM）"
else
	fail "Mod+V 之后 tile1 还是 $FM"
fi
if [ "$FS" = "false" ]; then
	pass "全屏标志被清掉了（fullscreen=false）"
else
	fail "全屏标志还在（fullscreen=$FS）"
fi
if grep -aq "全屏: tile1 关（变浮窗）" "$LOG"; then
	pass "日志里写着「全屏: tile1 关（变浮窗）」"
else
	fail "日志里没有全屏那行：$(grep -a '全屏:' "$LOG" | tail -2 | tr '\n' ' ')"
fi
# 它现在是**浮窗**：缩小它，底下那个浮窗 A 就露出来了（全屏状态下缩不动、也永远压着）。
msg set-window-size 300 200 --title tile1
sleep 0.8
expect_color fs-after-float 蓝 "缩小它之后浮窗 A 露出来了（不再无条件压着别人）"
msg toggle-floating --title tile1
sleep 0.8

# ------------------------------------------------------------ 5) 新开的平铺窗口
mark "新开的平铺窗口也不许盖住浮窗"
start_client pattern.py tile2
sleep 0.8
expect_color new-tile 蓝 "新开的平铺窗口 tile2 没有盖住浮窗"

# ------------------------------------------------------------ 6) 浮窗之间靠点击换层级
mark "浮窗之间：点哪个哪个上来"
start_client sheet.py B 1 0 0 1
msg set-floating true --title B
msg set-window-position $(( PX - 80 )) $(( PY - 80 )) --title B
sleep 0.8
read -r BX BY BW BH <<< "$(box B)"
if [ "$BW" -lt 100 ]; then
	fail "浮窗 B 没有盒子（$BX,$BY ${BW}x${BH}）"
fi
expect_color float-b 红 "后开的浮窗 B 压在 A 上面"

# 点 A 露出来的那一块（在 A 里、不在 B 里）
CAX=$(( PX - 150 )); CAY=$(( PY + 100 ))
msg move-cursor "$CAX" "$CAY"
sleep 0.2
msg pointer-button click left
sleep 0.6
expect_color click-a 蓝 "点 A 露出来的那块（$CAX,$CAY）→ A 上来压住 B"

# 点 B 露出来的那一块（在 B 里、不在 A 里）
CBX=$(( PX + 250 )); CBY=$(( PY + 100 ))
msg move-cursor "$CBX" "$CBY"
sleep 0.2
msg pointer-button click left
sleep 0.6
expect_color click-b 红 "点 B 露出来的那块（$CBX,$CBY）→ B 又上来压住 A"

# 焦点还在平铺窗口上的时候点浮窗：焦点没变也必须抬（这就是「点了没反应」那个坑）
mark "焦点没变也要抬（浮窗已经拿着键盘时再点它）"
msg focus-window --title tile2
sleep 0.5
msg move-cursor "$CAX" "$CAY"
sleep 0.2
msg pointer-button click left
sleep 0.6
expect_color click-a-focused 蓝 "焦点在平铺窗口上时点 A → A 上来（焦点没变也抬）"

# ------------------------------------------------------------ 7) 收尾
mark "收尾"
if grep -aqE "assert|abort|SIGSEGV|段错误" "$LOG"; then
	fail "日志里有 assert / abort / 崩溃"
else
	pass "日志里没有 assert / abort / 崩溃"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "test-floatstack.sh: 全部通过"
else
	echo "test-floatstack.sh: 有失败项（运行目录 $RUNDIR，VIDOW_TEST_KEEP=1 可保留）"
fi
exit "$FAILED"
