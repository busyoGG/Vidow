#!/bin/bash
# 录屏复现「高分辨率(DP-2) 拖到 低分辨率(DP-3)」：一边录一边用 IPC 注入拖拽。
# 输出 /tmp/drag-demo/drag.mp4 + 逐帧 PNG（给分析用）。
set -u
cd "$(dirname "$0")" || exit 1

export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
S=$XDG_RUNTIME_DIR/vidow-wayland-0.sock
OUT=/tmp/drag-demo
rm -rf "$OUT"; mkdir -p "$OUT"

M() { ./build/vidowmsg --socket "$S" "$@" 2>/dev/null; }

pick() {
	M pick-window "$1" "$2" | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin); w=d.get('window') or {}
    print(w.get('id') if w else 'none')
except: print('none')
"
}


# 先确保 DP-2 上有一个大窗口：把最左边的窗口拖过去
if ! M windows | python3 -c "
import json,sys
d=json.load(sys.stdin); ws=d if isinstance(d,list) else d.get('windows',[])
sys.exit(0 if any(w.get('output')=='DP-2' and (w.get('geometry') or {}).get('width',0)>800 for w in ws) else 1)
" 2>/dev/null; then
	echo "DP-2 空着，先搬一个窗口过去"
	read -r SID SX SY SW SH < <(M windows | python3 -c "
import json,sys
d=json.load(sys.stdin); ws=d if isinstance(d,list) else d.get('windows',[])
for w in ws:
    g=w.get('geometry') or {}
    if w.get('output')=='DP-3' and g.get('width',0)>800 and g.get('x',0)>=0:
        print(w['id'], g['x'], g['y'], g['width'], g['height']); break
")
	if [ -n "${SID:-}" ]; then
		SCX=$((SX + SW/2)); SCY=$((SY + SH/2))
		M move-cursor $SCX $SCY >/dev/null; sleep 0.4
		M send-key press super >/dev/null
		M pointer-button press left >/dev/null; sleep 0.5
		M move-cursor 2800 $SCY >/dev/null; sleep 0.6
		M pointer-button release left >/dev/null
		M send-key release super >/dev/null
		sleep 1.2
	fi
fi

# 选一块在 DP-2 上的大窗口（分辨率最高那块屏）
read -r WID WX WY WW WH < <(M windows | python3 -c "
import json,sys
d=json.load(sys.stdin); ws=d if isinstance(d,list) else d.get('windows',[])
best=None
for w in ws:
    g=w.get('geometry') or {}
    if w.get('output')!='DP-2': continue
    if g.get('width',0) < 800: continue
    if best is None or g['width']*g['height'] > best[0]:
        best=(g['width']*g['height'], w['id'], g['x'], g['y'], g['width'], g['height'])
if best: print(best[1], best[2], best[3], best[4], best[5])
")
if [ -z "${WID:-}" ]; then
	echo "FAIL: DP-2 上没有大窗口"
	M windows | python3 -c "
import json,sys
d=json.load(sys.stdin); ws=d if isinstance(d,list) else d.get('windows',[])
for w in ws:
    g=w.get('geometry') or {}
    print('  id=%s out=%s %sx%s@%s,%s' % (w.get('id'),w.get('output'),g.get('width'),g.get('height'),g.get('x'),g.get('y')))
"
	exit 1
fi
echo "目标窗口 id=$WID geom=${WX},${WY} ${WW}x${WH}"

CX=$((WX + WW/2))
CY=$((WY + WH/2))

# 开始录屏
gpu-screen-recorder -w 3840x1080+0+0 -f 30 -o "$OUT/drag.mp4" >"$OUT/rec.log" 2>&1 &
RPID=$!
sleep 2

echo "== 起拖 =="
M move-cursor $CX $CY >/dev/null; sleep 0.4
M send-key press super >/dev/null
M pointer-button press left >/dev/null; sleep 0.5
echo "grab: $(M input | python3 -c "import json,sys; print(json.load(sys.stdin).get('grab'))")"

# 慢慢往左拖：每次 60px，间隔 120ms（约 8fps 的移动，录屏能看清）
X=$CX
STEPS=0
while [ $X -gt 900 ] && [ $STEPS -lt 40 ]; do
	X=$((X - 60))
	M move-cursor $X $CY >/dev/null
	sleep 0.12
	STEPS=$((STEPS+1))
done
echo "拖到 x=$X（$STEPS 步）"
sleep 0.8

echo "== 松手 =="
M pointer-button release left >/dev/null
M send-key release super >/dev/null
sleep 1.5

kill -TERM $RPID 2>/dev/null
wait $RPID 2>/dev/null
sleep 0.5

echo "== 结果 =="
M windows | python3 -c "
import json,sys
d=json.load(sys.stdin); ws=d if isinstance(d,list) else d.get('windows',[])
for w in ws:
    g=w.get('geometry') or {}
    if w.get('id')==$WID:
        print('窗口 %s 松手后: out=%s %sx%s@%s,%s' % ($WID,w.get('output'),g.get('width'),g.get('height'),g.get('x'),g.get('y')))
"
ls -la "$OUT/drag.mp4" 2>/dev/null
