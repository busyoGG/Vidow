#!/bin/bash
# 混合 DPI 死区回归：左 1x + 右 2x 两块屏，窗口挪到 2x 屏后**真实点击**必须送到 X。
#
# 这个 bug 的症状：画面正常（wayland 路径），但整块窗口点不着。
# 根因是 XWayland 的窗口位置按「布局坐标 × 全局 xwayland_scale」算，混合 DPI 下
# 算出来的 X 绝对坐标能飞出 X 屏（实测 3852 > X 屏宽 2560）。
#
# 判据：X 侧窗口的 Absolute upper-left X 必须 < X 屏宽；且逐个位置注入点击，
# xev 必须每个位置都收到新事件。
set -u

SRC="$(cd "$(dirname "$0")" && pwd)"
RUN=/tmp/vidow-deadzone-test
PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); printf '  PASS  %s\n' "$1"; }
bad()  { FAIL=$((FAIL + 1)); printf '  FAIL  %s\n' "$1"; }

cleanup() {
	# 只杀这个测试目录下的实例；用户桌面的 vidow 绝不碰（见 tools/kill-test-vidow.sh）
	VIDOW_KILL_DIR="$RUN" "$SRC/tools/kill-test-vidow.sh" "$RUN" >/dev/null 2>&1
	[ -n "${XEV_PID:-}" ] && kill "$XEV_PID" 2>/dev/null
	[ -n "${PROBE_PID:-}" ] && kill "$PROBE_PID" 2>/dev/null
	true
}
trap cleanup EXIT

rm -rf "$RUN"; mkdir -p "$RUN"
cat > "$RUN/config.lua" <<'EOF'
xwayland = true
xwayland_scale = "auto"
vidow.output{ name = "HEADLESS-1", scale = 1, x = 0, y = 0 }
vidow.output{ name = "HEADLESS-2", scale = 2, x = 1920, y = 0 }
EOF

export XDG_RUNTIME_DIR="$RUN"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2
export WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=gles2
export VIDOW_LOG=info VIDOW_CONFIG="$RUN/config.lua"

setsid "$SRC/build/vidow" > "$RUN/vidow.log" 2>&1 &
sleep 4

export WAYLAND_DISPLAY=$(grep -aoE 'WAYLAND_DISPLAY=[a-z0-9-]+' "$RUN/vidow.log" |
	tail -1 | cut -d= -f2)
export DISPLAY=$(grep -aoE 'DISPLAY=:[0-9]+' "$RUN/vidow.log" | tail -1 | cut -d= -f2)
: "${WAYLAND_DISPLAY:=wayland-0}"
: "${DISPLAY:=:0}"

msg() { "$SRC/build/vidowmsg" "$@"; }

# X 屏宽（设备像素合计）
XW=$(DISPLAY=$DISPLAY xdpyinfo 2>/dev/null |
	sed -n 's/.*dimensions: *\([0-9]*\)x.*/\1/p' | head -1)
if [ -z "$XW" ]; then
	bad "拿不到 X 屏宽（xdpyinfo）"
	echo "PASS=$PASS FAIL=$FAIL"; exit 1
fi
echo "  X 屏宽 = $XW"

# 起一个 xev 做事件接收器
setsid nohup xev -name dzev -geometry 300x200+0+0 > "$RUN/xev.log" 2>&1 &
XEV_PID=$!
sleep 2.5

# 把它挪到 2x 屏
msg move-window-to-output HEADLESS-2 >/dev/null 2>&1
sleep 1.5

GEO=$(msg windows 2>/dev/null | python3 -c "
import sys, json
for w in json.load(sys.stdin):
    if w.get('title') == 'dzev':
        g = w['float_geometry'] if w['floating'] else w['geometry']
        print(g['x'], g['y'], g['width'], g['height']); break
" 2>/dev/null)
if [ -z "$GEO" ]; then
	bad "窗口没上屏"
	echo "PASS=$PASS FAIL=$FAIL"; exit 1
fi
read -r GX GY GW GH <<< "$GEO"
echo "  窗口布局 $GX,$GY ${GW}x$GH（2x 屏）"

# 判据一：X 侧绝对 X 必须落在 X 屏内
XABS=$(DISPLAY=$DISPLAY xwininfo -name dzev 2>/dev/null |
	sed -n 's/.*Absolute upper-left X: *\([0-9-]*\).*/\1/p' | head -1)
if [ -n "$XABS" ] && [ "$XABS" -lt "$XW" ]; then
	ok "X 绝对 X=$XABS < 屏宽 $XW（没飞出屏）"
else
	bad "X 绝对 X=${XABS:-未知} 不在屏内（屏宽 $XW）—— 死区根因"
fi

# 判据二：逐个位置真实点击，每个都要有新事件
click() {
	local ex=$(( $1 * 10000 / 2560 )) ey=$(( $2 * 10000 / 720 ))
	"$SRC/build/vdrag" "$ex" "$ey" "$ex" "$ey" >/dev/null 2>&1
	sleep 0.25
	msg action "pointer-button left press" >/dev/null 2>&1
	sleep 0.12
	msg action "pointer-button left release" >/dev/null 2>&1
	sleep 0.35
}

GOT=0; TRIED=0
for frac in 10 30 50 70 90; do
	x=$(( GX + GW * frac / 100 )); y=$(( GY + GH / 2 ))
	before=$(grep -ac 'root:' "$RUN/xev.log" 2>/dev/null | head -1)
	before=${before:-0}
	click "$x" "$y"
	after=$(grep -ac 'root:' "$RUN/xev.log" 2>/dev/null | head -1)
	after=${after:-0}
	TRIED=$((TRIED + 1))
	if [ "$after" -gt "$before" ]; then
		GOT=$((GOT + 1))
	fi
done

if [ "$GOT" -eq "$TRIED" ]; then
	ok "2x 屏点击 $GOT/$TRIED 个位置全部送达"
else
	bad "2x 屏点击只有 $GOT/$TRIED 个位置送达 —— 死区"
fi

echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]