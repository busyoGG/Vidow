#!/bin/bash
# 全屏的窗口**参与滚动**（用户报「全屏的窗口也要参与滚动。现在的全屏好像不是真全屏，
# 之前 obs 不小心全屏的时候看起来和普通窗口也差不多」）。
#
# niri 的两条规矩（官方文档 Fullscreen and Maximize）：
#   * "Fullscreen windows cover the entire screen" —— 全屏占的是**整块屏**，不吃 gap、
#     也不给状态栏留位置（vidow 自己的状态栏在更上面的那层 layer 树里，是另一件事）。
#     以前它摆的是**工作区**，单列布局下跟普通平铺窗口几乎一模一样 —— 这就是用户看到的
#     「不是真全屏、和普通窗口差不多」。
#   * "fullscreen and maximized windows remain a normal participant of the layout: you can
#     scroll left and right from them and see other windows" —— 它在条带上占**一整屏**宽，
#     往旁边滚一列它就整块滑出去、旁边的列并排露出来。以前它钉在屏幕上（不跟着条带走），
#     怎么滚都糊在整块屏上，看着像「滚动对它没反应」。
#
# 判据（headless 1280x720 一块屏，gaps 10 → 工作区 10,10 1260x700；4 条列各 0.4 = 504 宽）：
#   1) 聚焦 A、Mod+F → A 的盒子 0,0 1280x720（**整块屏**，不是工作区 10,10 1260x700）；
#      左下角那颗像素从壁纸色变成 A 的内容色 —— 贴到屏幕边上了，没有 gap 那圈缝
#   2) 焦点右移一列 → A 滑到 -514（还是 1280x720、还可见），B 并排出现在 766；
#      取样点左边是 A 的尾巴、右边是 B —— 两块同时在屏上，这才是「参与滚动」
#   3) 再右移一列 → A -1028（露出小半块）；再右移 → A -1542 整块滚出屏 → 藏起（visible=false）
#   4) 左移三次回到 A → 又严丝合缝盖住整屏（0,0 1280x720）—— 它还是条带里的普通一员
#   5) 退全屏 → 列宽回到 0.4（10,10 504x700），后面的列跟着合拢（B 524）
#   6) 滑出去那半块**裁在自己那块屏里**（日志 `clip: A 裁到自己那块屏里（屏框 0,0 1280x720，
#      窗口 -514,0 1280x720）`）—— 多屏时不许画到隔壁显示器上
#   7) 客户端**自己**要的全屏（`preinit --after-commit --fullscreen`，OBS 按 F11 那条路）：
#      同样盖住整块屏（它是个定尺寸客户端，也照样整屏 + 黑底衬）、同样跟着条带走
#
# 用法：./test-fullscreen-scroll.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py / grim。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-fullscreen-scroll-test
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
		*pattern.py*|*grim*|*build/preinit*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

python3 -c "import gi" 2>/dev/null || { echo "test-fullscreen-scroll.sh: 需要 python3-gobject"; exit 1; }
python3 -c "from PIL import Image" 2>/dev/null || { echo "test-fullscreen-scroll.sh: 需要 python3-pil"; exit 1; }
command -v grim >/dev/null || { echo "test-fullscreen-scroll.sh: 需要 grim"; exit 1; }

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
./build/vidowmsg output-mode HEADLESS-1 1280 720 60 >/dev/null 2>&1
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

visible() { # <标题> -> true/false
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["title"] == sys.argv[1]:
        print("true" if w.get("visible") else "false")
        break
else:
    print("?")
' "$1"
}

field() { # <标题> <字段>
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
for w in json.load(sys.stdin):
    if w["title"] == sys.argv[1]:
        print(w.get(sys.argv[2]))
        break
else:
    print("?")
' "$1" "$2"
}

expect_box() { # <标题> <期望盒子> <说明>
	local got
	got=$(box "$1")
	if [ "$got" = "$2" ]; then
		pass "$3（$1 = $got）"
	else
		fail "$3（$1 = $got，期望 $2）"
	fi
}

# 这一小块里最多的那个颜色叫什么（pattern.py 的四象限：左上红 / 右上绿 / 左下蓝）
dominant() { # <png> <x> <y> [边长]
	python3 - "$1" "$2" "$3" "${4:-20}" <<'PY'
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

shot() { # <名字> <x> <y> [边长] -> 存成 $RUNDIR/<名字>.png，打印那一小块的主色
	grim "$RUNDIR/$1.png" >/dev/null 2>&1
	dominant "$RUNDIR/$1.png" "$2" "$3" "${4:-20}"
}

expect_color() { # <名字> <x> <y> <期望颜色> <说明> [边长]
	local got
	got=$(shot "$1" "$2" "$3" "${6:-20}")
	if [ "$got" = "$4" ]; then
		pass "$5（$2,$3 是$got）"
	else
		fail "$5（$2,$3 是$got，期望$4）"
	fi
}

expect_not_color() { # <名字> <x> <y> <不该是> <说明> [边长]
	local got
	got=$(shot "$1" "$2" "$3" "${6:-20}")
	if [ "$got" != "$4" ]; then
		pass "$5（$2,$3 是$got）"
	else
		fail "$5（$2,$3 是$got，不该是$4）"
	fi
}

start_client() { # <窗口标题>
	python3 ./tools/pattern.py "$1" >/dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 1.6
}

# ------------------------------------------------------------ 0) 四条 0.4 宽的列
mark "四条列各 0.4 宽（工作区 1260，所以每条 504）"
for t in A B C D; do start_client "$t"; done
for t in A B C D; do msg set-column-width 0.4 --title "$t"; done
sleep 0.6

read -r AX AY AW AH <<< "$(box A)"
if [ "$AW" = 504 ] && [ "$AH" = 700 ] && [ "$AY" = 10 ]; then
	pass "列宽 504、工作区高 700（A = $AX,$AY ${AW}x${AH}）"
else
	fail "工作区不是预想的 1260x700 / 列宽 504（A = $AX,$AY ${AW}x${AH}）—— 后面的数字全要重算"
fi

# ------------------------------------------------------------ 1) 聚焦 A + 全屏：盖住整块屏
mark "聚焦 A 再全屏：占的是整块屏，不是工作区"
msg focus-window --title A
sleep 0.6
expect_box A "10 10 504 700" "聚焦 A：它在自己那条 504 宽的列里（左边就是那道 10px 的 gap）"
# 屏幕最左边那一竖条（x=4，取样窗开小一点）：没全屏时是 gap（黑背景），全屏之后是 A 的
# 内容（红）—— 「贴到屏幕边上、没有 gap 那圈缝」靠这两条判。
expect_not_color base 4 200 红 "基线：屏幕最左边是 gap（黑背景）" 6
msg toggle-fullscreen --title A
sleep 0.8
expect_box A "0 0 1280 720" "全屏的 A 盖住整块屏（0,0 1280x720）"
grep -aq "全屏: A 开" "$LOG" && pass "日志里写着「全屏: A 开」" ||
	fail "日志里没有全屏那行：$(grep -a '全屏:' "$LOG" | tail -2 | tr '\n' ' ')"
expect_color flush 4 200 红 "盖住左边那条 gap 了（贴到屏幕边上，缝没了）" 6
expect_color content 300 100 红 "屏上是 A 的内容（左上象限红）"
[ "$(visible B)" = "false" ] && pass "后面的列被推到屏外（B 藏起）" ||
	fail "B 还画着（visible=$(visible B)）—— 全屏的列没占整屏宽"

# ------------------------------------------------------------ 2) 右移一列：并排
mark "焦点右移一列：A 滑出去、B 并排露出来（参与滚动）"
msg focus-column 1
sleep 0.8
expect_box A "-514 0 1280 720" "A 跟着条带滑到 -514，尺寸还是整块屏"
expect_box B "766 10 504 700" "B 并排出现在右半屏（766）"
[ "$(visible A)" = "true" ] && pass "A 还画着（滑出去一半没被藏掉）" ||
	fail "A 被藏了（visible=$(visible A)）—— 半块在屏里就该画"
expect_color side-left 200 300 绿 "左边那块是 A 的尾巴（右上象限绿）"
expect_color side-right 900 300 红 "右边那块是 B（左上象限红）"

# ------------------------------------------------------------ 3) 再右移：整块滚出屏
mark "再往右滚：A 露出小半块 → 整块滚出屏 → 藏起"
msg focus-column 1
sleep 0.8
expect_box A "-1028 0 1280 720" "A 露出小半块（-1028，右边缘到 252）"
msg focus-column 1
sleep 0.8
expect_box A "-1542 0 1280 720" "A 整块滚出屏（-1542，右边缘到 -262）"
[ "$(visible A)" = "false" ] && pass "滚出屏的 A 藏起来了（visible=false）" ||
	fail "A 还在画（visible=$(visible A)）—— 屏外那块会落到隔壁显示器上"
grep -aq "workspace: A 藏起（停到屏外）" "$LOG" && pass "日志里写着「A 藏起（停到屏外）」" ||
	fail "日志里没有藏起那行：$(grep -a 'workspace: A' "$LOG" | tail -2 | tr '\n' ' ')"

# ------------------------------------------------------------ 4) 滚回来
mark "左移三次回 A：又严丝合缝盖住整屏"
msg focus-column -1
sleep 0.4
msg focus-column -1
sleep 0.4
msg focus-column -1
sleep 0.8
expect_box A "0 0 1280 720" "回到 A 这条列，它又盖住整块屏"
expect_color back 4 200 红 "还是贴着屏幕左边（缝还是没的）" 6

# ------------------------------------------------------------ 5) 退全屏
mark "退全屏：列宽回到 0.4，后面的列合拢"
msg toggle-fullscreen --title A
sleep 0.8
expect_box A "10 10 504 700" "退出全屏的 A 回到 0.4 宽的列"
expect_box B "524 10 504 700" "B 跟着合拢（从 766 回到 524）"
grep -aq "全屏: A 关" "$LOG" && pass "日志里写着「全屏: A 关」" ||
	fail "日志里没有退全屏那行：$(grep -a '全屏:' "$LOG" | tail -2 | tr '\n' ' ')"
expect_not_color gapback 4 200 红 "退全屏之后屏幕左边又是 gap（壁纸色）" 6

# ------------------------------------------------------------ 6) 滑出去那半块裁在自己那块屏里
mark "滑出屏外那半块裁在自己那块屏里"
grep -aq "clip: A 裁到自己那块屏里（屏框 0,0 1280x720，窗口 -514,0 1280x720）" "$LOG" &&
	pass "日志里写着 A 裁到自己那块屏里（窗口 -514,0 1280x720）" ||
	fail "日志里没有那条裁剪：$(grep -a 'clip: A' "$LOG" | tail -3 | tr '\n' '|')"

# ------------------------------------------------------------ 7) 客户端自己请求全屏
mark "客户端自己要的全屏（preinit --after-commit --fullscreen：OBS 按 F11 / 播放器切全屏那条路）"
# 先把焦点放到最后一条列 D 上：新窗口的列是插在**聚焦那条**后面的，不这么做 E 会插到 A
# 后面去（那它左边那条列就不是 D 了，下面按条带算的数字全错）。
msg focus-window --title D
sleep 0.6
expect_box D "766 10 504 700" "先把焦点放最后一条列上（新窗口的列插在它后面）"
./build/preinit --after-commit --fullscreen --linger=60 preinit E \
	> "$RUNDIR/preinit.out" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do grep -aq "preinit mapped" "$RUNDIR/preinit.out" && break; sleep 0.25; done
sleep 1.2

grep -aq "preinit request fullscreen after-commit" "$RUNDIR/preinit.out" &&
	pass "客户端自己发了 set_fullscreen（上屏之后）" ||
	fail "客户端没发出全屏请求：$(tail -2 "$RUNDIR/preinit.out" | tr '\n' ' ')"
[ "$(field E fullscreen)" = "True" ] && pass "vidow 兑现了：窗口 fullscreen=true" ||
	fail "窗口没进全屏（fullscreen=$(field E fullscreen)）"

msg focus-window --title E
sleep 0.8
expect_box E "0 0 1280 720" "客户端要的全屏同样盖住整块屏（它是定尺寸客户端，也照样整屏）"
[ "$(visible D)" = "false" ] && pass "前面的列被推到屏外（D 藏起）" ||
	fail "D 还画着（visible=$(visible D)）—— 全屏那条列没占整屏宽"

msg focus-column -1
sleep 0.8
expect_box E "514 0 1280 720" "焦点左移一列：全屏的 E 跟着条带往右滑（514）"
expect_box D "10 10 504 700" "D 回到它自己那条列（10）"
[ "$(visible E)" = "true" ] && pass "滑出去半块的 E 还画着" ||
	fail "E 被藏了（visible=$(visible E)）"

msg focus-column 1
sleep 0.8
expect_box E "0 0 1280 720" "焦点右移回来：E 又严丝合缝盖住整屏"
kill "${CLIENT_PIDS##* }" 2>/dev/null   # 收掉 preinit（它按 --linger 还活着）

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
