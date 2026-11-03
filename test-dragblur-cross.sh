#!/bin/bash
# 跨屏拖拽：窗口压在**两块屏的边界**上时，两边各自的私有底衬都得采到对面。
#
# 用户 2026-11-03：「拖拽跨屏底衬还是不行」。靶子是 view_region_rect()：它原来
# 按「这块屏上看得见的那一块」（place 的 hit）算区域，窗口骑在两块屏之间时
# 每块屏只烘自己那一半。链条每一趟都把边界外那圈**透明**像素一起平均，于是
# 窗口靠屏边界那一条变成「自己 + 透明」的混合，跨过去的部分看起来根本没采到
# 底衬。另外「和这块屏完全没有交集」时以前整块跳过 —— 窗口从这块屏上滚出去
# 的那一瞬间，那一半的底衬就停在上一帧上。
#
# 场景和用户真机一样：左屏 1 倍 1920x1080，右屏 2 倍（物理 3840x2160，
# 逻辑 1920x1080），边界在逻辑 x = W。
#   wB：不透明纯蓝，摆在右屏上、左半截越界到左屏（x = W-220）。
#   wA：半透明白，从左边拖到边界上 —— 左半截还要压着 wB，右半截跨到右屏。
#
# 判据：
#   1) 前提：拖动期间 wA 在两块屏上都重抓过私有底衬
#   2) 果冻**每块屏各画一格**：左屏那格用左屏的倍率、右屏那格用右屏的倍率
#      （边框每格各采自己那块屏的装饰缓冲和底衬，见 jelly.c 的 struct
#      jelly_slot）。整窗只画一份的话另一半必然是错倍率 —— 横纹 / 闪烁。
#   3) 左屏：wA 压着 wB 的那一条看得到蓝
#   4) 右屏：wA 越过边界那一截不发黑
#   5) vidow 全程活着
#
# 用法：./test-dragblur-cross.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试并行跑 —— 收尾按 cmdline 杀 sheet.py / wall.py。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-dragblur-cross-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

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
xwayland = false
-- 用户真机：一块 1 倍、一块 2 倍（2 倍那块物理 3840x2160，逻辑还是 1920x1080）
-- 和用户真机同一个摆法：左屏 1 倍在 x=0，右屏 2 倍在 x=1920（HEADLESS-1 排在
-- 后面，布局里落在 1920）。倍率挂在**非原点**那块屏上，坐标空间写错时最先炸的
-- 就是它。
vidow.output{ name = "HEADLESS-1", scale = 2, mode = "3840x2160" }
vidow.output{ name = "HEADLESS-2", scale = 1, mode = "1920x1080" }
blur_enabled = true
blur_all = true
blur_levels = 4
blur_offset = 1.5
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
jelly = true
scale = "1"
vidow.bind("Mod+mouse-left", "interactive-move")
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug VIDOW_BLUR_TRACE=1
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
# 果冻的每帧探针（jelly v=... scale=）只在 x11 trace 开关打开时打
export VIDOW_X11_TRACE="$RUNDIR/xtrace"
export VIDOW_GTRACE="$RUNDIR/gtrace.log"
mkfifo "$RUNDIR/actions"

BIN=${VIDOW_BIN:-./build/vidow}
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

# 读写方式打开 FIFO：只写的方式在 vidow 还没 open 读端时会阻塞在这儿。
exec 9<> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }

geom_of() { # <标题> -> "x y w h floating"（全局逻辑坐标）
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

to_output() { # <标题> <输出> <x> <y>
	local t="$1" out="$2" x="$3" y="$4"
	for _ in $(seq 1 30); do
		./build/vidowmsg windows 2>/dev/null | grep -q "\"$t\"" && break
		sleep 0.3
	done
	./build/vidowmsg move-window-to-output "$out" --title "$t" >/dev/null 2>&1
	sleep 0.6
	float_at "$t" "$x" "$y"
}

mark "场景：左 1 倍 / 右 2 倍，wB 横跨边界，wA 从左边拖到边界上压住 wB"

./build/vidowmsg outputs | python3 -c '
import sys, json
outs = sorted(json.load(sys.stdin), key=lambda o: o["layout"]["x"])
a, b = outs[0], outs[1]
print("L %s %d %d %s" % (a["name"], a["layout"]["x"],
      a["logical_width"], a["scale"]))
print("R %s %d %d %s" % (b["name"], b["layout"]["x"],
      b["logical_width"], b["scale"]))
' > "$RUNDIR/layout.txt" || { echo "读不到两块屏的布局"; exit 1; }
LNAME=$(awk '$1 == "L" {print $2}' "$RUNDIR/layout.txt")
RX=$(awk '$1 == "L" {print $3}' "$RUNDIR/layout.txt")
RW=$(awk '$1 == "L" {print $4}' "$RUNDIR/layout.txt")
LSCALE=$(awk '$1 == "L" {print $5}' "$RUNDIR/layout.txt")
RNAME=$(awk '$1 == "R" {print $2}' "$RUNDIR/layout.txt")
RRX=$(awk '$1 == "R" {print $3}' "$RUNDIR/layout.txt")
RRW=$(awk '$1 == "R" {print $4}' "$RUNDIR/layout.txt")
RSCALE=$(awk '$1 == "R" {print $5}' "$RUNDIR/layout.txt")
W=$((RX + RW))   # 边界（全局逻辑 x）
echo "    左 $LNAME @$RX 宽 $RW 倍率 $LSCALE"
echo "    右 $RNAME @$RRX 宽 $RRW 倍率 $RSCALE"
echo "    边界 = 逻辑 x $W"
[ "$W" -eq "$RRX" ] || echo "    （两块屏不是紧挨着：按左屏右缘算边界）"

# B：不透明纯蓝，放在右屏上，左半截越界到左屏
setsid nohup python3 ./tools/sheet.py wB 0 0 1 1 > "$RUNDIR/b.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.6
to_output wB "$RNAME" $((W - 220)) 300
echo "    wB（蓝）：$(geom_of wB)"

# A：半透明白，先摆在左屏
setsid nohup python3 ./tools/sheet.py wA 1 1 1 0.5 > "$RUNDIR/a.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.8
float_at wA $((RX + 600)) 300
echo "    wA（白）：$(geom_of wA)"

AX=$(geom_of wA | awk '{print $1}')
AY=$(geom_of wA | awk '{print $2}')
ACT_BEFORE=$(wc -c < "$LOG")
GTRACE_BEFORE=$(wc -c < "$RUNDIR/gtrace.log" 2>/dev/null || echo 0)

# 按住 wA 中心往右拖，停在「盒子左 = 边界 - 285」附近：wB 占
# [边界-220, 边界+192)，wA 占 [边界-285, 边界+127) —— 左半截压着 wB，右半截
# 跨到右屏。再往右整个盖住 wB 就看不到「压着的那一条」了。
TX=$((W - 285 + 200))
act "pointer-motion $((AX + 200)) $((AY + 150))"
act "pointer-button left press mod"
for i in $(seq 0 9); do
	px=$((AX + 200 + (TX - (AX + 200)) * i / 9))
	py=$((AY + 150 + (i % 2 == 0 ? 10 : -10)))
	echo "pointer-motion $px $py" >&9
	sleep 0.06
	grim -o "$LNAME" "$RUNDIR/L-$i.png" 2>/dev/null
	grim -o "$RNAME" "$RUNDIR/R-$i.png" 2>/dev/null
done
sleep 0.4
./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    g = w["float_geometry"] if w["floating"] else w["geometry"]
    print("    %s %d %d %d %d" % (w["title"], g["x"], g["y"],
          g["width"], g["height"]))'
# 落点用真实几何，不靠推算（抓点偏移、抖动都会影响它）
FBX=$(geom_of wA | awk '{print $1}')
FBY=$(geom_of wA | awk '{print $2}')
FBW=$(geom_of wA | awk '{print $3}')
FBH=$(geom_of wA | awk '{print $4}')
echo "    wA 落点 = $FBX,$FBY ${FBW}x${FBH}"

grim -o "$LNAME" "$RUNDIR/L-during.png" 2>/dev/null
grim -o "$RNAME" "$RUNDIR/R-during.png" 2>/dev/null
act "pointer-button left release mod" 0.9
ACT_AFTER=$(wc -c < "$LOG")

mark "日志（拖动期间 wA 在两块屏上的私有底衬）"
grep -a "blur: own region wA" "$LOG" | tail -4 | sed 's/^/    /'

mark "断言"
python3 - "$RUNDIR" "$W" "$RX" "$LSCALE" "$RRX" "$RSCALE" \
	"$FBX" "$FBY" "$FBW" "$FBH" "$ACT_BEFORE" "$ACT_AFTER" \
	"${GTRACE_BEFORE:-0}" "$LNAME" "$RNAME" "$RRW" <<'PY'
import os, re, sys
from PIL import Image

(rundir, w, lx, lscale, rx, rscale, bx, by, bw, bh,
 before, after, gt_before, lname, rname, rw) = (
                   sys.argv[1], int(sys.argv[2]), int(sys.argv[3]),
                   float(sys.argv[4]), int(sys.argv[5]), float(sys.argv[6]),
                   int(sys.argv[7]), int(sys.argv[8]), int(sys.argv[9]),
                   int(sys.argv[10]), int(sys.argv[11]), int(sys.argv[12]),
                   int(sys.argv[13]), sys.argv[14], sys.argv[15],
                   int(sys.argv[16]))
failed = []

def check(cond, msg):
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        failed.append(msg)

def shots(prefix):
    out = []
    for i in range(10):
        p = os.path.join(rundir, "%s-%d.png" % (prefix, i))
        if os.path.exists(p):
            out.append(p)
    p = os.path.join(rundir, "%s-during.png" % prefix)
    if os.path.exists(p):
        out.append(p)
    return out

def stats(path, gbox, ox, scale):
    """gbox 是全局逻辑坐标；ox / scale 是这块屏的布局原点与倍率。"""
    im = Image.open(path).convert("RGB")
    x1, y1, x2, y2 = gbox
    x1 = int(round((x1 - ox) * scale)); x2 = int(round((x2 - ox) * scale))
    y1 = int(round(y1 * scale)); y2 = int(round(y2 * scale))
    x1 = max(0, x1); y1 = max(0, y1)
    x2 = min(x2, im.size[0]); y2 = min(y2, im.size[1])
    if x2 - x1 < 8 or y2 - y1 < 8:
        return None
    reg = im.crop((x1, y1, x2, y2))
    px = reg.load()
    n = blue = dark = 0
    for y in range(reg.size[1]):
        for x in range(reg.size[0]):
            r, g, b = px[x, y]
            n += 1
            if b > r + 25 and b > g + 25:
                blue += 1
            if max(r, g, b) < 32:
                dark += 1
    return (blue / n, dark / n)

# 1) 前提：拖动期间两块屏都重抓过
log = open(os.path.join(rundir, "log"), "rb").read()[before:after]
log = log.decode("utf-8", "replace")
# 果冻每帧探针走的是另一份文件（vidow_gtrace）。
try:
    log += open(os.path.join(rundir, "gtrace.log"), "rb").read()[gt_before:].decode(
        "utf-8", "replace")
except OSError:
    pass
own = log.count("blur: own region wA")
check(own >= 2, "前提：拖动期间 wA 的私有底衬重抓过（%d 次）" % own)

# 0) 果冻**每块屏各一格**：每格必须用自己那块屏的倍率（源和底衬都是那块屏
#    自己的）。以前整窗只画一份，挑到哪块屏的源另一半就错了 —— 1x 铺到 2x
#    屏上是横纹，两套源轮流用是闪烁。
per_out = {}
for mm in re.finditer(r"jelly v=\d+ out=(\S+) .*?scale=([\d.]+)", log):
    per_out.setdefault(mm.group(1), set()).add("%.2f" % float(mm.group(2)))
check(per_out.get(lname) == {"%.2f" % lscale} and
      per_out.get(rname) == {"%.2f" % rscale},
      "跨屏拖拽期间两块屏各画一格、各用自己的倍率（%s=%s，%s=%s）"
      % (lname, ",".join(sorted(per_out.get(lname) or ["无"])),
         rname, ",".join(sorted(per_out.get(rname) or ["无"]))))

# 0b) 每格的显示节点必须落在**它自己那块屏**里、而且是开着的。坐标空间搞错
#     （节点的局部框直接跟屏的全局框求交）时：起点不在 (0,0) 的那块屏永远求
#     不出交集 —— 那一格被整个关掉；起点在 (0,0) 的那块屏求出的是假交集（位置
#     被夹到 0、目标尺寸压短），画出来横跨到隔壁屏、还是错的倍率。拖起来就是
#     横纹 / 闪烁。
boxes = {lname: (lx, w - lx), rname: (rx, rw)}
total = stray = off = 0
for mm in re.finditer(
        r"jelly v=\d+ out=(\S+) .*?node=(-?\d+),(-?\d+).*?en=(\d+)", log):
    name, nx, en = mm.group(1), int(mm.group(2)), int(mm.group(4))
    if name not in boxes:
        continue
    total += 1
    if en != 1:
        off += 1
        continue
    ox, ow = boxes[name]
    if not (ox - 2 <= nx <= ox + ow):
        stray += 1
check(total > 0 and stray == 0 and off == 0,
      "每格的显示节点都在自己那块屏里、且是开着的"
      "（%d 帧，越界 %d，关着 %d）" % (total, stray, off))

# 2) 左屏：wA ∩ wB ∩ 左屏 的那一条，左右各留 20 避开圆角 / 阴影
gbox = (max(w - 200, bx + 20), by + 60,
        min(w - 20, bx + bw - 20), by + bh - 40)
if gbox[2] - gbox[0] < 20:
    check(False, "前提：wA 没压在左屏的 wB 上（wA x=%d..%d，wB 到 %d）"
          % (bx, bx + bw, w - 220))
else:
    ls = shots("L")
    worst, name, seen = 1.0, "", 0
    for p in ls:
        r = stats(p, gbox, lx, lscale)
        if r is None:
            continue
        seen += 1
        if r[0] < worst:
            worst, name = r[0], os.path.basename(p)
    check(seen > 0 and worst > 0.30,
          "左屏：wA 压着 wB 的那一条看得到蓝（最差 %s：%.0f%% 偏蓝，%d 帧）"
          % (name or "-", worst * 100, seen))

# 3) 右屏：wA 越过边界那一截，紧贴边界往右 100，不许发黑
gbox = (max(w + 10, bx + 20), by + 60,
        min(w + 100, bx + bw - 20), by + bh - 40)
if gbox[2] - gbox[0] < 20:
    check(False, "前提：wA 没有跨到右屏（wA 右缘 %d，边界 %d）"
          % (bx + bw, w))
else:
    rs = shots("R")
    worst, name, seen = 0.0, "", 0
    for p in rs:
        r = stats(p, gbox, rx, rscale)
        if r is None:
            continue
        seen += 1
        if r[1] > worst:
            worst, name = r[1], os.path.basename(p)
    check(seen > 0 and worst < 0.15,
          "右屏：wA 越过边界那一截不发黑（最差 %s：%.0f%% 近黑，%d 帧）"
          % (name or "-", worst * 100, seen))

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
