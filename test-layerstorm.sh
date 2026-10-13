#!/bin/bash
# 两块屏（1 倍 + 2 倍）上一起上屏一批 layer 面：vidow 不许在**开关动画**里重跑布局。
#
# 靶子：tools/layerstorm.py —— 每块屏一套 layer 面（壁纸 + 状态栏 + 停靠栏 + 四个角，
# 和 noctalia 摆的一样），两块屏共 14 块，一起上屏，每块都有 3 秒的入场动画。
#
# 修之前：layer 的入场动画每帧调一次 vidow_arrange_layers()，而那是**布局** ——
# 它每碰到一块 layer 面就调一次 wlroots 的 configure，configure 是无条件发包的。
# 于是「每块正在动的面各跑一趟」= 面数² 条 configure：14 块面一起动，每帧
# 14×14 = 196 条，一秒上万条（实测 300ms 里 8000 条 / 190KB）。客户端一时读不过来
# （启动那几秒正是它最忙的时候，或者像这里一样被冻住），socket（213504 字节）灌满
# 之后 libwayland 的 4KB 出口缓冲放不下下一条事件，直接判 E2BIG 把整个客户端踢掉：
# vidow 日志里是
#     Data too big for buffer (4092 + 12 > 4096).
#     error in client communication (pid …)
# 客户端看到的是 EPIPE（display_error=32「断开的管道」），就是用户报的 noctalia
# 断连（见 README 241；240 那条是「vidow 自己崩了」，机制不同）。
#
# 判据：
#   1) 面全部上屏之后、动画还在跑、布局没变的那 0.5 秒里，configure 不超过面数
#      （一次完整布局的量）—— 修之前是 面数 × 帧数
#   2) 客户端被冻住 2 秒（一个字节都读不动）之后还活着：进程还在、它的面还在
#   3) vidow 日志里没有 "Data too big for buffer" / "error in client communication"
#   4) vidow 全程活着，日志里没有 ERROR
#
# 用法：./test-layerstorm.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-layerstorm-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$CLIENT_PID" ] && kill -CONT "$CLIENT_PID" 2>/dev/null
	[ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-layerstorm.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1920x1080" }
vidow.output{ name = "HEADLESS-2", scale = 2, mode = "1920x1080" }
-- 动画拉长到 3 秒：修之前那一整套重排要连着跑 3 秒，量起来不用抢时间
layer_open_ms = 3000
layer_close_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=2
export WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER_ALLOW_SOFTWARE=1
export HOME="$CFGDIR"
export VIDOW_LOG=debug
unset DISPLAY WAYLAND_DISPLAY

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-layerstorm.sh: vidow 没起来，日志尾："
	tail -20 "$LOG"
	exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

PER=7
python3 ./tools/layerstorm.py "$PER" 0 > "$RUNDIR/client.log" 2>&1 &
CLIENT_PID=$!
for _ in $(seq 1 60); do grep -aq "面已上屏" "$RUNDIR/client.log" && break; sleep 0.25; done
grep -aq "面已上屏" "$RUNDIR/client.log" ||
	{ echo "test-layerstorm.sh: 靶子没上屏，客户端日志："; cat "$RUNDIR/client.log"; exit 1; }

# 让最后一块面的 map / initial commit（这两条路上重排是应该的）走完
sleep 0.4
FACES=$(grep -ac "layer map" "$LOG")
NS=$(./build/vidowmsg layers 2>/dev/null | grep -o '"namespace": *"storm' | wc -l)

mark "判据 1：布局没变时不许重排（动画还在跑）"
# 这 0.5 秒里没有任何面在 map / unmap、没有输出变化 —— 布局是静止的，
# 只有入场动画在动。修之前这里每帧 面数×面数 条 configure。
S1=$(stat -c%s "$LOG")
sleep 0.5
S2=$(stat -c%s "$LOG")
CFG_N=$(tail -c +$((S1 + 1)) "$LOG" | head -c $((S2 - S1)) | grep -ac "configure layer")
echo "  布局静止的 0.5 秒里 configure = $CFG_N 次（面数 $FACES，判据上限 $FACES）"
[ "$CFG_N" -le "$FACES" ] &&
	pass "静止期间没有 configure 洪水（$CFG_N ≤ $FACES）" ||
	fail "布局没变却在重发 configure：$CFG_N 次（上限 $FACES）—— 动画每帧在重排布局"

mark "判据 2：客户端冻住 2 秒也活下来"
kill -STOP "$CLIENT_PID" 2>/dev/null
sleep 2
kill -CONT "$CLIENT_PID" 2>/dev/null
sleep 0.5
if kill -0 "$CLIENT_PID" 2>/dev/null; then
	pass "客户端还活着（pid $CLIENT_PID）"
else
	fail "客户端被 vidow 断开了（进程没了）"
fi
NS2=$(./build/vidowmsg layers 2>/dev/null | grep -o '"namespace": *"storm' | wc -l)
[ "$NS2" -eq "$NS" ] && [ "$NS" -gt 0 ] &&
	pass "客户端的 $NS 块面还在（$NS2）" ||
	fail "客户端的面没了：上屏 $NS 块，现在 $NS2 块"

mark "判据 3：vidow 没有把客户端踢掉"
if grep -aqE "Data too big for buffer|error in client communication" "$LOG"; then
	fail "vidow 日志里有断连痕迹："
	grep -aE "Data too big for buffer|error in client communication" "$LOG" | head -3 | sed 's/^/        /'
else
	pass "没有 Data too big / error in client communication"
fi

mark "判据 4：vidow 全程活着"
if kill -0 "$VIDOW_PGID" 2>/dev/null && grep -aq "vidow ready" "$LOG"; then
	pass "vidow 还在跑"
else
	fail "vidow 不在了"
fi
if grep -aq "\[ERROR\]" "$LOG"; then
	fail "vidow 日志里有 ERROR："
	grep -a "\[ERROR\]" "$LOG" | head -3 | sed 's/^/        /'
else
	pass "日志里没有 ERROR"
fi

echo
if [ "$FAILED" -eq 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
