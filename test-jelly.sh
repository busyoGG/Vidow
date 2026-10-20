#!/bin/bash
# 拖窗口的果冻效果（config 的 jelly / jelly_strength，见 jelly.c）。
#
# 判据（headless 1280x720 一块屏，截图用 grim 当裁判 —— 弯没弯由像素说了算）：
#   1) 拖着走的时候窗口是**弯的**：左边缘在窗口顶部与底部的 x 差 ≥ 5px
#      （抓点在标题栏 = 顶部，往右拖 → 顶部在前、底部拖后腿）
#   2) 松手晃完 → 又直了，而且**窗口还在**（不是变全黑 / 消失 —— 交接时装饰节点
#      的透明度没还回去就踩过这个）
#   3) jelly = false：同一个拖法，边缘是直的（差值 ≤ 2px），日志里没有 jelly
#   4) jelly_strength = 0：还走果冻那条路（日志里有起手），但不弯
#   5) 改大小（Mod+右键）不弯：那条路每帧换源缓冲尺寸，网格对不上
#   6) 平铺窗口（没浮动）拖拽也弯
#   7) 窗口最后落在光标松手的地方（果冻不该把位置语义搞坏）
#   8) 果冻的软硬梯度**只看行号**：抓标题栏和抓底边，形变方向一样（顶部在前、
#      底部拖后腿）—— 早先按「到抓点的距离」算，抓底边时梯度是反的
#   9) 2 倍屏上拖起来窗口不许跑位：那圈留白是设备像素、节点位置是逻辑像素，
#      单位写错整块窗口偏 72 逻辑像素（拖起来「坐标偏移」、松手才回位）
#  10) 半透明 + 模糊的窗口：拖起来连模糊底衬一起画在对的位置（跑位的话模糊的
#      内容也跟着偏，看着就是「背景错位」）
#  11) 拖着**透明**窗口抓模糊底衬时，底衬里不许混进这个窗口自己上一帧的形变图：
#      故意不放壁纸、只放两个重叠窗口（底下那个品红不透明、上面那个半透明绿）。
#      绿色是被拖的那个：它自己的底衬 dump 里不许有绿色（自己混进去了）；
#      品红的底衬里可以同时有品红和绿色 —— 绿色现在会带一层屏幕空间模糊，
#      那层模糊里就有品红（dump 的 owner 写在 /tmp/vidow-bd-<n>.owner 里）
#  12) 模糊采样屏幕坐标（四象限 + 半透明薄纱）
#  13) 日志里没有 assert / abort
#
# 用法：./test-jelly.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py / wall.py。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-jelly-test
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
		*pattern.py*|*wall.py*|*sheet.py*|*build/frames*|*/wev*|*grim*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

# 收尾兜底：把「环境里带着本测试 RUNDIR 的 vidow」全找出来杀掉。不认记下的 pid
# —— setsid 起来到把 pid 记进列表之间有个空档，正好被 timeout / Ctrl-C 打断时
# 漏在那里的进程就没人管了（实测漏过 7 个）。按环境变量认：只碰本测试起的那些，
# 绝不碰用户自己的会话。
kill_stray_vidows() {
	for p in /proc/[0-9]*; do
		local pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		local cmd
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in *build/vidow*) ;; *) continue ;; esac
		local env
		[ -r "$p/environ" ] || continue
		env=$(tr '\0' '\n' < "$p/environ" 2>/dev/null) || continue
		case "$env" in
		*"$RUNDIR"*) kill -KILL "$pid" 2>/dev/null ;;
		esac
	done
}

cleanup() {
	kill_stray_vidows
	stop_clients
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT
# 被 timeout / Ctrl-C 杀掉时也要收尾：bash 收到 SIGTERM 默认不跑 EXIT 陷阱，
# 直接死掉会留下一串没主的 vidow（它们已经 reparent 到 systemd 了，没人再管）。
# `exit 143` 会让 EXIT 陷阱照常跑。
trap 'exit 143' TERM INT

command -v grim >/dev/null || { echo "test-jelly.sh: 需要 grim"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "test-jelly.sh: 需要 python3-PIL"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-jelly.sh: 先构建"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"

start_vidow() { # <额外配置行> [blur 那一行，缺省 blur_enabled = false]
	# 注意：同一个名字在配置里**第一次赋值**才算数（后面那行是普通 Lua 全局），
	# 所以 blur 开关得在这儿替换，不能塞在 $1 里再写一遍。
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
-- 指针手势（拖动 / 改列宽 / 滚轮换列）现在是键位：合成器不内置，测试也得自己绑。
vidow.bind("Mod+mouse-left", "interactive-move")
vidow.bind("Mod+mouse-right", "interactive-resize")
vidow.bind("Mod+wheel-up", "focus-column left")
vidow.bind("Mod+wheel-down", "focus-column right")
scale = "1"
gaps = 10
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
${2:-blur_enabled = false}
$1
CFG
	stop_clients
	rm -f "$RUNDIR/actions" "$LOG"
	mkfifo "$RUNDIR/actions"
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
	exec 9> "$RUNDIR/actions"
	sleep 0.4
}

stop_vidow() {
	stop_clients
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.5
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
		VIDOW_PGID=""
	fi
}

act() { echo "$1" >&9; sleep "${2:-0.3}"; }

# 窗口的浮动态 / 位置尺寸
win_geom() { # <标题> -> "x y w h floating"
	./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    if w["title"] != sys.argv[1]:
        continue
    g = w["float_geometry"] if w["floating"] else w["geometry"]
    print(g["x"], g["y"], g["width"], g["height"], "1" if w["floating"] else "0")' "$1"
}

# 左边缘轮廓：窗口顶部 / 中间 / 底部那三行的最左边非黑像素
edge() { # <png> -> "top mid bottom"（没有窗口就打印 none）
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
W, H = im.size
px = im.load()
bg = px[0, 0]
ys = [y for y in range(H) if any(px[x, y] != bg for x in range(W))]
if not ys:
    print("none")
    sys.exit()
y0, y1 = min(ys), max(ys)
out = []
for frac in (0.05, 0.5, 0.95):
    y = int(y0 + (y1 - y0) * frac)
    row = [x for x in range(W) if px[x, y] != bg]
    out.append(row[0] if row else -1)
print("%d %d %d" % (out[0], out[1], out[2]))
PY
}

# 右边缘轮廓：同上，取每行**最右**那个非黑像素。
# 平铺窗口那一段用这个而不用左边缘：拖平铺窗口时会画一块落点预览（niri 的
# insert hint，见 drag.c）—— 它正好落在窗口原来那个空位上（屏幕左边），量左边缘
# 会量到预览块。形变判据（顶部比底部靠前）两边一样，往右拖就看右边缘。
edge_right() { # <png> -> "top mid bottom"
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
W, H = im.size
px = im.load()
bg = px[0, 0]
ys = [y for y in range(H) if any(px[x, y] != bg for x in range(W))]
if not ys:
    print("none")
    sys.exit()
y0, y1 = min(ys), max(ys)
out = []
for frac in (0.05, 0.5, 0.95):
    y = int(y0 + (y1 - y0) * frac)
    row = [x for x in range(W) if px[x, y] != bg]
    out.append(row[-1] if row else -1)
print("%d %d %d" % (out[0], out[1], out[2]))
PY
}

# 窗口宽（非黑像素的外接盒），用来确认「窗口还在」
win_w() { # <png>
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
W, H = im.size
px = im.load()
bg = px[0, 0]
xs = [x for y in range(0, H, 4) for x in range(W) if px[x, y] != bg]
print(0 if not xs else max(xs) - min(xs) + 1)
PY
}

# 左边缘逐行步进：果冻是**网格**（每格两个三角形、相邻格共用顶点），形变连续，
# 相邻行最多差 1px。老实现是「一格一块轴对齐矩形」—— 剪切形变下相邻行各自取整，
# 左边缘会一段一段跳（屏幕上就是「窗口像好几条矩形拼出来的」+ 锯齿）。
# 输出 "最大步进 ≥2px的跳变处数 总弯"（量不到就 "none"）。
edge_steps() { # <png>
	python3 - "$1" <<'PY'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
W, H = im.size
px = im.load()
bg = px[0, 0]
rows = []
for y in range(H):
    row = [x for x in range(W) if px[x, y] != bg]
    if row:
        rows.append((y, row[0]))
if len(rows) < 100:
    print("none")
    sys.exit()
y0, y1 = rows[0][0], rows[-1][0]
body = [(y, x) for (y, x) in rows if y0 + 30 < y < y1 - 30]
steps = [abs(body[i + 1][1] - body[i][1]) for i in range(len(body) - 1)]
big = [s for s in steps if s >= 2]
print("%d %d %d" % (max(steps), len(big), body[0][1] - body[-1][1]))
PY
}

# 从标题栏起拖：往右快速拖 400px，中途截一张，然后松手
drag_and_shot() { # <png> <x> <y> <dx>
	local png=$1 cx=$2 cy=$3 dx=$4
	act "pointer-motion $cx $cy"
	act "pointer-button left press mod"
	local i
	for i in $(seq 1 8); do
		act "pointer-motion $((cx + i * dx / 8)) $cy" 0.02
	done
	grim "$png" 2>/dev/null
	act "pointer-button left release mod" 0.05
}

# ---------------------------------------------------------------- 1) 浮动窗口
mark "浮动窗口：拖着走是弯的，松手又直回来"
start_vidow ""
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
if [ "$FLOAT" = "1" ] && [ "$WW" -gt 200 ]; then
	pass "浮动窗口 w1：$WX,$WY ${WW}x${WH}"
else
	fail "没拿到浮动窗口（$WX $WY $WW $WH float=$FLOAT）"
fi
drag_and_shot "$RUNDIR/drag.png" "$((WX + WW / 2))" "$((WY + 20))" 400
E=$(edge "$RUNDIR/drag.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "拖动中截不到窗口"
else
	pass "拖动中窗口还在（左边缘 $ET/$EM/$EB）"
	if [ $((ET - EB)) -ge 5 ]; then
		pass "拖着走是弯的：顶部比底部靠前 $((ET - EB))px（≥5）"
	else
		fail "没弯：顶部 $ET、底部 $EB（差 $((ET - EB))，要 ≥5）"
	fi
fi
if grep -aq "jelly: w1 起手" "$LOG"; then
	pass "日志记下了果冻起手：$(grep -a 'jelly: w1 起手' "$LOG" | head -1 | sed 's/.*jelly: /jelly: /')"
else
	fail "没有果冻起手日志"
fi
read -r MAXSTEP NBIG BEND <<<"$(edge_steps "$RUNDIR/drag.png")"
if [ "$MAXSTEP" = "none" ]; then
	fail "量不到左边缘的逐行步进（窗口没截到）"
elif [ "$MAXSTEP" -le 1 ] && [ "$NBIG" = "0" ]; then
	pass "左边缘是连续的：逐行步进最大 ${MAXSTEP}px、没有 ≥2px 的跳变（弯了 ${BEND}px）"
else
	fail "左边缘一段一段跳：逐行步进最大 ${MAXSTEP}px、≥2px 的跳变 ${NBIG} 处（窗口像几条矩形拼的）"
fi

sleep 1.4
grim "$RUNDIR/settled.png" 2>/dev/null
E=$(edge "$RUNDIR/settled.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "松手晃完之后窗口没了（截图全黑）"
else
	if [ $((ET - EB)) -le 2 ] && [ $((EB - ET)) -le 2 ]; then
		pass "晃完又直了：$ET/$EM/$EB（差 $((ET - EB))）"
	else
		fail "晃完还弯着：$ET/$EM/$EB"
	fi
	W=$(win_w "$RUNDIR/settled.png")
	if [ "$W" -gt 200 ]; then
		pass "窗口还在：宽 ${W}px"
	else
		fail "窗口不见了（宽 ${W}px）"
	fi
fi
# 落点：窗口跟着光标走多远就落多远（抓点相对窗口不动，所以位移 == 光标位移）
read -r X1 Y1 W1 H1 F1 <<<"$(win_geom w1)"
WANT=$((WX + 400))
if [ "$X1" -ge $((WANT - 40)) ] && [ "$X1" -le $((WANT + 40)) ]; then
	pass "落在光标松手的地方：x=$X1（期望 ≈$WANT）"
else
	fail "落点不对：x=$X1（期望 ≈$WANT）"
fi
stop_vidow

# ---------------------------------------------------------------- 2) jelly = false
mark "jelly = false：同一个拖法，不弯"
start_vidow "jelly = false"
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
drag_and_shot "$RUNDIR/off.png" "$((WX + WW / 2))" "$((WY + 20))" 400
E=$(edge "$RUNDIR/off.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "jelly=false 时截不到窗口"
elif [ $((ET - EB)) -le 2 ] && [ $((EB - ET)) -le 2 ]; then
	pass "关掉之后边缘是直的：$ET/$EM/$EB（差 $((ET - EB))）"
else
	fail "关掉了还弯：$ET/$EM/$EB（差 $((ET - EB))）"
fi
if grep -aq "jelly: " "$LOG"; then
	fail "关掉了还在起手：$(grep -a 'jelly: ' "$LOG" | head -1)"
else
	pass "关掉之后日志里没有果冻"
fi
stop_vidow

# ---------------------------------------------------------------- 3) 倍率 0
mark "jelly_strength = 0：还走这条路，但不弯"
start_vidow "jelly_strength = 0"
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
drag_and_shot "$RUNDIR/zero.png" "$((WX + WW / 2))" "$((WY + 20))" 400
E=$(edge "$RUNDIR/zero.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "倍率 0 时截不到窗口"
elif [ $((ET - EB)) -le 2 ] && [ $((EB - ET)) -le 2 ]; then
	pass "倍率 0：不弯（$ET/$EM/$EB）"
else
	fail "倍率 0 还弯：$ET/$EM/$EB"
fi
if grep -aq "jelly: w1 起手" "$LOG"; then
	pass "倍率 0 还是走果冻那条路（日志里有起手）"
else
	fail "倍率 0 没走果冻那条路"
fi
stop_vidow

# ---------------------------------------------------------------- 4) 改大小
mark "改大小（Mod+右键）不弯"
start_vidow ""
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
CX=$((WX + WW - 30)); CY=$((WY + WH - 30))
act "pointer-motion $CX $CY"
act "pointer-button right press mod"
for i in 1 2 3 4 5 6; do
	act "pointer-motion $((CX + i * 20)) $((CY + i * 15))" 0.03
done
grim "$RUNDIR/resize.png" 2>/dev/null
act "pointer-button right release mod" 0.1
if grep -aq "jelly: " "$LOG"; then
	fail "改大小也起手了果冻：$(grep -a 'jelly: ' "$LOG" | head -1)"
else
	pass "改大小不弯（日志里没有果冻起手）"
fi
read -r X2 Y2 W2 H2 F2 <<<"$(win_geom w1)"
if [ "$W2" -gt "$WW" ]; then
	pass "窗口确实变大了：${WW} → ${W2}"
else
	fail "改大小没生效：${WW} → ${W2}"
fi
stop_vidow

# ---------------------------------------------------------------- 5) 平铺窗口
mark "平铺窗口（没浮动）拖拽也弯"
start_vidow ""
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
if [ "$FLOAT" = "0" ]; then
	pass "w1 是平铺窗口：$WX,$WY ${WW}x${WH}"
else
	fail "w1 不该是浮动的"
fi
drag_and_shot "$RUNDIR/tile.png" "$((WX + WW / 2))" "$((WY + 20))" 400
E=$(edge_right "$RUNDIR/tile.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "平铺拖拽时截不到窗口"
elif [ $((ET - EB)) -ge 5 ]; then
	pass "平铺窗口拖着走也是弯的：右边缘差 $((ET - EB))px"
else
	fail "平铺窗口没弯：$ET/$EM/$EB（差 $((ET - EB))）"
fi
if grep -aq "jelly: w1 起手" "$LOG"; then
	pass "平铺拖拽也起了果冻"
else
	fail "平铺拖拽没起果冻"
fi
sleep 1.2
grim "$RUNDIR/tile-settled.png" 2>/dev/null
E=$(edge "$RUNDIR/tile-settled.png")
read -r ET EM EB <<<"$E"
if [ "$ET" = "none" ]; then
	fail "平铺窗口松手晃完之后没了"
elif [ $((ET - EB)) -le 2 ] && [ $((EB - ET)) -le 2 ]; then
	pass "平铺窗口晃完也直了：$ET/$EM/$EB"
else
	fail "平铺窗口晃完还弯：$ET/$EM/$EB"
fi
stop_vidow

# ------------------------------------------------- 6) 拖到一半客户端退出
mark "拖到一半客户端退出：不能撞上「节点身上还挂着 destroy 钩子」那条断言"
start_vidow ""
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
CX=$((WX + WW / 2)); CY=$((WY + 20))
act "pointer-motion $CX $CY"
act "pointer-button left press mod"
act "pointer-motion $((CX + 60)) $CY" 0.05
act "pointer-motion $((CX + 140)) $CY" 0.05
if grep -aq "jelly: w1 起手" "$LOG"; then
	pass "拖到一半，果冻正开着"
else
	fail "拖到一半没起果冻（这条测不到东西）"
fi
stop_clients
sleep 0.6
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "客户端退出之后合成器还活着"
else
	fail "客户端退出把合成器带崩了"
fi
if grep -aqE "assert|abort|Assertion" "$LOG"; then
	fail "撞上了断言：$(grep -aE 'assert|abort|Assertion' "$LOG" | head -2)"
else
	pass "日志里没有断言"
fi
act "pointer-button left release mod" 0.1
stop_vidow

# ------------------------------------------------- 8) 梯度只看行号
mark "梯度只看行号：抓标题栏 / 抓底边，形变方向一个样"
start_vidow ""
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
grab_shot() { # <png> <cy>：从 <cy> 那行抓着往右拖 400px，中途截一张
	local png=$1 cy=$2 cx=$((WX + WW / 2))
	act "pointer-motion $cx $cy"
	act "pointer-button left press mod"
	local i
	for i in $(seq 1 8); do
		act "pointer-motion $((cx + i * 50)) $cy" 0.02
	done
	grim "$png" 2>/dev/null
	act "pointer-button left release mod" 0.05
}
grab_shot "$RUNDIR/grab-top.png" $((WY + 20))
read -r TT TM TB <<<"$(edge "$RUNDIR/grab-top.png")"
sleep 1.5
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
grab_shot "$RUNDIR/grab-bottom.png" $((WY + WH - 20))
read -r BT BM BB <<<"$(edge "$RUNDIR/grab-bottom.png")"
if [ "$TT" = "none" ] || [ "$BT" = "none" ]; then
	fail "抓上边 / 抓下边有一张截不到窗口（$TT / $BT）"
else
	if [ $((TT - TB)) -ge 5 ]; then
		pass "抓标题栏：顶部在前 $((TT - TB))px（$TT/$TM/$TB）"
	else
		fail "抓标题栏没弯：$TT/$TM/$TB"
	fi
	if [ $((BT - BB)) -ge 5 ]; then
		pass "抓底边：顶部照样在前 $((BT - BB))px（$BT/$BM/$BB）—— 梯度跟抓点无关"
	else
		fail "抓底边梯度反了：$BT/$BM/$BB（差 $((BT - BB))，要 ≥5）"
	fi
fi
sleep 1.5
stop_vidow

# ------------------------------------------------- 9) 2 倍屏：不许跑位
mark "2 倍屏上拖起来窗口不跑位（留白是设备像素、节点位置是逻辑像素）"
start_vidow 'vidow.output{ name = "HEADLESS-1", scale = 2 }'
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
# 先把窗口摆到屏幕中间：贴边的话偏移会被屏幕边裁掉，量不出那 144px。
./build/vidowmsg center-window --title w1 >/dev/null
sleep 0.3
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
# 截图是**设备**像素（这块屏 2 倍）：逻辑 72px 的偏移在这里是 144px
bbox() { # <png> -> "x y w h"（非黑像素外接盒，设备像素）
	python3 - "$1" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
W, H = im.size
px = im.load()
xs = []
ys = []
for y in range(0, H, 2):
    for x in range(0, W, 2):
        r, g, b = px[x, y]
        if r + g + b > 24:
            xs.append(x)
            ys.append(y)
if not xs:
    print("none")
else:
    print(min(xs), min(ys), max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
PY
}
grim "$RUNDIR/s2-rest.png" 2>/dev/null
read -r RX RY RW RH <<<"$(bbox "$RUNDIR/s2-rest.png")"
CX=$((WX + WW / 2))
CY=$((WY + 20))
act "pointer-motion $CX $CY"
act "pointer-button left press mod" 0.35
grim "$RUNDIR/s2-press.png" 2>/dev/null
read -r PX PY PW PH <<<"$(bbox "$RUNDIR/s2-press.png")"
if [ "$RX" = "none" ] || [ "$PX" = "none" ]; then
	fail "2 倍屏上截不到窗口（静置 $RX / 按住 $PX）"
else
	DX=$((PX - RX))
	DY=$((PY - RY))
	if [ "${DX#-}" -le 2 ] && [ "${DY#-}" -le 2 ]; then
		pass "按住没动，窗口还在原地：位移 $DX,$DY 设备像素（静置 $RX,$RY → 按住 $PX,$PY）"
	else
		fail "按住就跑了 $DX,$DY 设备像素（2 倍屏上单位写错是 144）"
	fi
fi
act "pointer-button left release mod" 0.2
sleep 1.5
stop_vidow

# ------------------------------------------------- 10) 半透明 + 模糊
mark "半透明 + 模糊的窗口：拖起来连底衬一起画对位置"
start_vidow 'vidow.output{ name = "HEADLESS-1", scale = 2 }
window_opacity = 0.5' 'blur_enabled = true'
python3 ./tools/wall.py >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1
python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.6
read -r WX WY WW WH FLOAT <<<"$(win_geom w1)"
# 窗口那一块（逻辑 -> 设备像素，往外留 20px 收住阴影）
RX=$((WX * 2 - 20))
RY=$((WY * 2 - 20))
RW=$((WW * 2 + 40))
RH=$((WH * 2 + 40))
grim "$RUNDIR/blur-rest.png" 2>/dev/null
CX=$((WX + WW / 2))
CY=$((WY + 20))
act "pointer-motion $CX $CY"
act "pointer-button left press mod" 0.35
grim "$RUNDIR/blur-press.png" 2>/dev/null
act "pointer-button left release mod" 0.2
sleep 1.2
# 两张图在**同一个窗口矩形**里逐像素比，外加一个「窗口真的在那儿」的哨兵：
# pattern.py 左半红、右半绿，窗口在的话左边红色分量明显高于右边。窗口整块跑掉
# （留白单位写错就是偏 144px）时，这一块只剩壁纸 —— 两张图差别巨大、而且哨兵
# 也立不住。
DIFF=$(python3 - "$RUNDIR/blur-rest.png" "$RUNDIR/blur-press.png" \
	"$RX" "$RY" "$RW" "$RH" <<'PY'
import sys
from PIL import Image


def load(path):
    return Image.open(path).convert("RGB").load()


pa = load(sys.argv[1])
pb = load(sys.argv[2])
x0, y0, w, h = (int(v) for v in sys.argv[3:7])
m = 16
x0 += m
y0 += m
w -= 2 * m
h -= 2 * m
n = 0
bad = 0
tot = 0
red_l = 0
red_r = 0
for y in range(y0, y0 + h, 2):
    for x in range(x0, x0 + w, 2):
        da = pa[x, y]
        db = pb[x, y]
        d = abs(da[0] - db[0]) + abs(da[1] - db[1]) + abs(da[2] - db[2])
        n += 1
        tot += d
        if d > 90:
            bad += 1
        # 哨兵只在静置那张上量（左边红、右边绿）
        if x < x0 + w // 3:
            red_l += da[0] - da[1]
        elif x > x0 + w - w // 3:
            red_r += da[0] - da[1]
mean = tot / max(n, 1)
frac = bad / max(n, 1)
left = red_l / max(n / 3, 1)
right = red_r / max(n / 3, 1)
print("%.1f %.3f %d %.1f %.1f" % (mean, frac, n, left, right))
PY
)
read -r MEAN FRAC N LEFT RIGHT <<< "$DIFF"
if python3 -c "import sys; sys.exit(0 if $LEFT - $RIGHT > 10 else 1)"; then
	pass "窗口真的在这一块里（左半边 R-G=$LEFT、右半边 $RIGHT —— pattern 左红右绿）"
else
	fail "这一块里没有窗口（左 R-G=$LEFT、右 $RIGHT），这条测不到东西"
fi
if [ "${MEAN%.*}" -le 12 ] && python3 -c "import sys; sys.exit(0 if $FRAC < 0.08 else 1)"; then
	pass "按住前后窗口那一块几乎一样（平均差 $MEAN，差得多的点 $FRAC，$N 个采样）"
else
	fail "拖起来窗口那一块变了样：平均差 $MEAN、差得多的点 $FRAC（$N 个采样）"
fi
if grep -aq "blur: shared rebuild\|blur: own region" "$LOG"; then
	pass "模糊确实在跑（日志里有底衬重建）"
else
	fail "模糊没跑，这条测不到东西"
fi
if grep -aq "jelly: w1 起手" "$LOG"; then
	pass "半透明窗口也起了果冻"
else
	fail "半透明窗口没起果冻"
fi
stop_vidow

# ------------------------------------- 11) 抓底衬不许混进被拖窗口自己
mark "拖着透明窗口抓底衬：底衬里不许混进它自己上一帧的形变图"
stop_vidow
export VIDOW_BLUR_DUMP=400
# 第 2 个参数才是 blur 那行（同一个键第一次赋值才算数，见 start_vidow 的说明）
start_vidow "" "blur_enabled = true"
# 故意**不放壁纸**：底衬里只有别的窗口，混进自己就一眼看得出来（有壁纸时那一块
# 被壁纸盖住，两种颜色也都是「壁纸色」，这个判据就不灵了）。
python3 ./tools/sheet.py under 0.9 0.1 0.8 1.0 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
python3 ./tools/sheet.py green 0.1 0.9 0.2 0.6 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg set-window-position --title under 300 200 >/dev/null
sleep 0.4
./build/vidowmsg set-window-position --title green 100 100 >/dev/null
sleep 0.8
read -r WX WY WW WH FLOAT <<<"$(win_geom green)"
if [ "$WW" -lt 200 ]; then
	fail "半透明窗口没起来（$WX $WY $WW $WH）"
else
	rm -f /tmp/vidow-bd-*.raw /tmp/vidow-bd-*.txt /tmp/vidow-bd-*.owner
	CX=$((WX + WW / 2))
	CY=$((WY + 8))
	act "pointer-motion $CX $CY"
	act "pointer-button left press mod"
	for i in 1 2 3 4 5 6; do
		act "pointer-motion $((CX + i * 20)) $((CY + i * 12))" 0.05
	done
	sleep 0.3
	act "pointer-button left release mod"
	sleep 1.0
	read -r SELF_MIXED SELF_GREEN GREEN_MAG UNDER_GREEN UNDER_MAG NDUMP <<<"$(python3 - <<'PY'
import glob
import numpy as np

# 底衬 dump 的 owner 写在 sidecar 里（/tmp/vidow-bd-<n>.owner）。绿色是
# **被拖的那个**：它自己的底衬里不该有绿色（自己混进去了）；绿色底衬里该有
# 品红（底下那个窗口），品红底衬里该有绿色（被拖窗口，可能已经带着模糊）。
self_mixed = self_green = green_mag = under_green = under_mag = 0
files = sorted(glob.glob("/tmp/vidow-bd-*.raw"))
for path in files:
    base = path[:-4]
    try:
        w, h, fmt = open(base + ".txt").read().split()
        w, h = int(w), int(h)
        a = np.frombuffer(open(path, "rb").read(), dtype=np.uint8).reshape(h, w, 4)
    except Exception:
        continue
    try:
        owner = open(base + ".owner").read().strip()
    except Exception:
        owner = "?"
    R, G, B = a[:, :, 2].astype(int), a[:, :, 1].astype(int), a[:, :, 0].astype(int)
    g = int(((G > 60) & (G > R + 30) & (G > B + 30)).sum())
    m = int(((R > 60) & (B > 60) & (G < 60)).sum())
    if owner == "green":
        if g:
            self_green += 1
        if g and m:
            self_mixed += 1
        if m:
            green_mag += 1
    elif owner == "under":
        if g:
            under_green += 1
        if m:
            under_mag += 1
print("%d %d %d %d %d %d" % (self_mixed, self_green, green_mag,
                             under_green, under_mag, len(files)))
PY
)"
	if grep -aq "jelly: green 起手" "$LOG" && grep -aq "blur: own region" "$LOG"; then
		pass "底衬抓取和果冻都真的在跑（$NDUMP 份 dump）"
	else
		fail "果冻或底衬抓取没跑（$NDUMP 份 dump），这条测不到东西"
	fi
	if [ "$SELF_MIXED" = "0" ] && [ "$SELF_GREEN" = "0" ]; then
		pass "绿色自己的底衬里没有它自己（$NDUMP 份里 0 份混进自己）"
	else
		fail "$SELF_MIXED 份绿色底衬里混进了它自己（$SELF_GREEN 份带绿）—— 被拖窗口的形变图混进底衬了"
	fi
	if [ "$GREEN_MAG" -ge 1 ] && [ "$UNDER_GREEN" -ge 1 ]; then
		pass "两个窗口都抓到了：绿色底衬里有品红 $GREEN_MAG 份，品红底衬里有绿色 $UNDER_GREEN 份（品红那份可带模糊）"
	else
		fail "没抓到两种窗口的底衬（绿底衬带品红 $GREEN_MAG / 品红底衬带绿 $UNDER_GREEN）—— 判据立不住"
	fi
fi
unset VIDOW_BLUR_DUMP
stop_vidow

# ------------------------------------- 12) 模糊采样的是屏幕上的背景
mark "果冻模糊采样的是屏幕上的背景，不是跟着窗口拉长的旧图"
stop_vidow
start_vidow "" "blur_enabled = true"
# 底下 pattern.py 四象限（左上红、右上绿），上面一张半透明绿薄纱。薄纱从
# 四象限边界**右边**往左拖，拖完左半压住红、右半压住绿：
#   - 模糊按屏幕坐标采：窗口左边透出红，R-G > 0；
#   - 模糊跟着窗口内容走（旧实现）：左边仍是绿，R-G < 0。
python3 ./tools/pattern.py under >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.2
python3 ./tools/sheet.py green 0.1 0.9 0.2 0.4 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
./build/vidowmsg set-window-position --title green 450 150 >/dev/null 2>&1
sleep 0.6
read -r WX WY WW WH FLOAT <<<"$(win_geom green)"
if [ "$WW" -lt 200 ] || [ "$FLOAT" != "1" ]; then
	fail "薄纱窗口没起来 / 不是浮动（$WX $WY $WW $WH float=$FLOAT）"
else
	CX=$((WX + WW / 2))
	CY=$((WY + 20))
	act "pointer-motion $CX $CY" 0.2
	act "pointer-button left press mod" 0.35
	for i in 1 2 3 4; do
		act "pointer-motion $((CX - i * 50)) $CY" 0.12
	done
	grim "$RUNDIR/blur-space.png" 2>/dev/null
	act "pointer-button left release mod" 0.2
	sleep 0.8
	read -r LEFT RIGHT <<<"$(python3 - "$RUNDIR/blur-space.png" <<'PY'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")


def patch(x, y):
    vals = [im.getpixel((x + dx, y + dy))
            for dx in range(-2, 3) for dy in range(-2, 3)]
    r = sum(v[0] for v in vals) / len(vals)
    g = sum(v[1] for v in vals) / len(vals)
    return r - g


# pattern.py 的窗口占满第一列（10..635），四象限边界在 x≈322；薄纱拖到
# 250..650，y=200 在它上半部分。280 在红区、380 在绿区。
print("%.1f %.1f" % (patch(280, 200), patch(380, 200)))
PY
)"
	if python3 -c "import sys; sys.exit(0 if $LEFT > 20 and $RIGHT < -20 else 1)"; then
		pass "左边透出红、右边透出绿（R-G：280=$LEFT，380=$RIGHT）—— 背景按屏幕坐标采样"
	else
		fail "左边没透出红 / 右边没透出绿（R-G：280=$LEFT，380=$RIGHT）—— 模糊跟着窗口内容走了"
	fi
fi
stop_vidow

# ---------------------------------------------------------------- 13) 日志
mark "日志里没有 assert / abort"
if grep -aqE "assert|abort|Assertion" "$LOG"; then
	fail "日志里有 assert / abort：$(grep -aE 'assert|abort|Assertion' "$LOG" | head -2)"
else
	pass "日志干净"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-jelly.sh: OK"
else
	echo "test-jelly.sh: 有项目没通过"
	exit 1
fi
