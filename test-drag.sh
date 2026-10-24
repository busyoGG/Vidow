#!/bin/bash
# 鼠标拖窗口：拖得动、能拖到**另一块屏**上、松手按落点归位。
#
# 三件事一起验（它们各自都可能悄悄坏掉，而且坏掉时屏幕上「看着还行」）：
#   1) 浮动窗口拖得动 —— 位置是「相对自己那块屏」记的（home_dx/home_dy），
#      只改绝对盒不刷新偏移，下一次 arrange 的 float_place_recursive() 会按旧偏移
#      把它摆回去：拖了等于没拖（这个 bug 真存在过）。
#   2) 浮动窗口拖到另一块屏上 → 改归那块屏显示着的工作区（窗口的 output 跟着变）。
#   3) 平铺窗口 Mod+左键拖到另一块屏上 → 整条列搬进那块屏的工作区，并按落点插到
#      列带的第几位（拖到左边第一列左边 = 插到最前面）。
#
# 怎么注入鼠标：headless 里没有真鼠标，用 `VIDOW_TEST_ACTION` 的 FIFO 形式发
#   pointer-motion x y / pointer-button left press|release mod
# 这两个动作走的是和真实输入**完全同一条路**（vidow_pointer_motion /
# vidow_pointer_button），不是另开一条捷径。`mod` = 假装按着 Mod（拖拽要求按着
# Mod，而测试环境里没有键盘）。
#
# 判据都带数字：拖了多少像素、拖过去之后窗口的 output 是不是变了、列顺序有没有换。
#
# 用法：./test-drag.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-drag-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-drag.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
-- 指针手势（拖动 / 改列宽 / 滚轮换列）现在是键位：合成器不内置，测试也得自己绑。
vidow.bind("Mod+mouse-left", "interactive-move")
vidow.bind("Mod+mouse-right", "interactive-resize")
vidow.bind("Mod+wheel-up", "focus-column left")
vidow.bind("Mod+wheel-down", "focus-column right")
scale = "1"
gaps = 10
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

BIN=${VIDOW_BIN:-./build/vidow}
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "test-drag.sh: vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }
# 窗口状态一行一条：标题 / 在哪块屏 / 浮动与否 / 位置尺寸
wins() {
	./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    g = w["float_geometry"] if w["floating"] else w["geometry"]
    print("%s %s %s %d %d %d %d" % (w["title"], w["output"], w["floating"],
                                    g["x"], g["y"], g["width"], g["height"]))'
}
field() { wins | awk -v t="$1" '$1 == t { print $'"$2"' }'; }

python3 ./tools/wall.py >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.2

python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.6

echo "== 一块屏上的浮动窗口：拖得动 =="
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.5
X0=$(field w1 4); Y0=$(field w1 5); OUT0=$(field w1 2)
GAP=10
# 这块屏的布局原点：贴左边缘的平铺窗口位置减去 gap（同一个窗口浮动前的 x）
ORIGIN=$((X0 - GAP))
# 另一块屏的原点：两块屏横着排，各 1280 宽
OTHER_ORIGIN=$((ORIGIN == 0 ? 1280 : 0))

# 从窗口里起拖，往右拖 200（y 不动：窗口 700 高，屏 720 高，竖着没地方走）
act "pointer-motion $((X0 + 300)) $((Y0 + 300))"
act "pointer-button left press mod"
act "pointer-motion $((X0 + 400)) $((Y0 + 300))" 0.25
act "pointer-motion $((X0 + 500)) $((Y0 + 300))" 0.25
act "pointer-button left release mod" 0.5
X1=$(field w1 4); Y1=$(field w1 5); OUT1=$(field w1 2)
if grep -aq "grab move: w1" "$LOG"; then
	pass "拖拽真的起来了（grab move: w1）"
else
	fail "没起来拖拽（没有 grab move 日志）"
fi
if [ "$((X1 - X0))" -ge 150 ]; then
	pass "往右拖 200 → 窗口从 $X0 到了 $X1（+$((X1 - X0))，没被 arrange 摆回去）"
else
	fail "窗口没跟着走：$X0 → $X1（只动了 $((X1 - X0)) 像素）"
fi
if [ "$OUT1" = "$OUT0" ]; then
	pass "还在原来那块屏上（$OUT1）"
else
	fail "没拖出屏外却换屏了：$OUT0 → $OUT1"
fi

echo
echo "== 浮动窗口拖到另一块屏：改归属 =="
# 从窗口里起拖，一路拖到另一块屏中间，松手
act "pointer-motion $((X1 + 300)) $((Y1 + 300))"
act "pointer-button left press mod"
act "pointer-motion $((OTHER_ORIGIN + 400)) $((Y1 + 300))" 0.25
act "pointer-motion $((OTHER_ORIGIN + 700)) $((Y1 + 300))" 0.25
# **拖拽过程中**（还没松手）窗口的位置必须已经跨到对面那块屏上。
# 以前这里不查：只验松手后的归属，于是「拖的时候被裁在自己那块屏里、
# 屏幕上看着窗口根本不动」这个 bug 一路绿灯（用户报的跨屏拖不动）。
XMID=$(field w1 4)
if [ -n "$XMID" ] && [ "$XMID" -ge "$((OTHER_ORIGIN - 100))" ]; then
	pass "拖拽中窗口已经跟着光标跨屏：x=$XMID（对面屏从 $OTHER_ORIGIN 起）"
else
	fail "拖拽中窗口还在原屏：x=${XMID:-?}（对面屏从 $OTHER_ORIGIN 起）"
fi
# 裁剪不能把正在拖的窗口裁掉：view_clip_box 对它必须放行，
# 日志里那句「裁到自己那块屏里」不该出现在拖拽期间的这个窗口上。
act "pointer-button left release mod" 0.6
X2=$(field w1 4); OUT2=$(field w1 2)
if [ "$OUT2" != "$OUT1" ]; then
	pass "拖到另一块屏 → 归属跟着变（$OUT1 → $OUT2）"
else
	fail "拖过去还留在 $OUT1（跨屏没生效）"
fi
if grep -aq "grab: 浮窗拖到 $OUT2 上" "$LOG"; then
	pass "日志记下了跨屏那一步（grab: 浮窗拖到 $OUT2 上）"
else
	fail "没有跨屏日志（grab: 浮窗拖到 …）"
fi
# 落点应该是「光标位置 - 抓点偏移」，误差允许 ±40px（中途几次 motion 的取整）
WANT=$((OTHER_ORIGIN + 700 - 300))
if [ "$X2" -ge $((WANT - 40)) ] && [ "$X2" -le $((WANT + 40)) ]; then
	pass "落在光标松手的地方：x=$X2（期望 ≈$WANT）"
else
	fail "落点不对：x=$X2（期望 ≈$WANT）"
fi

echo
echo "== 平铺窗口拖到另一块屏：整条列搬过去 =="
./build/vidowmsg toggle-floating --title w1 >/dev/null
sleep 0.4
python3 ./tools/pattern.py w2 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.6
W1OUT=$(field w1 2); W2OUT=$(field w2 2)
W2X=$(field w2 4); W2Y=$(field w2 5)

# 拖 w2 到 w1 那块屏**最左边那条外层 gap 里**（= 新开一条列插到最前面，不是插
# 进 w1 那条列 —— 光标再往右挪一点就落在 w1 窗口身上了，那是 niri 的 InColumn）
if [ "$W1OUT" = "$OUT2" ]; then TARGET=$OTHER_ORIGIN; else TARGET=$ORIGIN; fi
act "pointer-motion $((W2X + 300)) $((W2Y + 300))"
act "pointer-button left press mod"
act "pointer-motion $((TARGET + 2)) $((W2Y + 300))" 0.25
act "pointer-button left release mod" 0.8
W2OUT2=$(field w2 2)
if grep -aq "grab tile: w2" "$LOG"; then
	pass "平铺窗口的拖拽起来了（grab tile: w2）"
else
	fail "平铺拖拽没起来（没有 grab tile 日志）"
fi
if [ "$W2OUT2" = "$W1OUT" ]; then
	pass "拖到 w1 那块屏上 → w2 归到那块屏的工作区（$W2OUT → $W2OUT2）"
else
	fail "跨屏没生效：w2 还在 $W2OUT2（w1 在 $W1OUT）"
fi

echo
echo "== 平铺窗口按落点插入列位置 =="
# w2 那条列这会儿排在最前面（上一节落的第 1 位）。把 **w1** 拖到同一块屏最左边
# 那条外层 gap 里 → 新开一条列、插到第 1 位（niri 的 NewColumn(0)）→ w1 换到
# w2 前面。光标要真的在条带左边那条缝里：落在窗口身上就是「插进那条列」了
# （niri 的 InColumn，见 test-drag-insert.sh）。
A=$(field w1 4); B=$(field w2 4)
if [ "$W2OUT2" = "$OUT2" ]; then LEFT=$OTHER_ORIGIN; else LEFT=$ORIGIN; fi
act "pointer-motion $((A + 300)) $((W2Y + 300))"
act "pointer-button left press mod"
act "pointer-motion $((LEFT + 2)) $((W2Y + 300))" 0.25
act "pointer-motion $((LEFT + 2)) $((W2Y + 300))" 0.25
act "pointer-button left release mod" 0.8
A2=$(field w1 4); B2=$(field w2 4)
if grep -aq "grab: w1 在 HEADLESS-1 的第 1 块工作区里挪到第 1 位" "$LOG"; then
	pass "落点是「新开一条列、插到第 1 位」（niri 的 NewColumn）"
else
	fail "没有新列的日志（$(grep -a 'grab: w1' "$LOG" | tail -1)）"
fi
if [ "$A2" -lt "$B2" ]; then
	pass "w1 那条列换到了 w2 前面（w1 x=$A→$A2，w2 x=$B→$B2）"
else
	fail "没换到前面：w1 x=$A2，w2 x=$B2（w1 该在更左边）"
fi

exec 9>&-
echo
if [ $FAILED -eq 0 ]; then
	echo "全部通过 / test-drag.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
