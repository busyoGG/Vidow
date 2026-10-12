#!/bin/bash
# 「窗口不重叠时的模糊背景」必须是壁纸的模糊，而不是一块缩放 + 错位的壁纸。
#
# 场景：壁纸 + 半透明浮窗 A（模糊底衬从它背后透出来）+ 另一个浮窗 B。
#   A 单独待着      → 它用**共享**底衬（整屏壁纸那张，fx->blur_tex_w == 0）
#   B 压上来        → A 改用**私有**区域底衬（它那块 768x768，fx->blur_tex_* = 区域）
#   B 再挪走        → A 换回共享底衬
#
# 修之前：换回共享贴图时只换了贴图、没换区域（fx->blur_tex_x/y/w/h 还是那块私有
# 区域的），装饰着色器于是拿整屏那张贴图按 768x768 的区域去采样 —— A 的客户区里
# 是一块被缩放 + 错位的壁纸模糊（用户报的「窗口没有重叠时模糊背景是错的」）。
#
# 判据：
#   1) 前提：A 真的走过私有区域那条路（日志有「blur: own region wA」）
#   2) 前提：B 压着 A 时，A 的模糊里真的有 B（那块像素偏蓝）—— 反向对照，
#      证明私有区域不是白建的
#   3) A 的客户区在「从没重叠」和「重叠过后又分开」两张截图里逐像素一致
#      （只比客户区：标题栏 / 焦点边框的颜色会因为焦点变了而变，那是另一回事）
#   4) 分开之后 A 用的是共享贴图（日志里 A 那条路的区域记号回到整屏）
#   5) vidow 全程活着
#
# 用法：./test-blurnooverlap.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurnooverlap-test
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

for tool in grim python3; do
	command -v "$tool" >/dev/null || {
		echo "test-blurnooverlap.sh: 需要 $tool"; exit 1; }
done
python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-blurnooverlap.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

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
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

mark "窗口不重叠时的模糊底衬，必须和「从没重叠过」时一致"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
sleep 1.5

geom_of() { # <标题> -> "x y w h floating"
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
w = next((w for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), None)
if w is None:
    print("? ? ? ? ?")
else:
    g = w["geometry"]
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
	sleep 0.8
}

# A：半透明薄纱（400x300，模糊从它背后透出来）
setsid nohup python3 ./tools/sheet.py wA 0 0 0 0.5 > "$RUNDIR/a.log" 2>&1 </dev/null &
sleep 1.8
float_at wA 100 100
sleep 1.0
echo "    A 单独：$(geom_of wA)"
grim -o HEADLESS-1 "$RUNDIR/ref.png" 2>/dev/null

# B：压在 A 上（400x300 那个盒子 100..512 × 100..449 和 B 的 300..712 × 250..599 重叠）
setsid nohup python3 ./tools/sheet.py wB 0.2 0 1 0.9 > "$RUNDIR/b.log" 2>&1 </dev/null &
sleep 1.8
float_at wB 300 250
sleep 1.2
echo "    A + B 重叠：$(geom_of wA) / $(geom_of wB)"
grim -o HEADLESS-1 "$RUNDIR/overlap.png" 2>/dev/null

# B 挪走（不再重叠）
./build/vidowmsg set-window-position --title wB 1000 700 >/dev/null 2>&1
sleep 1.5
echo "    B 挪走：$(geom_of wA) / $(geom_of wB)"
grim -o HEADLESS-1 "$RUNDIR/after.png" 2>/dev/null
AFTER_OFF=$(wc -c < "$LOG")

grep -a "blur: own region" "$LOG" | tail -3 | sed 's/^/    /'

mark "断言"
python3 - "$RUNDIR" "$(geom_of wA)" "$AFTER_OFF" <<'PY'
import os, re, sys
from PIL import Image

rundir = sys.argv[1]
gx, gy, gw, gh = (int(v) for v in sys.argv[2].split()[:4])
after_off = int(sys.argv[3])
raw = open(os.path.join(rundir, "log"), "rb").read()
log = raw.decode("utf-8", "replace")
failed = []

def check(cond, msg):
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        failed.append(msg)

def img(name):
    return Image.open(os.path.join(rundir, name)).convert("RGB")

# 1) 前提：A 真的建过私有区域底衬（说明两个窗口真的重叠了，走的不是共享那条路）
own = re.findall(r"blur: own region wA (\d+)x(\d+) at (-?\d+),(-?\d+)", log)
check(bool(own), "前提：A 重叠时建过私有区域底衬（%s）"
      % ((own[-1],) if own else ("没有",)))

# 2) 反向对照：B 压着 A 时，A 的客户区里那块（B 的背后）偏蓝
#    A 盒子 100..512 × 100..449，B 盒子 300..712 × 250..599 → 取交集中间一点
ox, oy = gx + 250, gy + 200
if 300 <= ox < 712 and 250 <= oy < 599:
    p = img("overlap.png").getpixel((ox, oy))
    check(p[2] > p[0] + 10 and p[2] > p[1] + 10,
          "对照：B 压着 A 时 A 的底衬里看得到 B 的蓝（(%d,%d)=%s）" % (ox, oy, p))
else:
    check(False, "对照：取的点 (%d,%d) 不在 B 的盒子里，测试自己写错了" % (ox, oy))

# 3) 主判据：客户区（躲开标题栏和焦点边框）逐像素一致
x1, y1 = gx + 20, gy + 55          # 标题栏 ≈ 43 + 边框，再往里缩一点
x2, y2 = gx + gw - 20, gy + gh - 20
ca = img("ref.png").crop((x1, y1, x2, y2))
cb = img("after.png").crop((x1, y1, x2, y2))
pa, pb = ca.load(), cb.load()
worst = 0
bad = 0
for y in range(ca.size[1]):
    for x in range(ca.size[0]):
        d = max(abs(p - q) for p, q in zip(pa[x, y], pb[x, y]))
        worst = max(worst, d)
        if d > 6:
            bad += 1
total = ca.size[0] * ca.size[1]
check(bad == 0, "A 的客户区 %d 个像素逐点一致（最大差 %d，超阈 %d 个）"
	% (total, worst, bad))

# 4) 分开之后 A 不再走私有区域那条路：从「B 挪走」那一刻往后，日志里没有新的
#    「own region wA」（有的话说明 A 还在用那块 768x768 的私有底衬）
tail = raw[after_off:].decode("utf-8", "replace")
check("blur: own region wA" not in tail,
      "分开之后 A 不再重建私有区域底衬（切回共享那张）")
check("Assertion" not in log and "abort" not in log.lower(),
      "vidow 全程活着（日志里没有 assert / abort）")

print()
if failed:
    print("test-blurnooverlap.sh: %d 项没通过" % len(failed))
    sys.exit(1)
print("全部通过 / test-blurnooverlap.sh: OK")
PY
[ $? -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "test-blurnooverlap.sh: OK"
else
	echo "test-blurnooverlap.sh: 有失败项"
fi
exit $FAILED
