#!/bin/bash
# 跨屏拖拽实测：拖拽过程中连续截图，直接看窗口有没有跟手、跨屏时糊不糊。
#
# 只读 + 注入指针 + 截图，不改配置、不重启。
set -u
cd "$(dirname "$0")"

export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
S=/run/user/1000/vidow-wayland-0.sock
WID=${1:-9}
OUT=/tmp/dragshots

rm -rf "$OUT"; mkdir -p "$OUT"

shot() {
	grim "$OUT/$1.png" >/dev/null 2>&1
	echo "  截图 $1"
}

state() {
	./build/vidowmsg --socket "$S" input 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
g=d.get('grab',{})
print('  grab=%s win=%s' % (g.get('mode'),g.get('window')))
" 2>/dev/null
}

echo "=== 复位 ==="
./build/vidowmsg --socket "$S" pointer-button release left >/dev/null 2>&1
./build/vidowmsg --socket "$S" send-key release super >/dev/null 2>&1
sleep 0.5
state

echo
echo "=== 抓起点：移到窗口标题栏 ==="
./build/vidowmsg --socket "$S" move-cursor 2128 60 >/dev/null 2>&1
sleep 0.3
./build/vidowmsg --socket "$S" send-key press super >/dev/null 2>&1
sleep 0.2
./build/vidowmsg --socket "$S" pointer-button press left >/dev/null 2>&1
sleep 0.4
state
shot "00-start"

echo
echo "=== 逐点往左拖（DP-2 2x -> DP-3 1x）==="
i=1
for x in 2000 1900 1800 1700 1600 1400 1200 1000 800 600; do
	./build/vidowmsg --socket "$S" move-cursor $x 60 >/dev/null 2>&1
	sleep 0.35
	printf "  cursor=%s " "$x"
	state | tr -d '\n'
	echo
	shot "$(printf '%02d-x%s' "$i" "$x")"
	i=$((i + 1))
done

echo
echo "=== 松手 ==="
./build/vidowmsg --socket "$S" pointer-button release left >/dev/null 2>&1
./build/vidowmsg --socket "$S" send-key release super >/dev/null 2>&1
sleep 0.6
state
shot "99-after"

echo
echo "截图在 $OUT"
ls -la "$OUT" | tail -13
