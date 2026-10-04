#!/bin/bash
# 显示器热插拔：拔掉一块屏，它上面的东西先挪到别的屏上住（归属记着）；
# 同一块屏插回来，按记忆还原工作区 —— 列的顺序、列宽、滚动位置都留着。
#
# 判据（全部来自日志和像素）：
#   1) 拔线：日志里「拔掉 X（N 列 / M 个窗口 / K 个浮动窗口），先挪到 Y 上住」
#      —— 列 / 窗口 / layer surface 都算进去
#   2) 插回来：「X 回来了，还原 N 列 / M 个窗口 / K 个浮动窗口」
#   3) 还原得对：列的先后顺序、列与列的间距（= 列宽 + 间隙）、三个窗口相对自己那块
#      屏的位置，都和拔线前一模一样。屏回来时布局位置会变，所以绝对坐标整体平移 ——
#      平移量必须正好等于「新原点 - 旧原点」，滚动位置和列偏移都还原了才会这样
#   4) 窗口真的画在那块屏上：按日志坐标采样窗口的四象限颜色（红绿蓝白）
#   5) 屏真的没了 / 回来了：grim 截出来的画面尺寸随之变化
#   6) layer surface（壁纸/状态栏）也跟着搬家：没搬的话接收的那块屏底下是黑的
#
# 拔插是靠 VIDOW_TEST_OUTPUTS 这个测试钩子触发的 —— 它调用的是 wlr_output_destroy
# 和 wlr_headless_add_output，走 backend 的真实事件路径，不是模拟状态。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-hotplug-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*glass.py*|*wall.py*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
	if [ -n "${VIDOW_PGID:-}" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	rm -rf "$RUNDIR"
}
trap cleanup EXIT

for tool in grim wtype; do
	if ! command -v "$tool" >/dev/null; then
		echo "test-hotplug.sh: 需要 $tool"
		exit 1
	fi
done
python3 -c "import gi" 2>/dev/null || {
	echo "test-hotplug.sh: 需要 python3-gobject（GTK3 测试客户端）"
	exit 1
}

# 时间线（毫秒，相对 vidow 启动）：9000 拔屏，18000 插回来
T_SHOT1=6500
T_REMOVE=9000
T_SHOT2=11000
T_ADD=18000
T_SHOT3=19500

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
outputs = "HEADLESS-1:1 HEADLESS-2:1"
xwayland = false
window_open_ms = 150
window_close_ms = 150
window_move_ms = 150
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
# 测试钩子：拔掉左边那块屏，再把一块新屏以它的名字插回来
export VIDOW_TEST_OUTPUTS="$T_REMOVE:remove:left,$T_ADD:add:1280x720:last"

T0=$(date +%s%3N)
wait_until() { # wait_until <ms since vidow start>
	local now=$(( $(date +%s%3N) - T0 ))
	local left=$(( $1 - now ))
	[ "$left" -gt 0 ] && sleep "$(awk -v ms="$left" 'BEGIN{printf "%.2f", ms/1000}')"
	return 0
}

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
VIDOW_PGID=$VIDOW_PID
for _ in $(seq 1 40); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG"; then
	fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 |
	cut -d= -f2)

# 两块屏谁在左边由日志说了算（不假设 headless 后端先创建哪一块）
read -r LEFT RIGHT LEFT_X LEFT_W LAYOUT_W <<<"$(python3 - "$LOG" <<'PY'
import re, sys
log = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
outs = {}
for m in re.finditer(r"output (HEADLESS-\d): .*逻辑 (\d+)x(\d+)，布局 (-?\d+),(-?\d+)",
                     log):
    outs[m.group(1)] = (int(m.group(4)), int(m.group(2)))
if len(outs) != 2:
    print("")
else:
    (ln, (lx, lw)), (rn, (rx, rw)) = sorted(outs.items(), key=lambda kv: kv[1][0])
    print(ln, rn, lx, lw, max(lx + lw, rx + rw))
PY
)"
if [ -z "${LEFT:-}" ]; then
	fail "日志里没读出两块屏"; exit 1
fi
echo "  左边 $LEFT（原点 x=$LEFT_X 宽 $LEFT_W），右边 $RIGHT，布局宽 $LAYOUT_W"

mark "把窗口放在左边那块屏上（$LEFT）"
# 光标移到左边那块屏中间：新窗口就开在那儿
CX=$(( (LEFT_X + LEFT_W / 2) * 10000 / LAYOUT_W ))
./build/vdrag "$CX" 5000 "$CX" 5000 >/dev/null 2>&1
sleep 0.4
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
# 一个一个来：并发起会让它们的列顺序每次都不一样，位置就没法对比了
for n in p-a p-b p-c; do
	python3 tools/pattern.py "$n" >/dev/null 2>&1 &
	sleep 0.7
done
sleep 0.8

# 把焦点那列的宽度改成 2/3 —— 验「列宽也留着」。
# 必须在起浮动窗口之前按：浮动窗口一 map 焦点就跑到它身上，而它在列外，
# Mod+R 只管「焦点窗口所在的那一列」。
wtype -M logo -k r >/dev/null 2>&1
sleep 0.6

python3 tools/glass.py >/dev/null 2>&1 &
sleep 1.2

grep -aq "place p-" "$LOG" || { fail "三个窗口没摆上"; exit 1; }

wait_until "$T_SHOT1"
shot1=$RUNDIR/s1.png
grim "$shot1"
grep -aE "place p-[abc] -> |place glass -> " "$LOG" > "$RUNDIR/before.txt"

wait_until "$T_SHOT2"
shot2=$RUNDIR/s2.png
grim "$shot2"
sleep 0.5
grep -aE "place p-[abc] -> |place glass -> " "$LOG" > "$RUNDIR/parked.txt"

wait_until "$T_SHOT3"
# 还原会把窗口挪回原来的位置，那是一次 150ms 的移动补间（window_move_ms）—— 拍之前
# 多等一会儿，让补间走完，别拍到窗口"正在飞"的那一帧。机器忙的时候补间会拖得更久，
# 所以拍两张（间隔 0.4 秒），判据里任意一张对得上就算过。
sleep 0.4
shot3=$RUNDIR/s3.png
grim "$shot3"
sleep 0.4
shot3b=$RUNDIR/s3b.png
grim "$shot3b"

pgrep -x vidow >/dev/null && pass "合成器还活着（没崩）" || fail "合成器挂了"

# ---------------------------------------------------------------- 量结果
python3 - "$LOG" "$RUNDIR/before.txt" "$RUNDIR/parked.txt" "$shot1" "$shot2" "$shot3" \
	"$LEFT" "$RIGHT" "$LEFT_X" "$LEFT_W" "$LAYOUT_W" "$shot3b" <<'PY'
import re, sys
from PIL import Image

log = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
before = open(sys.argv[2], "rb").read().decode()
parked = open(sys.argv[3], "rb").read().decode()
s1, s2, s3 = sys.argv[4], sys.argv[5], sys.argv[6]
s3b = sys.argv[12] if len(sys.argv) > 12 else s3
LEFT, RIGHT = sys.argv[7], sys.argv[8]
LEFT_X, LEFT_W, LAYOUT_W = int(sys.argv[9]), int(sys.argv[10]), int(sys.argv[11])
failed = []

def check(ok, what):
    print(("  PASS  " if ok else "  FAIL  ") + what)
    if not ok:
        failed.append(what)

def places(txt):
    out = {}
    for m in re.finditer(r"place (\S+) -> (-?\d+),(-?\d+) (\d+)x(\d+)", txt):
        out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return out

def size(path):
    try:
        return Image.open(path).size
    except OSError:
        return (0, 0)

print()
print("== 断言 ==")

# 1) 拔线 / 插回来的日志
m = re.search(r"hotplug: 拔掉 %s（(\d+) 列 / (\d+) 个窗口 / (\d+) 个浮动窗口）"
              r"，先挪到 (\S+) 上住，归属记着" % re.escape(LEFT), log)
check(m is not None, "拔线：把 %s 上的东西记下来并挪到别的屏上住" % LEFT)
if m:
    check((m.group(1), m.group(2), m.group(3)) == ("3", "3", "1"),
          "拔线时数清了三列 / 三个平铺窗口 / 一个浮动窗口（%s 列 / %s 窗口 / %s 浮动）"
          % m.groups()[:3])
    check(m.group(4) == RIGHT, "挪到右边那块屏 %s 上（优先接左边最近的活屏）" % RIGHT)

check(re.search(r"hotplug: 1 个 layer surface 挪到 %s 上住" % re.escape(RIGHT),
                log) is not None,
      "layer surface（壁纸/状态栏）也跟着挪到接收的那块屏上")

m = re.search(r"hotplug: %s 回来了，还原 (\d+) 列 / (\d+) 个窗口 / (\d+) 个浮动窗口"
              r"（列宽、滚动位置都留着）" % re.escape(LEFT), log)
check(m is not None, "插回来：按记忆还原 %s 上的工作区" % LEFT)
if m:
    check((m.group(1), m.group(2), m.group(3)) == ("3", "3", "1"),
          "还原的数目和拔线时一致（%s 列 / %s 窗口 / %s 浮动）" % m.groups()[:3])

# 2) 画面尺寸：屏真的没 / 真的回来了
check(size(s1) == (LAYOUT_W, 720), "拔线前画面 %s（两块屏拼起来）" % (size(s1),))
check(size(s2) == (LEFT_W, 720), "拔线后画面 %s（只剩一块屏）" % (size(s2),))
check(size(s3) == (LAYOUT_W, 720), "插回来画面 %s（又是两块屏）" % (size(s3),))

# 3) 还原得对不对：顺序、间距、相对位置、平移量
tiles = ["p-a", "p-b", "p-c"]
b, p, a = places(before), places(parked), places(log)

# 画面原点：拔掉一块屏之后 wlroots 会重新摆放剩下的输出（右边那块会挪到 x=0），
# 所以每一段的原点要从日志里那一段的「最后已知位置」取，不能拿一开始的值。
def segment_origins(txt):
    out = {}
    for m in re.finditer(r"output (HEADLESS-\d): \d+x\d+ @ .*布局 (-?\d+),", txt):
        out[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"output (HEADLESS-\d): 布局位置 -?\d+,-?\d+ "
                         r"\d+x\d+ -> (-?\d+),", txt):
        out[m.group(1)] = int(m.group(2))
    return out

cut = log.find("hotplug: 拔掉 %s（" % LEFT)
cut2 = log.find("hotplug: %s 回来了" % LEFT)
seg1 = log[:cut] if cut > 0 else log
seg2 = log[cut:cut2] if cut2 > 0 else log[cut:]
seg3 = log[cut2:] if cut2 > 0 else ""
o1, o2, o3 = segment_origins(seg1), segment_origins(seg2), segment_origins(seg3)

for tag, tbl in (("拔线前", b), ("寄住中", p), ("还原后", a)):
    if tbl:
        print("  %s：%s" % (tag, "  ".join(
            "%s@%d,%d %dx%d" % (k, v[0], v[1], v[2], v[3])
            for k, v in sorted(tbl.items()))))
check(all(t in b for t in tiles) and all(t in a for t in tiles),
      "三个窗口在拔线前、还原后都有摆位记录")

all_outs = [(m.group(1), int(m.group(2))) for m in
            re.finditer(r"output (HEADLESS-\d): .*布局 (-?\d+),", log)]
left_origin = all_outs[0][1]      # 一开始 LEFT 在哪儿
restored_origin = all_outs[-1][1] # 最后加进来那块屏（也就是它回来了）在哪儿
shift = restored_origin - left_origin
print("  屏的布局原点：开始 %d -> 回来 %d（平移 %d）"
      % (left_origin, restored_origin, shift))

if all(t in b for t in tiles) and all(t in a for t in tiles):
    order_b = sorted(tiles, key=lambda t: b[t][0])
    order_a = sorted(tiles, key=lambda t: a[t][0])
    check(order_b == order_a, "列的先后顺序没变（%s）" % " < ".join(order_a))

    gaps_b = [b[order_b[i + 1]][0] - b[order_b[i]][0] - b[order_b[i]][2]
              for i in range(2)]
    gaps_a = [a[order_a[i + 1]][0] - a[order_a[i]][0] - a[order_a[i]][2]
              for i in range(2)]
    check(gaps_b == gaps_a, "列与列之间的间距没变（%s -> %s）" % (gaps_b, gaps_a))
    widths = sorted({b[t][2] for t in tiles})
    check(max(widths) == 845 and min(widths) == 634,
          "Mod+R 改过的那一列宽度还在（宽度 %s：焦点那列 2/3 = 845，其余 634）"
          % widths)

    moved = all(a[t][0] - b[t][0] == shift for t in tiles)
    check(moved, "还原后每个窗口整体平移 %d（= 新原点 - 旧原点）：滚动位置和列偏移"
          "都还原了（%s）" % (shift, [a[t][0] - b[t][0] for t in tiles]))
    check(all(a[t][1] == b[t][1] and a[t][2] == b[t][2] and a[t][3] == b[t][3]
              for t in tiles), "每个窗口的 y / 宽 / 高都没变")

    if "glass" in b and "glass" in a:
        dx = a["glass"][0] - b["glass"][0]
        dw = abs(a["glass"][2] - b["glass"][2])
        dh = abs(a["glass"][3] - b["glass"][3])
        check(dx == shift and abs(a["glass"][1] - b["glass"][1]) <= 2 and
              dw <= 8 and dh <= 8,
              "浮动窗口（不在列里）也按同样的量平移 %d，尺寸只差 %dx%d（GTK 自己"
              "微调，不算变）：%s -> %s" % (shift, dw, dh, b["glass"], a["glass"]))
    else:
        print("  SKIP  浮动窗口没测到")

    # 拔线后：借住期间也保持顺序和间距（相对接收那块屏）
    if all(t in p for t in tiles):
        order_p = sorted(tiles, key=lambda t: p[t][0])
        gaps_p = [p[order_p[i + 1]][0] - p[order_p[i]][0] - p[order_p[i]][2]
                  for i in range(2)]
        check(order_p == order_b and gaps_p == gaps_b,
              "寄住期间（拔线后）三个窗口的顺序和间距照旧（%s）" % " < ".join(order_p))

# 4) 像素：窗口真的画在那块屏上（四象限：左上红 / 右上绿 / 左下蓝 / 右下白）
quad = [((1, 1), (255, 0, 0)), ((3, 1), (0, 255, 0)),
        ((1, 3), (0, 0, 255)), ((3, 3), (255, 255, 255))]

def covers(rect, pt):
    x, y, w, h = rect
    return x <= pt[0] < x + w and y <= pt[1] < y + h

def quad_ok(paths, rect, float_rect=None, label=""):
    """窗口的四象限颜色对不对。落在浮动窗口（半透明模糊）底下的采样点跳过 ——
    那里看到的不是 pattern 的颜色。全都采不到就返回 None。
    paths 可以是多张图（同一个瞬间前后各拍一张）：**任意一张对得上就算过** ——
    动画补间在机器忙的时候会拖，单张容易拍到「窗口正在飞」的那一帧。

    rect 用**布局绝对坐标**：grim 拍的是整个布局（几块屏拼起来那张），place
    日志给的也是布局坐标，所以直接索引。别减屏的原点 —— 减了只有在「那块屏恰好
    在布局 x=0」时才碰巧对（插回来的屏由 wlr_output_layout_add_auto 放在最右，
    原点不是 0，减了就采到隔壁屏上去了）。"""
    if isinstance(paths, str):
        paths = [paths]
    worst = None
    for path in paths:
        im = Image.open(path).convert("RGB")
        x, y, w, h = rect
        if x < 0 or y < 0 or x + w > im.size[0] or y + h > im.size[1]:
            continue  # 列被滚出画面了，正常
        sampled, bad = 0, []
        for (fx, fy), want in quad:
            pt = (x + w * fx // 4, y + h * fy // 4)
            if float_rect is not None and covers(float_rect, pt):
                continue
            got = im.getpixel(pt)
            sampled += 1
            if max(abs(c - wnt) for c, wnt in zip(got, want)) > 24:
                bad.append((pt, got, want))
        if sampled == 0:
            continue
        if not bad:
            return True
        worst = (x, y, w, h, bad)
    if worst is None:
        return None
    x, y, w, h, bad = worst
    for pt, got, want in bad:
        print("        %s：画面 (%d,%d)+%dx%d 采样 %s 得到 %s，预期 %s"
              % (label, x, y, w, h, pt, got, want))
    return False

vis1 = [t for t in tiles if quad_ok(s1, b[t], b.get("glass"), t + " @s1")]
check(len(vis1) >= 1, "拔线前窗口画在这块屏上（四象限颜色对，%d 个在画面里）" % len(vis1))

vis2 = [t for t in tiles if p.get(t) and quad_ok(s2, p[t], p.get("glass"),
                                                  t + " @s2")]
check(len(vis2) >= 1, "拔线后窗口在剩下的那块屏上照常渲染（%d 个在画面里）" % len(vis2))

vis3 = [t for t in tiles if quad_ok([s3, s3b], a[t], a.get("glass"),
                                    t + " @s3")]
check(len(vis3) >= 1, "插回来后窗口又画在那块屏上（%d 个在画面里）" % len(vis3))

# 5) layer surface 跟着走：接收的那块屏左下角不是空的（黑）
def backdrop(path, origin_x, layout_x):
    im = Image.open(path).convert("RGB")
    x = layout_x - origin_x
    if x < 0 or x >= im.size[0]:
        return None
    return im.getpixel((x, im.size[1] - 3))

bg1 = backdrop(s1, o1.get(LEFT, 0), o1.get(LEFT, 0) + 4)
bg2 = backdrop(s2, o2.get(RIGHT, 0), o2.get(RIGHT, 0) + 4)
check(bg1 is not None and sum(bg1) > 12,
      "拔线前左边那块屏底下是壁纸（layer surface 画着），像素 %s" % (bg1,))
check(bg2 is not None and sum(bg2) > 12,
      "拔线后接收的那块屏底下也是壁纸（壁纸真的挪过去了，否则这里是黑的），"
      "像素 %s" % (bg2,))

check("Assertion" not in log and "abort" not in log.lower(),
      "日志里没有 assert / abort")

print()
print("结论：%s" % ("全部通过" if not failed else "%d 项失败" % len(failed)))
sys.exit(1 if failed else 0)
PY
rc=$?
[ $rc -ne 0 ] && FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "test-hotplug.sh: OK"
else
	echo "test-hotplug.sh: 有失败项"
fi
exit $FAILED
