#!/bin/bash
# 录一次「跨屏拖拽」：把一块浮窗从 DP-2（2 倍）拖到 DP-3（1 倍），全程录屏。
# 输出 /tmp/drag-demo2/drag.mp4 + drag.gif
set -u
cd "$(dirname "$0")" || exit 1

export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
S=$XDG_RUNTIME_DIR/vidow-wayland-0.sock
OUT=/tmp/drag-demo2
rm -rf "$OUT"; mkdir -p "$OUT"

M() { ./build/vidowmsg --socket "$S" "$@" 2>/dev/null; }

WID=${1:?用法: record-drag-cross2.sh <窗口 id>}

read -r WX WY WW WH < <(M windows | python3 -c "
import json,sys
for w in json.load(sys.stdin):
    if w.get('id')==$WID:
        g=w['geometry']; print(g['x'], g['y'], g['width'], g['height'])
")
echo "窗口 $WID geom=${WX},${WY} ${WW}x${WH}"

CX=$((WX + WW/2))
CY=$((WY + WH/2))

gpu-screen-recorder -w 3840x1080+0+0 -f 30 -o "$OUT/drag.mp4" >"$OUT/rec.log" 2>&1 &
RPID=$!
sleep 2

M move-cursor $CX $CY >/dev/null; sleep 0.5
M action "pointer-button left press mod" >/dev/null; sleep 0.6

X=$CX
STEPS=0
# 目标：走到 DP-3 中间（约 960），每步 40 像素、120ms
while [ $X -gt 1450 ] && [ $STEPS -lt 90 ]; do
	X=$((X - 40))
	M move-cursor $X $CY >/dev/null
	sleep 0.12
	STEPS=$((STEPS+1))
done
echo "拖到 x=$X（$STEPS 步）"
sleep 1.0
M action "pointer-button left release mod" >/dev/null
sleep 1.5

kill -TERM $RPID 2>/dev/null
wait $RPID 2>/dev/null
sleep 0.5

M windows | python3 -c "
import json,sys
for w in json.load(sys.stdin):
    if w.get('id')==$WID:
        g=w['geometry']
        print('松手后: out=%s %sx%s@%s,%s vis=%s' % (w.get('output'),g['width'],g['height'],g['x'],g['y'],w.get('visible')))
"
ffmpeg -loglevel error -y -i "$OUT/drag.mp4" -vf "fps=10,scale=1400:-1:flags=lanczos" "$OUT/f%03d.png"
ls -lh "$OUT/drag.mp4" | awk '{print $5, $9}'
