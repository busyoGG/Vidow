#!/bin/bash
# 滚出视口的列不该画到相邻显示器上。
#
# 为什么会有这个问题：wlr_scene 是**一张全局坐标**的场景图，每块屏的场景输出画的
# 是「落在自己那块坐标里的所有节点」。列往右滚出屏宽之后，它的坐标正好落进右边那
# 块屏的框里 —— 于是就在隔壁显示器上出现了（niri 不会：它每块屏各渲染自己那棵，
# 屏外的部分被 framebuffer 自然裁掉）。
#
# 判据：
#   1) 三列里最左边那列滚出左边之后，它被「藏起」（不在场景里）
#   2) 焦点回到最左列时，第三列（x 落在右边那块屏的框里）被藏起 —— 修之前它正好
#      画在隔壁显示器上
#   3) 再滚回去，第三列重新显示、第一列又藏起（不是一次性关掉）
#   4) 藏起来的窗口照样在出帧（视口外补帧，不是被冻住）
#   5) 浮动窗口 / 拖拽不受这条规则管（允许跨屏）—— 见 test-drag.sh
#   6) **半块越界**：还看得见一部分、另一部分跨过屏边界的那种列，跨过去的那一块
#      也不许画到隔壁屏上（修之前只有「整块滚出去」才被摘掉，半块越界的照样串屏：
#      用户报的「滚动后超出视口的窗口会跑到另一个显示器里面」）。判据是**像素**：
#      隔壁那块屏的截图里一个窗口色像素都没有，而自己这块屏上那半块还在。
#   7) **挂着截图源（选择器预览 / OBS 录屏）时，视口外的窗口照样不串屏**：隔壁屏
#      还是 0 个窗口色像素，同时录屏那头拿得到帧、画面在校验和上是变的 ——
#      「藏」必须做成停到屏外（不是关场景节点），关了的话录到 / 预览到的就是黑。
#   8) **半块越界 + 挂着截图源**：也不许漏到隔壁屏（抓帧渲染的是窗口自己的一份
#      镜像，跟屏幕上那份裁没裁没关系，见 src/wincapture.c）。
#   9) **静止的窗口滚到视口外照样出帧**：窗口不重绘时场景里没有任何损伤，按损伤
#      出帧的截图源给不出帧（选择器的预览卡在「等第一帧」，OBS 的流一会儿
#      streaming 一会儿 paused）—— 客户端要一帧就渲染一帧。
#
# 用法：./test-scrollcull.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-scrollcull-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v python3 >/dev/null || { echo "test-scrollcull.sh: 需要 python3"; exit 1; }
python3 -c "import gi; gi.require_version('Gtk','3.0')" 2>/dev/null ||
	{ echo "test-scrollcull.sh: 需要 python3-gobject (GTK3)"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
-- 两块 1280 宽的屏，列宽 0.5：三列（634+6+634+6+634）装不下，必然有列越界
vidow.output{ name = "HEADLESS-1", scale = 1 }
vidow.output{ name = "HEADLESS-2", scale = 1 }
gaps = 6
scale = "1"
-- 补帧是「规则要求保活」才做的事（默认不补：滚出视口的窗口本来就该歇着）。
-- cull-1 用来说明：剔除只影响「画不画」，不影响这条规则要求的补帧。
vidow.rule{ title = "cull-1", force_render = true }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

mark "0) 两块屏并排"
./build/vidowmsg outputs | python3 -c '
import json, sys
outs = json.load(sys.stdin)
if isinstance(outs, dict):
    outs = outs.get("outputs", [])
boxes = {o["name"]: (o["layout"]["x"], o["layout"]["width"]) for o in outs}
print("      " + ", ".join("%s x=%d w=%d" % (n, x, w) for n, (x, w) in sorted(boxes.items())))
ok = len(boxes) == 2 and sorted(x for x, _ in boxes.values()) == [0, 1280]
sys.exit(0 if ok else 1)
' && pass "HEADLESS-1 在 0、HEADLESS-2 在 1280（相邻）" ||
	fail "两块屏不是并排的 0/1280"

# 三个窗口 = 三列
for i in 1 2 3; do
	setsid nohup python3 ./tools/heartbeat.py "cull-$i" > "$RUNDIR/beat-$i.log" 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 60); do grep -aq "place cull-$i -> " "$LOG" && break; sleep 0.2; done
	sleep 0.4
done
sleep 1

place() { grep -a "place cull-$1 -> " "$LOG" | tail -1; }
shown() { grep -a "workspace: cull-$1 显示" "$LOG" | wc -l; }
hidden() { grep -a "workspace: cull-$1 藏起" "$LOG" | wc -l; }
# 「它现在画在屏幕上没有」—— 问 IPC 的 visible（= 节点开着 **且** 没停到屏外）。
# 不看日志计数：开窗 / 滚动的动画里窗口会从屏边界上扫过去，路上被停过一次，计数
# 会把那些瞬态也算进来。
vis() { # vis <列号> -> true / false
	vis_raw "cull-$1"
}
vis_raw() { # vis_raw <标题> -> true / false（它现在画在屏幕上没有）
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
w = next((w for w in json.load(sys.stdin)
          if w.get("title") == sys.argv[1]), None)
print("true" if w and w.get("visible") else "false")' "$1"
}
count() { grep -ac "$1" "$LOG" 2>/dev/null || true; }

mark "1) 三列装不下：最左那列滚出左边 → 藏起"
echo "      $(place 1)"
echo "      $(place 3)"
[ "$(hidden 1)" -ge 1 ] && pass "cull-1 被藏起（滚到屏左边外面）" ||
	fail "cull-1 还留在场景里（$(place 1)）"
[ "$(vis 1)" = false ] && pass "cull-1 报 visible=false（停到屏外）" ||
	fail "cull-1 还报 visible=true"
[ "$(vis 3)" = true ] && pass "cull-3 显示着（焦点在它身上，视口滚到了右边）" ||
	fail "cull-3 当下的 visible=$(vis 3)"

mark "2) 焦点回到最左列：第三列的 x 落进右边那块屏的框 → 藏起"
./build/vidowmsg focus-column -1 >/dev/null 2>&1
sleep 0.4
./build/vidowmsg focus-column -1 >/dev/null 2>&1
sleep 1.0
echo "      $(place 1)"
echo "      $(place 3)"
python3 - "$LOG" <<'PY'
import re, sys
log = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
def last(title):
    m = None
    for m in re.finditer(r"place %s -> (-?\d+),(-?\d+) (\d+)x(\d+)" % title, log):
        pass
    return m
x3 = last("cull-3")
if x3 is None:
    print("  FAIL  日志里没有 cull-3 的 place")
    sys.exit(1)
x, w = int(x3.group(1)), int(x3.group(3))
inside_neighbor = x < 1280 + 1280 and x + w > 1280
print("  %s  cull-3 在 x=%d（宽 %d），%s隔壁屏的框（1280..2560）"
      % ("PASS" if inside_neighbor else "FAIL", x, w,
         "落在" if inside_neighbor else "没落在"))
sys.exit(0 if inside_neighbor else 1)
PY
[ $? -eq 0 ] || FAILED=1
[ "$(hidden 3)" -ge 1 ] && pass "cull-3 被藏起（不再画到隔壁显示器上）" ||
	fail "cull-3 还在场景里 —— 它的坐标落进隔壁屏，会串屏"
[ "$(shown 1)" -ge 1 ] && pass "cull-1 重新显示（焦点回到它身上）" ||
	fail "cull-1 没被放回来"

mark "3) 再滚回右边：第三列显示、第一列又藏起"
n1=$(count "workspace: cull-3 显示")
./build/vidowmsg focus-column 1 >/dev/null 2>&1
sleep 0.4
./build/vidowmsg focus-column 1 >/dev/null 2>&1
sleep 1.0
[ "$(count "workspace: cull-3 显示")" -gt "$n1" ] &&
	pass "cull-3 又显示了（剔除是可逆的，不是关掉一次就没了）" ||
	fail "cull-3 没被放回来"
[ "$(hidden 1)" -ge 2 ] && pass "cull-1 又被藏起" || fail "cull-1 还在画"

mark "4) 藏起来的窗口照样出帧（规则要求保活 → 视口外补帧）"
before=$(grep -c "^frame " "$RUNDIR/beat-1.log" 2>/dev/null || echo 0)
sleep 1.2
after=$(grep -c "^frame " "$RUNDIR/beat-1.log" 2>/dev/null || echo 0)
[ "$after" -gt "$before" ] &&
	pass "cull-1 藏起来之后还在出帧（$before → $after 帧）" ||
	fail "cull-1 被冻住了（帧数 $before → $after）"

winpix() { # winpix <png> -> 窗口色像素个数（heartbeat.py 的蓝底：g≈89、b≈191）
	python3 - "$1" <<'PY'
import sys
from PIL import Image

im = Image.open(sys.argv[1]).convert("RGB")
px = im.load()
n = 0
for y in range(0, im.size[1], 4):
    for x in range(0, im.size[0], 4):
        r, g, b = px[x, y]
        if abs(g - 89) <= 24 and abs(b - 191) <= 24:
            n += 1
print(n)
PY
}
cap_frames() { grep -ac "^capture " "$1" 2>/dev/null || true; }
cap_sums() {
	grep -a "^capture " "$1" 2>/dev/null | awk '{print $3}' | sort -u | wc -l
}

mark "5) 半块越界：跨过屏边界的那一块不画到隔壁屏上"
# 把中间那列调成 2/3 宽（853）：布局变成 634 + 853 + 634 装不下 1280 —— 右边的
# 那列就从屏里伸出去一截，跨过 1280 这条屏边界。修之前越出去的那一截正好落在
# HEADLESS-1（x=1280..2560）的框里、被它画出来。
./build/vidowmsg focus-column -1 >/dev/null 2>&1
sleep 0.3
./build/vidowmsg action cycle-column-width >/dev/null 2>&1
sleep 1.2
# 哪一列跨过了边界（按日志里最后一次 place 算）
straddler=$(python3 - "$LOG" <<'PY'
import re, sys
log = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
last = {}
for m in re.finditer(r"place (cull-\d) -> (-?\d+),(-?\d+) (\d+)x(\d+)", log):
    last[m.group(1)] = (int(m.group(2)), int(m.group(4)))
out = [t for t, (x, w) in sorted(last.items()) if x < 1280 and x + w > 1280]
print(" ".join(out))
PY
)
if [ -n "$straddler" ]; then
	pass "有列跨过了屏边界（$straddler）—— 要测的就是它"
else
	fail "没有列跨过屏边界（布局没变？）"
fi
echo "      $(grep -a 'place cull-' "$LOG" | tail -3 | sed 's/.*place //' | tr '\n' '|')"

grim -o HEADLESS-1 "$RUNDIR/neighbor.png" 2>/dev/null
grim -o HEADLESS-2 "$RUNDIR/own.png" 2>/dev/null
python3 - "$RUNDIR" <<'PY'
import sys
from PIL import Image


def window_pixels(path):
    """heartbeat.py 画的是一块蓝底：g≈89、b≈191（r 在小范围里抖）。"""
    im = Image.open(path).convert("RGB")
    px = im.load()
    n = 0
    for y in range(0, im.size[1], 4):
        for x in range(0, im.size[0], 4):
            r, g, b = px[x, y]
            if abs(g - 89) <= 24 and abs(b - 191) <= 24:
                n += 1
    return n


neighbor = window_pixels(sys.argv[1] + "/neighbor.png")
own = window_pixels(sys.argv[1] + "/own.png")
print("      隔壁屏 %d 个窗口色像素，自己那块屏 %d 个" % (neighbor, own))
ok = True
if neighbor != 0:
    print("  FAIL  隔壁屏上画着越界的那一块（%d 个像素）" % neighbor)
    ok = False
else:
    print("  PASS  隔壁屏上一个窗口色像素都没有（越界的那块被裁掉了）")
if own == 0:
    print("  FAIL  自己这块屏上那半块也没了（裁过头了）")
    ok = False
else:
    print("  PASS  自己这块屏上那半块还在（裁的是越界那一块，不是整个窗口）")
sys.exit(0 if ok else 1)
PY
[ $? -eq 0 ] || FAILED=1

# 5b) 同一列，这次挂着截图源（选择器预览 / OBS 正在录它）：隔壁屏照样一个像素都
# 不许有。裁的是**显示**那一份，抓帧渲染的是窗口自己的一份镜像 —— 两边互不影响。
# 以前这里是个取舍：裁的是场景节点，而 wlroots 那份窗口截图源渲染的就是同一棵树，
# 一裁录到的窗口就少一块，于是「有人录屏就不裁」→ 越界那半块照样串屏。
straddler_one=$(echo $straddler | awk '{print $1}')
if [ -n "$straddler_one" ]; then
	setsid nohup ./build/capture "$straddler_one" 20 10 > "$RUNDIR/cap-5b.txt" 2>&1 &
	CAP_5B=$!
	CLIENT_PIDS="$CLIENT_PIDS $CAP_5B"
	sleep 1.8
	grim -o HEADLESS-1 "$RUNDIR/neighbor-5b.png" 2>/dev/null
	sleep 0.8
	kill "$CAP_5B" 2>/dev/null
	n5b=$(winpix "$RUNDIR/neighbor-5b.png")
	f5b=$(cap_frames "$RUNDIR/cap-5b.txt")
	echo "      挂着截图源的 $straddler_one：隔壁屏 $n5b 个窗口色像素，录到 ${f5b:-0} 帧"
	[ "$n5b" = 0 ] && pass "挂着截图源，越界那半块也不画到隔壁屏上" ||
		fail "挂着截图源就漏到隔壁屏上了（$n5b 个像素）"
	[ "${f5b:-0}" -ge 8 ] && pass "挂着截图源照样录到 ${f5b} 帧" ||
		fail "挂着截图源录不到帧（${f5b:-0} 帧）"
fi

mark "6) 有人录屏 / 预览时，视口外的窗口照样不串屏（两头都对）"
# 选择器一打开就给每个窗口挂一个预览用的截图源（OBS 点共享也是同一条路）。挂着
# 截图源的窗口进「强制渲染」那条路 —— 以前那条路会**跳过剔除**（怕录到一片黑），
# 于是视口外的窗口整个身子又画到隔壁屏上了（用户报的「开了 obs 然后 portal 选择
# 开启的时候，视口外的窗口就完全没藏住」）。现在「藏」= 停到所有输出框外面，而
# 截图源那个虚拟输出是跟着节点位置走的（它渲染的就是这棵树）—— 隔壁屏一个像素
# 都没有，录屏照样拿到新画面。
#
# 两个顺序都要测：先挂上再滚出去（OBS 先开、再滚窗口），以及窗口已经在视口外
# 才挂上（选择器预览 / OBS 选的就是一个当前看不见的窗口）。
out_of_view() { # out_of_view <列号> -> 0 = 它整个滚出自己那块屏了
	python3 - "$LOG" "$1" <<'PY'
import re, sys
log = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
last = None
for m in re.finditer(r"place cull-%s -> (-?\d+),(-?\d+) (\d+)x(\d+)" % sys.argv[2],
                     log):
    last = m
x, w = int(last.group(1)), int(last.group(3))
print("  %s  cull-%s 整个滚出自己那块屏（x=%d，宽 %d）" % (
    "PASS" if x >= 1280 else "FAIL", sys.argv[2], x, w))
sys.exit(0 if x >= 1280 else 1)
PY
}
# 6a) 先挂上截图源（窗口还在画面上，跟 OBS 点共享一样），再把窗口滚出视口
./build/vidowmsg focus-column 1 >/dev/null 2>&1
sleep 1.0
echo "      $(place 3)"
setsid nohup ./build/capture cull-3 20 10 > "$RUNDIR/cap-a.txt" 2>&1 &
CAP_A=$!
CLIENT_PIDS="$CLIENT_PIDS $CAP_A"
sleep 1.2
./build/vidowmsg focus-column -1 >/dev/null 2>&1
sleep 0.4
./build/vidowmsg focus-column -1 >/dev/null 2>&1
sleep 1.0
echo "      $(place 3)"
out_of_view 3 || FAILED=1
[ "$(vis 3)" = false ] && pass "挂着截图源也照样停到屏外（不是关节点：关了录到黑的）" ||
	fail "cull-3 没被停到屏外"
grim -o HEADLESS-1 "$RUNDIR/neighbor2.png" 2>/dev/null
sleep 1.0
kill "$CAP_A" 2>/dev/null
n=$(winpix "$RUNDIR/neighbor2.png")
echo "      隔壁屏 $n 个窗口色像素（录屏进行中，窗口在视口外）"
[ "$n" = 0 ] && pass "隔壁屏上一个窗口色像素都没有（有人录屏也一样）" ||
	fail "挂着截图源时越界的窗口又画到隔壁屏上了（$n 个像素）"
fa=$(cap_frames "$RUNDIR/cap-a.txt")
sa=$(cap_sums "$RUNDIR/cap-a.txt")
[ "${fa:-0}" -ge 10 ] && pass "窗口滚出视口前后都录到了帧（$fa 帧）" ||
	fail "录不到帧（$fa 帧）"
[ "${sa:-0}" -ge 2 ] && pass "抓到的画面在校验和上有 $sa 种（是活的，不是冻住 / 全黑的一张）" ||
	fail "抓到的画面不变（$sa 种校验和）—— 录到的是冻住的一张或一片黑"

# 6b) 窗口已经在视口外，这时候才挂上截图源（选择器预览 / OBS 选一个看不见的窗口）
setsid nohup ./build/capture cull-3 20 10 > "$RUNDIR/cap-b.txt" 2>&1 &
CAP_B=$!
CLIENT_PIDS="$CLIENT_PIDS $CAP_B"
sleep 2.0
kill "$CAP_B" 2>/dev/null
fb=$(cap_frames "$RUNDIR/cap-b.txt")
sb=$(cap_sums "$RUNDIR/cap-b.txt")
echo "      $(tail -1 "$RUNDIR/cap-b.txt" 2>/dev/null)"
[ "${fb:-0}" -ge 10 ] && pass "视口外的窗口照样录到 $fb 帧（挂的时候它就已经在屏外）" ||
	fail "视口外的窗口录不到帧（$fb 帧）—— 截的是关掉的节点，一片黑"
[ "${sb:-0}" -ge 2 ] && pass "抓到的画面在校验和上有 $sb 种（是活的，不是冻住 / 全黑的一张）" ||
	fail "抓到的画面不变（$sb 种校验和）—— 录到的是冻住的一张或一片黑"
# 6c) 窗口所在的工作区被藏起来（切到别的工作区）：选择器里也得有预览
./build/vidowmsg focus-workspace 2 >/dev/null 2>&1
sleep 1.0
[ "$(vis 3)" = false ] && pass "切到工作区 2：cull-3 不画了（工作区藏起来了）" ||
	fail "cull-3 还报 visible=true"
setsid nohup ./build/capture cull-3 20 10 > "$RUNDIR/cap-c.txt" 2>&1 &
CAP_C=$!
CLIENT_PIDS="$CLIENT_PIDS $CAP_C"
sleep 2.0
kill "$CAP_C" 2>/dev/null
fc=$(cap_frames "$RUNDIR/cap-c.txt")
sc=$(cap_sums "$RUNDIR/cap-c.txt")
echo "      $(tail -1 "$RUNDIR/cap-c.txt" 2>/dev/null)"
[ "${fc:-0}" -ge 10 ] && pass "藏起来的工作区里的窗口照样录到 $fc 帧" ||
	fail "藏起来的工作区里的窗口录不到帧（$fc 帧）"
[ "${sc:-0}" -ge 2 ] && pass "抓到的画面在校验和上有 $sc 种（是活的，不是冻住 / 全黑的一张）" ||
	fail "抓到的画面不变（$sc 种校验和）—— 录到的是冻住的一张或一片黑"

# 6d) 滚回来的**那一瞬间**也不许串屏：动画中途连抓几张隔壁屏
# 判据只能按「这一帧画在哪儿」（几何 + 动画偏移）算：按布局目标位置算的话，滚回来
# 的路上（目标已经落在自己那块屏里、画面还在屏外）会被当成「在屏里」，越界的那一块
# 就画到隔壁屏上了 —— 用户报的「视口外的窗口回来的瞬间会显示在另一个显示器上」
# （他只开着 kitty，没有录屏）。动画调到 900ms，抓的那几张一定落在动画中途。
./build/vidowmsg focus-workspace 1 >/dev/null 2>&1
sleep 1.0
./build/vidowmsg set-config window_move_ms 900 >/dev/null 2>&1
n_tween=$(count "move tween")
./build/vidowmsg focus-column 1 >/dev/null 2>&1
sleep 0.25
./build/vidowmsg focus-column 1 >/dev/null 2>&1
for i in 1 2 3 4; do
	sleep 0.2
	grim -o HEADLESS-1 "$RUNDIR/mid-$i.png" 2>/dev/null
done
sleep 1.2
[ "$(count "move tween")" -gt "$n_tween" ] &&
	pass "抓的这几张确实落在滚动动画里（日志里有新的 move tween）" ||
	fail "没抓到动画（日志里没有新的 move tween）"
mid_bad=0
mid_px=""
for i in 1 2 3 4; do
	p=$(winpix "$RUNDIR/mid-$i.png")
	mid_px="$mid_px $p"
	[ "$p" = 0 ] || mid_bad=1
done
echo "      动画中途四张：隔壁屏窗口色像素 =$mid_px"
[ "$mid_bad" = 0 ] && pass "滚回来的路上隔壁屏一直是 0 个窗口色像素" ||
	fail "滚回来的路上串屏了（$mid_px）"

# 6e) 录着它滚出视口：录屏那头帧不能断，隔壁屏这一档也不许漏 —— 抓帧渲染的是窗口
# 自己的一份镜像，跟屏幕上那份裁没裁没关系（以前这里是个取舍，见 5b 的说明）。
setsid nohup ./build/capture cull-3 20 10 > "$RUNDIR/cap-e.txt" 2>&1 &
CAP_E=$!
CLIENT_PIDS="$CLIENT_PIDS $CAP_E"
sleep 0.6
n_tween=$(count "move tween")
./build/vidowmsg focus-column -1 >/dev/null 2>&1
sleep 0.25
./build/vidowmsg focus-column -1 >/dev/null 2>&1
sleep 1.6
grim -o HEADLESS-1 "$RUNDIR/mid-e.png" 2>/dev/null
kill "$CAP_E" 2>/dev/null
fe=$(cap_frames "$RUNDIR/cap-e.txt")
se=$(cap_sums "$RUNDIR/cap-e.txt")
ne=$(winpix "$RUNDIR/mid-e.png")
echo "      录着它滚出视口：隔壁屏 $ne 个窗口色像素，录到 ${fe:-0} 帧"
[ "$ne" = 0 ] && pass "录着它滚出视口，隔壁屏一个窗口色像素都没有" ||
	fail "录着它滚出视口又漏到隔壁屏上了（$ne 个像素）"
[ "${fe:-0}" -ge 10 ] && pass "录着它滚出视口照样录到 $fe 帧" ||
	fail "录着它滚出视口录不到帧（$fe 帧）"
[ "${se:-0}" -ge 2 ] && pass "抓到的画面在校验和上有 $se 种（是活的）" ||
	fail "抓到的画面不变（$se 种校验和）"
./build/vidowmsg set-config window_move_ms 200 >/dev/null 2>&1
sleep 0.5

mark "6f) 静止的窗口滚到视口外：照样出帧（按损伤出帧那条路的老毛病）"
# bluewin 是一块纯色、画完就歇的窗口：它滚到视口外之后场景里**没有任何损伤**。
# wlroots 那份窗口截图源只在场景有损伤时才渲染（"We can only emit frames with
# damage"），客户端 `capture` 要的那一帧就永远不来 —— 选择器的预览卡在「等第一帧」
# 而且不再重试（把窗口挪回视口也还是没预览），OBS 的流一会儿 streaming 一会儿
# paused（error 是 none：协议里没这个错，就是没帧）。量过：修之前 3 秒只出 1 帧。
setsid nohup python3 ./tools/bluewin.py > "$RUNDIR/blue.log" 2>&1 &
BLUE_PID=$!
CLIENT_PIDS="$CLIENT_PIDS $BLUE_PID"
for _ in $(seq 1 60); do grep -aq "place bluewin -> " "$LOG" && break; sleep 0.2; done
sleep 0.8
for _ in 1 2 3 4; do
	[ "$(vis_raw bluewin)" = false ] && break
	./build/vidowmsg focus-column 1 >/dev/null 2>&1
	sleep 0.4
done
sleep 1.0
echo "      $(grep -a 'place bluewin -> ' "$LOG" | tail -1 | sed 's/.*place //')"
if [ "$(vis_raw bluewin)" = false ]; then
	pass "bluewin 停到屏外了（静止窗口，没人给它补帧）"
else
	fail "bluewin 还在画面上，这一节测不到"
fi
setsid nohup ./build/capture bluewin 3 30 > "$RUNDIR/cap-f.txt" 2>&1 &
CAP_F=$!
CLIENT_PIDS="$CLIENT_PIDS $CAP_F"
sleep 4.0
kill "$CAP_F" 2>/dev/null
ff=$(cap_frames "$RUNDIR/cap-f.txt")
echo "      静止窗口在视口外抓 3 秒：$(grep -a 'buffer_size' "$RUNDIR/cap-f.txt" | tail -1 | sed 's/.*buffer_size/buffer_size/')，${ff:-0} 帧"
[ "${ff:-0}" -ge 30 ] && pass "静止窗口滚到视口外照样出 ${ff} 帧（要一帧渲染一帧）" ||
	fail "静止窗口滚到视口外只有 ${ff:-0} 帧 —— 客户端要的那一帧被丢了（修之前是 1 帧）"
kill "$BLUE_PID" 2>/dev/null

mark "7) 没崩、没断言"
pgrep -x vidow >/dev/null && pass "合成器还活着" || fail "合成器挂了"
grep -aq "Assertion" "$LOG" && fail "日志里有 assert" || pass "日志里没有 assert"

echo
if [ $FAILED -eq 0 ]; then
	echo "test-scrollcull.sh: OK"
else
	echo "test-scrollcull.sh: 有失败项"
fi
exit $FAILED
