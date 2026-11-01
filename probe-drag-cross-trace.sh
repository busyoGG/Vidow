#!/bin/bash
# 隔离复现「跨屏拖拽过不去」：headless 两屏，1x + 2x，看 dragtrace 的 anim_off 是否跟手。
# 自带 XDG_RUNTIME_DIR / HOME，不碰正在跑的会话。
set -u
cd "$(dirname "$0")" || exit 1

R=$(mktemp -d /tmp/vidow-dragtrace.XXXXXX)
export XDG_RUNTIME_DIR=$R/run
export HOME=$R/home
mkdir -p "$XDG_RUNTIME_DIR" "$HOME"
chmod 700 "$XDG_RUNTIME_DIR"

LOG=$R/vidow.log
SOCK=$XDG_RUNTIME_DIR/vidow-wayland-0.sock

export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=2
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=pixman
export VIDOW_LOG=info
export VIDOW_LOG_FILE=$LOG
export VIDOW_DRAGTRACE=1

mkdir -p "$HOME/.config/vidow"
cat > "$HOME/.config/vidow/config.lua" <<'LUAEOF'
vidow.output { name = "HEADLESS-1", x = 0, y = 0, scale = 1 }
vidow.output { name = "HEADLESS-2", x = 1280, y = 0, scale = 2 }
LUAEOF

./build/vidow >"$R/stdout.log" 2>&1 &
VPID=$!
for _ in $(seq 1 100); do
	[ -S "$SOCK" ] && break
	sleep 0.1
done
if [ ! -S "$SOCK" ]; then
	echo "FAIL: vidow 没起来"; tail -20 "$R/stdout.log"; kill $VPID 2>/dev/null; exit 1
fi

M() { ./build/vidowmsg --socket "$SOCK" "$@" 2>/dev/null; }


echo "=== 输出 ==="
M outputs

echo "=== 起一个窗口 ==="
WAYLAND_DISPLAY=wayland-0 XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR \
	alacritty --class dragtest >"$R/alacritty.log" 2>&1 &
APID=$!
sleep 2.5
M windows

echo "=== 开始拖拽 ==="
# 拿第一个窗口的 id 和几何
read -r WID WX WY WW WH < <(M windows | python3 -c "
import json,sys
d=json.load(sys.stdin); ws=d if isinstance(d,list) else d.get('windows',[])
for w in ws:
    g=w.get('geometry') or {}
    if g.get('width',0)>50:
        print(w['id'], g['x'], g['y'], g['width'], g['height']); break
" 2>/dev/null)
if [ -z "${WID:-}" ]; then
	echo "SKIP: 没有可用窗口（foot 没起？）"; kill $VPID 2>/dev/null; exit 0
fi
echo "窗口 id=$WID geom=${WX},${WY} ${WW}x${WH}"

CX=$((WX + WW/2)); CY=$((WY + WH/2))
M move-cursor $CX $CY >/dev/null; sleep 0.2
M send-key press super >/dev/null
M pointer-button press left >/dev/null; sleep 0.3
M input | python3 -c "import json,sys; d=json.load(sys.stdin); print('grab:',d.get('grab'))"

: > "$LOG.dragmark"
MARK=$(wc -l < "$LOG")
for i in $(seq 0 10); do
	X=$((CX - i*120))
	M move-cursor $X $CY >/dev/null
	sleep 0.2
	echo "--- cursor x=$X ---"
done
M pointer-button release left >/dev/null
M send-key release super >/dev/null
sleep 0.3

echo "=== dragtrace（anim_off 是否随光标线性变化）==="
tail -n +$((MARK+1)) "$LOG" | grep -a 'dragtrace' | tail -25

echo "=== 判定 ==="
tail -n +$((MARK+1)) "$LOG" | grep -a 'dragtrace' | python3 -c "
import sys,re
xs=[]
for ln in sys.stdin:
    m=re.search(r'off=(-?[\d.]+),', ln)
    if m: xs.append(float(m.group(1)))
if len(xs)<3:
    print('FAIL: dragtrace 行太少（%d）' % len(xs)); sys.exit(1)
uniq=sorted(set(round(v,1) for v in xs))
print('anim_off_x 出现过的值:', uniq[:12], '...共 %d 个不同值' % len(uniq))
if len(uniq)>=4:
    print('PASS: anim_off 在跟手（%d 个不同值）' % len(uniq))
else:
    print('FAIL: anim_off 几乎不变 —— 窗口被卡住')
"

kill $VPID 2>/dev/null
wait $VPID 2>/dev/null
echo "日志: $LOG"
