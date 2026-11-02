#!/bin/bash
# 跨窗口拖拽时的私有底衬：**被拖的那个窗口压在别的窗口上时，底衬要采到那个窗口**。
#
# 用户 2026-11-03：「跨窗口之后底衬会闪烁，而且跨窗口的时候没有采集另一个窗口的
# 背景，完全跨过去也没有采集」（浮窗 / Super 拖拽都会）。三件事一起验：
#
#   1) 拖到另一个窗口上面、按住不放的那一帧：被拖的（半透明）窗口里，压在下面
#      那个**纯蓝**窗口上的那一块必须是「半透明白 + 蓝底」混出来的淡蓝，
#      不能是白的（白 = 底衬那一块根本没采到下面的窗口）。
#   2) 同一块像素，松手之后（静止）同样是淡蓝 —— 拖拽中和静止两种状态一致，
#      这就是用户说的「闪烁」：两种状态各糊各的、来回换。
#   3) vidow 全程活着。
#
# 怎么拖：headless 里没有真鼠标，走 `VIDOW_TEST_ACTION` 的 FIFO 发
#   pointer-motion / pointer-button（和真实输入同一条路，见 test-drag.sh）。
# 两个窗口都用 tools/sheet.py（定尺寸、可上色）：下面那个不透明纯蓝，
# 上面那个半透明白 —— 白纱后面透出来的只要有蓝，就说明底衬里画着下面那个窗口。
#
# 用法：./test-dragblur-xwin.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试并行跑 —— 收尾按 cmdline 杀 sheet.py / wall.py。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-dragblur-xwin-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""

mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in *sheet.py*|*wall.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1920x1080" }
blur_enabled = true
blur_all = true
blur_levels = 4
blur_offset = 1.5
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
jelly = true
jelly_strength = 1
scale = "1"
-- 拖动窗口 = 键位（合成器不内置），和用户的 Super+左键同一条路
vidow.bind("Mod+mouse-left", "interactive-move")
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug VIDOW_BLUR_TRACE=1
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

BIN=${VIDOW_BIN:-./build/vidow}
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }

geom_of() { # <标题> -> "x y w h floating"
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
w = next((w for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), None)
if w is None:
    sys.exit(1)
g = w["float_geometry"] if w["floating"] else w["geometry"]
print(g["x"], g["y"], g["width"], g["height"], w["floating"])' "$1"
}

float_at() { # <标题> <x> <y>
	local t="$1" x="$2" y="$3"
	for _ in $(seq 1 30); do
		./build/vidowmsg windows 2>/dev/null | grep -q "\"$t\"" && break
		sleep 0.3
	done
	for _ in $(seq 1 12); do
		./build/vidowmsg toggle-floating --title "$t" >/dev/null 2>&1
		[ "$(geom_of "$t" | awk '{print $5}')" = "True" ] && break
		sleep 0.3
	done
	./build/vidowmsg set-window-position --title "$t" "$x" "$y" >/dev/null 2>&1
	sleep 0.9
}

mark "场景：不透明纯蓝的 wB + 半透明白的 wA，把 wA 拖到 wB 上面"

# B：不透明纯蓝（它是「另一个窗口」，被拖的窗口要采到它）
setsid nohup python3 ./tools/sheet.py wB 0 0 1 1 > "$RUNDIR/b.log" 2>&1 </dev/null &
sleep 1.6
float_at wB 900 300
echo "    wB（蓝）：$(geom_of wB)"

# A：半透明白（它背后的底衬透出来）
setsid nohup python3 ./tools/sheet.py wA 1 1 1 0.5 > "$RUNDIR/a.log" 2>&1 </dev/null &
sleep 1.8
float_at wA 100 100
echo "    wA（白）：$(geom_of wA)"

AX=$(geom_of wA | awk '{print $1}')
AY=$(geom_of wA | awk '{print $2}')
ACT_BEFORE=$(wc -c < "$LOG")

# 按住窗口中心，一口气拖到 (AX+700, AY+200)：wA 的盒子变成
# (AX+700..AX+1112, AY+200..AY+549)，和 wB 的 (900..1312, 300..649) 相交。
# 到位之后还在小幅度左右晃：果冻在这一段一直变形，**闪烁就发生在这几帧里**，
# 停稳之后才截一张是看不到的（以前那样写会假通过）。
act "pointer-motion $((AX + 200)) $((AY + 150))"
act "pointer-button left press mod"
echo "pointer-motion $((AX + 700)) $((AY + 200))" >&9
sleep 0.05
for i in $(seq 0 5); do
	dx=$((i % 2 == 0 ? 10 : -10))
	echo "pointer-motion $((AX + 700 + dx)) $((AY + 200 + dx))" >&9
	sleep 0.08
	grim -o HEADLESS-1 "$RUNDIR/drag-$i.png" 2>/dev/null
done
sleep 0.5
grim -o HEADLESS-1 "$RUNDIR/during.png" 2>/dev/null
act "pointer-button left release mod" 0.9
grim -o HEADLESS-1 "$RUNDIR/after.png" 2>/dev/null
echo "    wA 松手后：$(geom_of wA)"
ACT_AFTER=$(wc -c < "$LOG")

mark "日志（拖动期间 wA 的私有底衬重抓）"
grep -a "blur: own region wA" "$LOG" | tail -3 | sed 's/^/    /'

mark "断言"
python3 - "$RUNDIR" "$AX" "$AY" "$ACT_BEFORE" "$ACT_AFTER" <<'PY'
import os, sys
from PIL import Image

rundir = sys.argv[1]
ax, ay = int(sys.argv[2]), int(sys.argv[3])
before, after = int(sys.argv[4]), int(sys.argv[5])
failed = []

def check(cond, msg):
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        failed.append(msg)

def img(name):
    return Image.open(os.path.join(rundir, name)).convert("RGB")

# 拖动中 / 松手后取同一块像素：wA 松手后的盒子从截图里看不出，用拖拽的落点算 ——
# 抓点 (AX+200, AY+150) 在窗口里的偏移不变，落点 = (AX+700, AY+200)。
bx, by = ax + 700, ay + 200
# 取样：wA 客户区内部、wB 盒子 (900..1312, 300..649) 里面那块
x1 = max(bx + 30, 910)
x2 = min(bx + 412 - 30, 1300)
y1 = max(by + 60, 320)
y2 = min(by + 349 - 30, 640)
if x2 - x1 < 40 or y2 - y1 < 40:
    check(False, "取样框算空了（wA=%d,%d wB=900,300），测试自己写错了"
          % (bx, by))
else:
    def stats(name):
        im = img(name)
        reg = im.crop((x1, y1, x2, y2))
        px = reg.load()
        n = blue = white = 0
        for y in range(reg.size[1]):
            for x in range(reg.size[0]):
                r, g, b = px[x, y]
                n += 1
                if b > r + 25 and b > g + 25:
                    blue += 1
                if min(r, g, b) > 240:
                    white += 1
        return blue / n, white / n

    # 1) 前提：拖动期间 wA 真的重抓过自己的私有底衬（不然这条测试什么也没测到）
    log = open(os.path.join(rundir, "log"), "rb").read()[before:after]
    log = log.decode("utf-8", "replace")
    own = log.count("blur: own region wA")
    check(own > 0,
          "前提：拖动期间 wA 的私有底衬重抓过（%d 次）" % own)

    # 2) 主判据：**拖动过程中的每一帧**，那一块都得是「白 + 蓝底」混出来的淡蓝。
    #    只截停稳之后那一张看不到闪烁 —— 白斑只出现在果冻还在变形的那几帧。
    shots = ["drag-%d.png" % i for i in range(6)]
    shots = [n for n in shots if os.path.exists(os.path.join(rundir, n))]
    if shots:
        worst_blue, worst_white, worst_name = 1.0, 0.0, ""
        for n in shots:
            blue, white = stats(n)
            if blue < worst_blue:
                worst_blue, worst_name = blue, n
            worst_white = max(worst_white, white)
        check(worst_blue > 0.30,
              "拖动中每一帧看得到蓝（最差 %s：%.0f%% 偏蓝，最多 %.0f%% 近白）"
              % (worst_name or "-", worst_blue * 100, worst_white * 100))
        # 光有蓝不够：半透明白压在蓝上本来就是淡蓝，底衬一坏也还可能**一块是
        # 淡蓝、一块是纯白**（线性光那条路把 alpha 与色值不自洽的像素放大成
        # 纯白，用户报的「闪烁」就是这个）。整块区域里纯白必须是个零头。
        check(worst_white < 0.12,
              "拖动中每一帧都不是一片纯白（最差 %s：%.0f%% 近白）"
              % (worst_name or "-", worst_white * 100))
    else:
        check(False, "一张拖动中的截图都没有（grim 失败）")

    # 3) 松手之后同一块像素：还是淡蓝（拖拽中 / 静止两种状态一致，不闪）
    if (os.path.exists(os.path.join(rundir, "during.png")) and
            os.path.exists(os.path.join(rundir, "after.png"))):
        b1, w1 = stats("during.png")
        b2, w2 = stats("after.png")
        check(abs(b1 - b2) < 0.15,
              "拖动中 / 松手后同一块像素一致（偏蓝 %.0f%% vs %.0f%%）"
              % (b1 * 100, b2 * 100))

sys.exit(1 if failed else 0)
PY
RC=$?

mark "vidow 还活着吗"
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	echo "  PASS  vidow 全程活着"
else
	echo "  FAIL  vidow 退出了"
	tail -5 "$LOG"
	FAILED=1
fi
[ "$RC" -ne 0 ] && FAILED=1

echo
if [ "$FAILED" -eq 0 ]; then
	echo "== 全部 PASS =="
else
	echo "== 有 FAIL =="
fi
exit "$FAILED"
