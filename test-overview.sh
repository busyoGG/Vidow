#!/bin/bash
# niri 风格的 overview：Mod+O 把这块屏上的整条列带缩小铺满屏幕看一眼全景，
# 点谁选谁，Esc（或再按一次 Mod+O）缩回去。
#
# 判据（日志 + 像素）：
#   1) 进入：`overview: 进入（… N 个窗口，占 WxH 缩到 S，铺在工作区 …）`，
#      每个窗口一行「窗口 X -> 屏幕 x,y wxh（缩放 S）」，缩放 < 1 且所有矩形都在屏内
#   2) 缩放是对的：overview 里的窗口尺寸 = 原尺寸 × S（±2px），窗口之间间距同样按 S 缩
#   3) 窗口真的画在那些位置：按日志坐标采样四象限颜色（红绿蓝白），而且是**没被压暗**的
#   4) 背景被压暗了（壁纸那块比正常时暗），窗口本身不暗（遮罩夹在壁纸和窗口之间 ——
#      这里踩过一次：遮罩层建了两次跑到窗口上面去了）
#   5) Esc 退出：日志 `overview: 退出`，画面**逐像素**回到进 overview 之前那张
#   6) 点选：点某个窗口 → `overview: 选中 X` + 退出 + 焦点换成它（那一列被滚进视野）
#   7) 两块屏时只看当前那块：另一块屏上的窗口淡出（那片区域不是 pattern 的颜色）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-overview-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
VIDOW_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	stop_clients
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	rm -rf "$RUNDIR"
}
trap cleanup EXIT

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*wall.py*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

command -v grim >/dev/null || { echo "test-overview.sh: 需要 grim"; exit 1; }
command -v wtype >/dev/null || { echo "test-overview.sh: 需要 wtype"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-overview.sh: 需要 python3-gobject"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config" <<'EOF'
scale = 1
xwayland = 0
window_open_ms = 150
window_close_ms = 150
window_move_ms = 150
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

start_vidow() { # start_vidow <outputs>
	export WLR_HEADLESS_OUTPUTS=$1
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	VIDOW_PIDS="$VIDOW_PIDS $VIDOW_PGID"
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

# 光标移到某块屏中间（归一化 0..10000，按整个布局宽算）
point_at() { # point_at <layout_x> <layout_w> <layout_h>
	local nx=$(( $1 * 10000 / $2 ))
	./build/vdrag "$nx" 5000 "$nx" 5000 >/dev/null 2>&1
	sleep 0.3
}

# ---------------------------------------------------------------- 单屏
mark "单屏：3 个窗口 + 壁纸"
start_vidow 1
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
for n in p-a p-b p-c; do
	python3 tools/pattern.py "$n" >/dev/null 2>&1 &
	sleep 0.7
done
sleep 1.2
grep -aq "place p-" "$LOG" || { fail "三个窗口没摆上"; exit 1; }

grim "$RUNDIR/normal.png"
SNAP=$RUNDIR/normal.txt
grep -aE "place p-[abc] -> " "$LOG" > "$SNAP"

wtype -M logo -k o >/dev/null 2>&1
sleep 0.9
grim "$RUNDIR/overview.png"

wtype -k Escape >/dev/null 2>&1
sleep 0.9
grim "$RUNDIR/back.png"

# 再进一次，点最左边那个窗口
wtype -M logo -k o >/dev/null 2>&1
sleep 0.9
read -r CX CY CW CH <<<"$(grep -a "overview: 窗口" "$LOG" | tail -3 |
	sed 's/.*屏幕 //; s/（.*//; s/,/ /; s/x/ /' | sort -k1 -n | head -1)"
echo "  点选目标：$CX,$CY ${CW}x${CH}"
NX=$(( (CX + CW / 2) * 10000 / 1280 ))
NY=$(( (CY + CH / 2) * 10000 / 720 ))
./build/vdrag "$NX" "$NY" "$NX" "$NY" >/dev/null 2>&1
sleep 1.2
grim "$RUNDIR/picked.png"
grep -aE "place p-[abc] -> " "$LOG" > "$RUNDIR/picked.txt"
grep -aE "place p-[abc] -> " "$LOG" > "$RUNDIR/picked.txt"

cp "$LOG" "$RUNDIR/log-single.txt"
stop_clients
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6

# ---------------------------------------------------------------- 双屏
mark "双屏：只缩当前那块屏，另一块的窗口淡出"
start_vidow 2
# 光标放左边那块屏 → 窗口开在那儿；然后把光标挪到右边，再开一个
point_at 640 2560 720
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
python3 tools/pattern.py left-1 >/dev/null 2>&1 &
sleep 1.0
point_at 1920 2560 720
sleep 0.3
python3 tools/pattern.py right-1 >/dev/null 2>&1 &
sleep 1.2
grim "$RUNDIR/two-normal.png"
point_at 640 2560 720
wtype -M logo -k o >/dev/null 2>&1
sleep 0.9
grim "$RUNDIR/two-overview.png"
cp "$LOG" "$RUNDIR/log-two.txt"
stop_clients
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6

leftover=0
for pid in $VIDOW_PIDS; do
	kill -0 "$pid" 2>/dev/null && leftover=$((leftover + 1))
done
[ "$leftover" -eq 0 ] && pass "测试起的合成器都收干净了" ||
	fail "还有 $leftover 个测试起的 vidow 没退"

# ---------------------------------------------------------------- 量结果
python3 - "$LOG" "$SNAP" "$RUNDIR" <<'PY'
import re, sys
from PIL import Image

rundir = sys.argv[3]
def image(name):
    return Image.open("%s/%s.png" % (rundir, name)).convert("RGB")

def places(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"place (\S+) -> (-?\d+),(-?\d+) (\d+)x(\d+)", line)
        if m:
            out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return out

# 单屏那段日志（双屏那段会覆盖 LOG，所以最后读到的只有双屏 —— 单屏的留在文件里）
single = open("%s/log-single.txt" % rundir, encoding="utf-8",
              errors="replace").read()
two = open("%s/log-two.txt" % rundir, encoding="utf-8",
           errors="replace").read()
failed = []

def check(ok, what):
    print(("  PASS  " if ok else "  FAIL  ") + what)
    if not ok:
        failed.append(what)

quad = [((1, 1), (255, 0, 0)), ((3, 1), (0, 255, 0)),
        ((1, 3), (0, 0, 255)), ((3, 3), (255, 255, 255))]

def quad_ok(im, rect):
    x, y, w, h = rect
    if x < 0 or y < 0 or x + w > im.size[0] or y + h > im.size[1]:
        return None
    for (fx, fy), want in quad:
        got = im.getpixel((x + w * fx // 4, y + h * fy // 4))
        if max(abs(c - w2) for c, w2 in zip(got, want)) > 24:
            return False
    return True

print()
print("== 单屏 ==")
m = re.search(r"overview: 进入（(\S+) 上 (\d+) 个窗口，占 (\d+)x(\d+) 缩到 ([\d.]+)，"
              r"铺在工作区 (\d+)x(\d+) 里）", single)
check(m is not None, "Mod+O 进入 overview，日志报出缩放和铺法")
scale = 0.0
if m:
    n, sw, sh, scale, ww, wh = (m.group(2), int(m.group(3)), int(m.group(4)),
                                float(m.group(5)), int(m.group(6)), int(m.group(7)))
    check(int(n) >= 3, "数到了 %s 个窗口" % n)
    check(0.0 < scale < 1.0, "缩放 %s < 1（是缩小不是放大）" % scale)
    check(sw * scale <= ww + 1 and sh * scale <= wh + 1,
          "整条列带 %dx%d 缩成 %.0fx%.0f 装进了工作区 %dx%d"
          % (sw, sh, sw * scale, sh * scale, ww, wh))

rects = {}
for mm in re.finditer(r"overview: 窗口 (\S+) -> 屏幕 (-?\d+),(-?\d+) (\d+)x(\d+)"
                      r"（缩放 ([\d.]+)）", single):
    rects[mm.group(1)] = tuple(int(v) for v in mm.groups()[1:5])
check(len(rects) >= 3, "每个窗口都报了它在 overview 里的位置（%d 个）" % len(rects))
check(all(0 <= r[0] and r[0] + r[2] <= 1280 and 0 <= r[1] and r[1] + r[3] <= 720
          for r in rects.values()),
      "所有窗口都落在屏幕里（不会缩到一半被切掉）")

before = places("%s/normal.txt" % rundir)
check(all(t in before for t in rects), "拔线前的位置都记下来了")

if before and rects:
    ok_size = all(abs(rects[t][2] - before[t][2] * scale) <= 2 and
                  abs(rects[t][3] - before[t][3] * scale) <= 2
                  for t in rects if t in before)
    check(ok_size, "overview 里的尺寸 = 原尺寸 × %s（±2px）" % scale)

    order_b = sorted(before, key=lambda t: before[t][0])
    order_o = sorted(rects, key=lambda t: rects[t][0])
    check(order_b == order_o, "窗口的先后顺序没变（%s）" % " < ".join(order_o))
    if len(order_b) >= 2:
        gap_b = before[order_b[1]][0] - before[order_b[0]][0]
        gap_o = rects[order_o[1]][0] - rects[order_o[0]][0]
        check(abs(gap_o - gap_b * scale) <= 3,
              "窗口之间的间距也跟着缩（%d -> %d ≈ %d × %s）"
              % (gap_b, gap_o, gap_b, scale))

im_ov = image("overview")
ok_px = [t for t in rects if quad_ok(im_ov, rects[t])]
check(len(ok_px) >= 3,
      "overview 里窗口的四象限颜色正确（%d 个按日志坐标采样通过）" % len(ok_px))
check(all(im_ov.getpixel((rects[t][0] + rects[t][2] // 4,
                          rects[t][1] + rects[t][3] // 4)) == (255, 0, 0)
          for t in rects if quad_ok(im_ov, rects[t])),
      "窗口没有被压暗（遮罩夹在壁纸和窗口之间，不是盖在窗口上）")

im_norm = image("normal")
bg_norm = im_norm.getpixel((4, 716))
bg_ov = im_ov.getpixel((4, 716))
check(sum(bg_ov) < sum(bg_norm),
      "壁纸被压暗了（%s -> %s）" % (bg_norm, bg_ov))

# Esc 退出：画面逐像素回到进 overview 之前
im_back = image("back")
diff = sum(1 for y in range(720) for x in range(1280)
           if im_back.getpixel((x, y)) != im_norm.getpixel((x, y)))
# 缩下去再回来，内容的场景尺寸要按比例算两遍（round），边缘允许有个位数的抗锯齿差
check(diff <= 200, "Esc 退出后画面回到原样（%d 个像素不同，只允许取整带来的零星差异）"
      % diff)

# 点选
m = re.search(r"overview: 选中 (\S+)", single)
check(m is not None, "点某个窗口 = 选中它")
check(re.search(r"overview: 选中 \S+\n.*overview: 退出", single, re.S) is not None or
      single.count("overview: 退出") >= 2,
      "选中之后就缩回去了（不用再按 Esc）")

picked = places("%s/picked.txt" % rundir)
if m and picked:
    t = m.group(1)
    r = picked.get(t)
    check(r is not None, "选中的窗口 %s 有新摆位（焦点真换过去了）" % t)
    if r:
        check(r[0] >= 0 and r[0] + r[2] <= 1280,
              "选中的那一列被滚进画面（x=%d 宽 %d）" % (r[0], r[2]))
        others = [v for k, v in picked.items() if k != t]
        check(any(v[0] < 0 or v[0] + v[2] > 1280 for v in others),
              "没选的列滚出画面了（还是那条带，没有重新排）")

print()
print("== 双屏 ==")
m = re.search(r"overview: 进入（(\S+) 上 (\d+) 个窗口", two)
check(m is not None, "进入 overview（只看当前那块屏 %s）" % (m.group(1) if m else "?"))
m2 = re.search(r"overview: 窗口 (\S+) -> 屏幕 (-?\d+),(-?\d+) (\d+)x(\d+)", two)
check(m2 is not None and m2.group(1).startswith("left"),
      "只报了当前那块屏上的窗口：%s" % (m2.group(1) if m2 else "无"))
check(re.search(r"overview: 窗口 right-1 -> ", two) is None,
      "另一块屏上的窗口不进 overview 的清单")

im_two = image("two-overview")
im_two_norm = image("two-normal")
# 另一块屏的窗口在 normal 里是 pattern 的颜色，overview 里应该被淡掉（只剩壁纸/遮罩）
def find_pattern(im, x0, x1):
    for y in range(120, 700, 7):
        for x in range(x0, x1, 7):
            if im.getpixel((x, y)) in ((255, 0, 0), (0, 255, 0), (0, 0, 255),
                                       (255, 255, 255)):
                return (x, y)
    return None

right_before = find_pattern(im_two_norm, 1300, 2560)
right_after = find_pattern(im_two, 1300, 2560)
check(right_before is not None, "另一块屏上本来有窗口（%s）" % (right_before,))
check(right_after is None, "overview 里另一块屏上的窗口淡出了（现在只剩壁纸）")

check("Assertion" not in single and "abort" not in single.lower() and
      "Assertion" not in two and "abort" not in two.lower(),
      "日志里没有 assert / abort")

print()
print("结论：%s" % ("全部通过" if not failed else "%d 项失败" % len(failed)))
sys.exit(1 if failed else 0)
PY
rc=$?
[ $rc -ne 0 ] && FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "test-overview.sh: OK"
else
	echo "test-overview.sh: 有失败项"
fi
exit $FAILED
