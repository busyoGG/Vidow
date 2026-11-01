#!/bin/bash
# 实测跨屏拖拽：从 DP-2（2x）往 DP-3（1x）拖，逐点记录窗口几何、归属屏、
# 光标所在屏，看清「拖不过去」和「跨屏发糊」两个症状各自发生在哪一段。
#
# 只读 + 注入指针，不改配置、不重启。
set -u
cd "$(dirname "$0")"

S=/run/user/1000/vidow-wayland-0.sock
WID=${1:-9}

snap() {
	local tag=$1
	./build/vidowmsg --socket "$S" windows 2>/dev/null | \
		python3 -c "
import json,sys
d=json.load(sys.stdin); ws=d if isinstance(d,list) else d.get('windows',[])
for w in ws:
    if w.get('id')==$WID:
        g=w.get('geometry') or {}
        print('  %-10s geom=%4sx%4s@%5s,%s out=%s' % ('$tag', g.get('width'),g.get('height'),g.get('x'),g.get('y'),w.get('output')))
" 2>/dev/null
	./build/vidowmsg --socket "$S" cursor 2>/dev/null | \
		python3 -c "
import json,sys
d=json.load(sys.stdin)
print('             cursor=%s,%s' % (d.get('x'),d.get('y')))
" 2>/dev/null
}

echo "=== 布局 ==="
./build/vidowmsg --socket "$S" outputs 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
for o in (d if isinstance(d,list) else d.get('outputs',[])):
    print('  %-6s scale=%s logical=%sx%s' % (o.get('name'),o.get('scale'),o.get('width'),o.get('height')))
" 2>/dev/null

echo
echo "=== 拖拽前 ==="
snap "before"

# 从窗口标题栏位置抓起（窗口 x+200, y+10），往左拖
echo
echo "=== 开始拖拽 ==="
./build/vidowmsg --socket "$S" move-cursor 2128 60 >/dev/null 2>&1
sleep 0.3
./build/vidowmsg --socket "$S" pointer-button press left >/dev/null 2>&1
sleep 0.3

# 往左逐点拖，记录每一步
for x in 2000 1900 1800 1600 1400 1200 1000 800 600; do
	./build/vidowmsg --socket "$S" move-cursor $x 60 >/dev/null 2>&1
	sleep 0.25
	snap "x=$x"
done

echo
echo "=== 松手 ==="
./build/vidowmsg --socket "$S" pointer-button release left >/dev/null 2>&1
sleep 0.5
snap "after"
