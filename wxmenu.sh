#!/bin/bash
# ./wxmenu.sh —— 在**你自己那个 vidow 会话里**右键微信主窗口，把「弹窗到底有没有建出来」
# 这件事一次问清楚。全部走 IPC 注入（和真鼠标同一条路），你不用手点。
#
#   ./wxmenu.sh            右键微信主窗口正中
#   ./wxmenu.sh <标题子串>  换个窗口（默认「微信」）
#
# 产出的东西留在 /tmp/vidow-wxmenu/：xwininfo 的 X 窗口树前后对比、两张截图、日志片段。
# 跑完跟我说一声，我自己去读。
set -u
cd "$(dirname "$0")"

R=/tmp/vidow-wxmenu
TITLE=${1:-微信}
rm -rf "$R"; mkdir -p "$R"
V=./build/vidowmsg

if ! $V version >/dev/null 2>&1; then
	echo "wxmenu: 连不上 vidow（要在跑着 vidow 的那个会话里跑，或者 --socket 指过去）"
	exit 1
fi

read -r WX WY WW WH WOUT <<< "$($V windows | python3 -c '
import sys, json
want = sys.argv[1]
best = None
for w in json.load(sys.stdin):
    t = w.get("title") or ""
    g = (w.get("float_geometry") or w.get("geometry")) or {}
    if want in t and g.get("width", 0) > 200:
        if best is None or g["width"] * g["height"] > best[2] * best[3]:
            best = (g["x"], g["y"], g["width"], g["height"], w.get("output") or "?")
print(" ".join(str(x) for x in best) if best else "0 0 0 0 ?")' "$TITLE")"

if [ "$WW" = "0" ]; then
	echo "wxmenu: 没找到标题里带「$TITLE」的窗口。现在开着的是："
	$V windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    g = (w.get("float_geometry") or w.get("geometry")) or {}
    if g.get("width", 0) > 0:
        print("  %s（%sx%s）" % (w.get("title") or "?", g.get("width"), g.get("height")))'
	exit 1
fi

OUT=${WOUT:-$($V outputs | python3 -c '
import sys, json
o = json.load(sys.stdin)
print(o[0]["name"] if o else "HEADLESS-1")')}
XP_DISPLAY=$($V xwayland | python3 -c '
import sys, json
print(json.load(sys.stdin).get("display") or ":0")' 2>/dev/null || echo ":0")

echo "目标窗口：$TITLE 在 $OUT 上，$WX,$WY ${WW}x${WH}（X 侧 DISPLAY=$XP_DISPLAY）"
echo "右键点它正中（$((WX + WW / 2)),$((WY + WH / 2))）"

xwininfo -root -tree > "$R/tree-before.txt" 2>&1 || echo "(xwininfo 打不开 X，先只比截图)"
grim -o "$OUT" "$R/before.png" 2>/dev/null

$V move-cursor $((WX + WW / 2)) $((WY + WH / 2)) >/dev/null
sleep 0.6
$V pointer-button press right >/dev/null
sleep 0.2
$V pointer-button release right >/dev/null
sleep 1.5

xwininfo -root -tree > "$R/tree-after.txt" 2>&1
grim -o "$OUT" "$R/after.png" 2>/dev/null

echo
echo "== X 窗口树差异（新建/尺寸变了的就是弹窗）=="
if diff <(sed 's/^ *//' "$R/tree-before.txt") <(sed 's/^ *//' "$R/tree-after.txt") > "$R/tree-diff.txt"; then
	echo "  一个字都没变 —— 客户端没建窗口"
else
	grep '^[<>]' "$R/tree-diff.txt" | head -12
fi

echo
echo "== 画面差异（弹窗画出来的话这里会有一块）=="
python3 - "$R" <<'PY'
import sys
import numpy as np
from PIL import Image
r = sys.argv[1]
try:
	a = np.asarray(Image.open(r + '/before.png').convert('RGB')).astype(int)
	b = np.asarray(Image.open(r + '/after.png').convert('RGB')).astype(int)
except Exception as e:
	print("  截图读不了：", e); raise SystemExit
d = np.abs(a - b).max(axis=2) > 8
ys, xs = np.nonzero(d)
if not d.sum():
	print("  0 个像素变化")
else:
	print("  变了 %d 个像素，范围 x %d..%d  y %d..%d"
		% (d.sum(), xs.min(), xs.max(), ys.min(), ys.max()))
PY

echo
echo "== vidow 那边说了什么（最近 6 条）=="
LOG=/tmp/vidow-dbg.log
if [ -f "$LOG" ]; then
	grep -a "x11: 新窗口\|点击\|popup" "$LOG" | tail -6 | cut -c1-140
else
	echo "  （没有 $LOG；这次没在 dbg.sh 里跑，想要日志就 ./dbg.sh 再跑一遍这个脚本）"
fi

echo
echo "产物在 $R/（tree-before/after、before/after.png）"
