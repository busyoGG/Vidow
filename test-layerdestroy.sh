#!/bin/bash
# layer surface 被销毁（客户端退出 / 面板重建）不能把合成器带走。
#
# 靶子：tools/bar.py 一块 50% 透明的 top 层状态栏 —— 它背后会画一份模糊（见
# test-layerblur.sh），那份模糊是个垫在 ls->scene->tree 里的场景节点。客户端一退，
# layer surface 连同它那棵树都被 wlroots 收走，我们的 destroy 监听要是排在 wlroots
# 后面，就是拿着已经释放的节点调 wlr_scene_node_destroy → 顺着野 parent 走 → 段错误。
# noctalia 每重建一次面板就走这条路。
#
# 注意：野指针不一定当场就炸 —— 释放过的块里 parent 那个字是啥由分配器决定，
# 有时候正好「走得下去」，所以崩是随机的（用户那边是「用着用着崩」）。修之前
# 跑这条测试，第 1 轮往往能过、第 2 轮才崩，所以这里要反复重建 5 轮。
#
# 判据：
#   1) 状态栏上屏后确实画了模糊（有节点可收，不然这条测试等于没测）
#   2) 客户端退出后 vidow 还活着（不崩、不自己退）
#   3) 每一轮的模糊节点都被收掉了（"layer blur release" 行数 == 轮数）
#   4) 反复重建 5 轮仍然活着，且状态栏真的从 layer 列表里没了
#   5) 5 轮之后 RSS 没长起来（每次重建漏一张缓冲的话这里会一路涨）
#
# 用法：./test-layerdestroy.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layerdestroy-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
ROUNDS=5
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi" 2>/dev/null || { echo "test-layerdestroy.sh: 需要 python3-gobject"; exit 1; }
python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-layerdestroy.sh: 需要 gtk-layer-shell"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-layerdestroy.sh: 先 ninja -C build"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1 }
blur_enabled = true
blur_levels = 4
blur_offset = 1.5
blur_alpha_min = 0.15
CFG

BIN=${VIDOW_BIN:-./build/vidow}
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-layerdestroy.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
SOCK="$XDG_RUNTIME_DIR/vidow-$WAYLAND_DISPLAY.sock"

alive() { kill -0 "$VIDOW_PID" 2>/dev/null; }
wait_for() { # <grep 模式> <最多等几秒>
	local i
	for i in $(seq 1 $(( $2 * 4 ))); do
		grep -aq "$1" "$LOG" && return 0
		alive || return 1
		sleep 0.25
	done
	return 1
}
# grep -c 没匹配时自己会打 0（但退出码是 1），所以只要吞掉退出码
count() { grep -ac "$1" "$LOG" 2>/dev/null || true; }

mark "1/2 状态栏上屏 + 画模糊"
setsid nohup python3 ./tools/bar.py > "$RUNDIR/bar.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
wait_for "layer map testbar" 10 || fail "状态栏没上屏"
sleep 1
first_region=$(count "blur: layer region")
if [ "$first_region" -gt 0 ]; then
	pass "状态栏背后的模糊画过了（$first_region 帧，有节点可收）"
else
	fail "状态栏背后没画模糊：这条测试测不到东西"
fi

mark "2/2 反复重建 $ROUNDS 轮"
rss_after_second=""
round=0
for round in $(seq 1 $ROUNDS); do
	blur_before=$(count "layer blur release")
	closed_before=$(count "layer closed testbar")
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	CLIENT_PIDS=""

	# 等这一轮收干净：release 和 closed 都往前走一格
	ok=0
	for _ in $(seq 1 40); do
		if [ "$(count 'layer blur release')" -gt "$blur_before" ] &&
				[ "$(count 'layer closed testbar')" -gt "$closed_before" ]; then
			ok=1; break
		fi
		alive || break
		sleep 0.25
	done

	if ! alive; then
		fail "第 $round 轮：客户端退出后 vidow 没了（崩了）"
		break
	fi
	if [ "$ok" = 1 ]; then
		pass "第 $round 轮：模糊节点收掉了，vidow 还在"
	else
		fail "第 $round 轮：没看到模糊节点被收（layer blur release 没动）"
	fi

	if [ "$round" = 2 ]; then
		rss_after_second=$(awk '/VmRSS/{print $2}' "/proc/$VIDOW_PID/status" 2>/dev/null)
	fi

	if [ "$round" != "$ROUNDS" ]; then
		setsid nohup python3 ./tools/bar.py > "$RUNDIR/bar$round.log" 2>&1 </dev/null &
		CLIENT_PIDS="$CLIENT_PIDS $!"
		sleep 1
	fi
done

alive && pass "$ROUNDS 轮重建之后 vidow 还活着" || fail "vidow 在重建过程里没了"

layers=$(timeout 5 ./build/vidowmsg --socket "$SOCK" layers 2>/dev/null)
if echo "$layers" | grep -q testbar; then
	fail "状态栏已经退出了，layer 列表里还有 testbar"
else
	pass "状态栏退出后从 layer 列表里消失了"
fi

rss_final=$(awk '/VmRSS/{print $2}' "/proc/$VIDOW_PID/status" 2>/dev/null)
if [ -n "$rss_after_second" ] && [ -n "$rss_final" ]; then
	grow=$(( (rss_final - rss_after_second) / 1024 ))
	if [ "$grow" -lt 30 ]; then
		pass "第 2 轮到第 $ROUNDS 轮 RSS 只长了 ${grow}MB（没漏缓冲）"
	else
		fail "第 2 轮到第 $ROUNDS 轮 RSS 长了 ${grow}MB：每次重建在漏东西"
	fi
else
	fail "量不到 RSS"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "test-layerdestroy.sh: 全部通过"
else
	echo "test-layerdestroy.sh: 有失败项"
fi
exit $FAILED
