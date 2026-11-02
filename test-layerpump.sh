#!/bin/bash
# 内容全透明的 layer 被关掉场景节点之后，补帧泵还在不在喂它。
#
# 为什么需要：wlr_scene 只给**可见**节点发 frame_done。quickshell 的 dock / bar /
# osd 交的是一张全透明缓冲（收起状态），vidow 会把它的场景节点整个关掉（把直通
# 扫屏让给全屏游戏）。节点一关，客户端等不到 frame_done 就不再画下一帧 —— 于是
# 收起的 dock 永远交不出「我出来了」的新内容，200ms 复检读到的永远是旧的全透明
# 缓冲，节点永远点不回来（现场：dock 隐藏后不回来、面板更新不及时）。
#
# 判据（全部来自客户端输出 / 日志）：
#   1) 合成器确实把节点关掉了：日志 `layer layerframes: 节点关（content=1`
#   2) 关掉之后客户端**还在出帧**：800~1400ms 之间至少 20 帧（补帧泵 16ms 一拍）
#   3) 客户端到点切成不透明之后，合成器复检到内容、节点点回来：
#      客户端打印 `show`，日志随后有 `节点开（content=0`
#   4) 整个 3 秒里客户端出的帧数 > 60
#
# 用法：./test-layerpump.sh
#       VIDOW_BIN=/tmp/opencode/vidow-old ./test-layerpump.sh   # 跑别的二进制
#       VIDOW_TEST_KEEP=1 ./test-layerpump.sh                   # 留着现场
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layerpump-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	if [ -n "$CLIENT_PID" ]; then
		kill "$CLIENT_PID" 2>/dev/null
	fi
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x "${VIDOW_BIN:-./build/vidow}" ] || { echo "test-layerpump.sh: 找不到 ${VIDOW_BIN:-./build/vidow}"; exit 1; }
[ -x ./build/layerframes ] || { echo "test-layerpump.sh: 需要 ./build/layerframes（ninja -C build）"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

rm -f "$LOG"
setsid "${VIDOW_BIN:-./build/vidow}" > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 40); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG"; then
	fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

mark "内容全透明的 layer：节点关掉之后补帧泵还在喂帧"
VIDOW_TEST_SHOW_MS=1500 ./build/layerframes 3 \
	> "$RUNDIR/frames.txt" 2> "$RUNDIR/frames.err" &
CLIENT_PID=$!
sleep 3.6
wait "$CLIENT_PID" 2>/dev/null
CLIENT_PID=""

if grep -aq "layer layerframes: 节点关（content=1" "$LOG"; then
	pass "节点确实被关掉了（内容全透明优化生效）"
else
	fail "日志里没有「layer layerframes: 节点关（content=1」—— 优化没触发，这条测试没测到东西"
	grep -a "layer layerframes" "$LOG" | tail -6
fi

# 800~1400ms：此时节点已经关了，客户端纯靠补帧泵活着。
middle=$(awk '$1 == "frame" && $3 >= 800 && $3 < 1400 { n++ } END { print n + 0 }' \
	"$RUNDIR/frames.txt")
if [ "$middle" -ge 20 ]; then
	pass "关节点之后 800~1400ms 出了 $middle 帧（补帧泵在喂）"
else
	fail "关节点之后 800~1400ms 只出了 $middle 帧（补帧泵没喂；旧代码这里是 0）"
	head -5 "$RUNDIR/frames.txt"
fi

last_vis=$(grep -a 'layer layerframes: 节点' "$LOG" | tail -1)
if ! grep -aq "^show " "$RUNDIR/frames.txt"; then
	fail "客户端没切到不透明（没收到帧回调，show 一行都没有）"
	grep -a "layer layerframes" "$LOG" | tail -6
elif [ "${last_vis#*节点开}" = "$last_vis" ]; then
	fail "切成不透明之后节点没点回来（最后一条可见性变化是「关」：$last_vis）"
	grep -a "layer layerframes" "$LOG" | tail -6
else
	pass "切成不透明之后节点点回来了（最后一条可见性变化是「节点开」）"
fi

total=$(awk '$1 == "frame" { n++ } END { print n + 0 }' "$RUNDIR/frames.txt")
if [ "$total" -gt 60 ]; then
	pass "3 秒一共出了 $total 帧"
else
	fail "3 秒只出了 $total 帧（应该在 60 以上）"
fi

mark "结果"
if [ "$FAILED" -eq 0 ]; then
	echo "  PASS  layer 补帧泵活着：隐藏的 layer 照样能更新、能回来"
	exit 0
fi
echo "  FAIL  layer 补帧泵有洞（见上面的失败项）"
exit 1
