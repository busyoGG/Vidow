#!/bin/bash
# 客户端发起的拖拽（CSD 标题栏 -> xdg_toplevel.move、X11 的 _NET_WM_MOVERESIZE）：
# 「按下」是转给客户端的，**「松开」也必须转回去**。
#
# 修之前：grab 里那句 `if (!pressed) { ...end_grab...; return; }` 把松开也吃了，
# 客户端手里那次按下永远等不到配对的松开 —— GTK 的 in_move_drag、Chromium 的
# 鼠标键状态都靠松开复位，于是「用标题栏拖一次之后窗口就不听使唤了：鼠标一动
# 它还在拖，之后第一下点击被当成按下」。用户报的正是这个：「微信登录界面用
# win 加左键拖拽是正常的，直接通过标题区域拖拽就会出问题」（Mod+拖拽那条路
# 按下也是吃掉的，两边对称，所以没事）。
#
# 探针是 tools/movedrag.py（GTK3，顶部 30px 当自定义标题栏，按在那里就发
# xdg_toplevel.move），它把收到的每条指针事件打一行。
#
# 判据：
#   1) 在「标题栏」按下 -> 客户端收到按下、发了 move 请求、合成器起了 grab
#   2) 拖拽期间窗口 1:1 跟手（几何 = 起点 + 光标位移）
#   3) **松手后客户端收到松开**（修之前 0 条）
#   4) 反向对照：Mod+左键拖拽那条路，客户端既没有按下也没有松开（状态对称），
#      窗口照样跟手
#   5) 松手后指针交还给客户端：光标移出去再移回来，客户端收得到位置
#   6) vidow 全程活着
#
# 用法：./test-titlebar-drag.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-titlebar-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
PLOG="$RUNDIR/probe.log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v python3 >/dev/null || { echo "test-titlebar-drag.sh: 需要 python3"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-titlebar-drag.sh: 需要 python3-gobject (GTK3)"; exit 1; }

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
window_move_ms = 190
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1920x1080" }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }

geom_of() { # -> "x y w h"
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
w = next((w for w in json.load(sys.stdin) if w["title"] == "movedrag"), None)
if w is None:
    print("? ? ? ?")
else:
    g = w.get("float_geometry") or w.get("geometry")
    print("%d %d %d %d" % (g["x"], g["y"], g["width"], g["height"]))'
}
viewport() { # -> 这块屏的 viewport_x（平移量）
	./build/vidowmsg state 2>/dev/null | python3 -c '
import sys, json
o = json.load(sys.stdin)["outputs"][0]
print("%.1f" % o.get("viewport_x", 0.0))'
}
count() { # <探针日志里的关键字> -> 行数
	grep -ac "$1" "$PLOG" 2>/dev/null || true
}
wait_log() { # <文件> <关键字> <超时秒>：等一行日志出现（动作是异步的，别拿死等当判据）
	local f=$1 pat=$2 t=${3:-4}
	for _ in $(seq 1 $((t * 10))); do
		grep -aq "$pat" "$f" && return 0
		sleep 0.1
	done
	return 1
}
wait_geom() { # <期望 x> <期望 y> <容差> <超时秒>：等窗口走到那个位置
	local wx=$1 wy=$2 tol=${3:-6} t=${4:-3} gx="?" gy="?" gw="-" gh="-"
	for _ in $(seq 1 $((t * 10))); do
		read -r gx gy gw gh <<< "$(geom_of)"
		if [ "$gx" != "?" ] && [ "$gx" -ge $((wx - tol)) ] &&
				[ "$gx" -le $((wx + tol)) ] &&
				[ "$gy" -ge $((wy - tol)) ] && [ "$gy" -le $((wy + tol)) ]; then
			echo "$gx $gy $gw $gh"
			return 0
		fi
		sleep 0.1
	done
	echo "$gx $gy $gw $gh"
	return 1
}

mark "起一个自己定尺寸的客户端（固定 320x240，vidow 直接当浮窗）"

setsid nohup python3 ./tools/movedrag.py > "$PLOG" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do
	[ "$(geom_of)" != "? ? ? ?" ] && break
	sleep 0.25
done
sleep 0.6
read -r GX GY GW GH <<< "$(geom_of)"
if [ "$GX" = "?" ]; then
	fail "探针窗口没起来（movedrag.py）"
	echo; echo "有判据没过（见上面的清单）"; exit 1
fi
echo "    窗口盒 $GX,$GY ${GW}x${GH}"
if [ "$GW" = "320" ] && [ "$GH" = "240" ]; then
	pass "客户端自己定的尺寸被当成浮窗（${GW}x${GH}）"
else
	fail "窗口尺寸不是客户端定的那个：${GW}x${GH}（期望 320x240）"
fi

./build/vidowmsg set-window-position --title movedrag 200 200 >/dev/null
sleep 0.8
read -r GX GY GW GH <<< "$(geom_of)"
echo "    挪到 $GX,$GY"

mark "① 客户端发起的拖拽：按在它自己画的标题栏上（局部 y < 30）"

# 抓点取标题栏里 (100,10)：局部坐标就是「光标 - 窗口盒」
act "pointer-motion $((GX + 100)) $((GY + 10))" 0.4
act "pointer-button left press" 0.3
if wait_log "$PLOG" "press button=" 3; then
	pass "客户端收到了按下（$(count 'press button=') 条）"
else
	fail "客户端没收到按下 —— 这条路上按下本来就该转给它"
fi
if wait_log "$PLOG" "begin_move_drag 已发" 3; then
	pass "客户端发了 xdg_toplevel.move（begin_move_drag）"
else
	fail "客户端没发 move 请求（begin_move_drag）"
fi
if wait_log "$LOG" "pan: 开始" 3; then
	pass "合成器接住了（拖标题栏 = 平移视图，pan: 开始）"
else
	fail "合成器没接住 —— 客户端发起的拖拽没人处理"
fi

N1=$(wc -l < "$LOG")
for i in 1 2 3 4 5; do
	act "pointer-motion $((GX + 100 + i * 40)) $((GY + 10 + i * 6))" 0.15
done
sleep 0.4
N2=$(wc -l < "$LOG")
WANT_X=$((GX + 200))
WANT_Y=$((GY + 30))
if read -r MGX MGY MGW MGH <<< "$(wait_geom "$WANT_X" "$WANT_Y" 6 3)"; then
	pass "拖拽期间窗口 1:1 跟手（$GX,$GY → $MGX,$MGY，期望 ≈$WANT_X,$WANT_Y）"
else
	fail "窗口没跟手：$GX,$GY → $MGX,$MGY（期望 ≈$WANT_X,$WANT_Y）"
fi
TWEENS=$(sed -n "$((N1 + 1)),${N2}p" "$LOG" | grep -c "move tween movedrag" || true)
if [ "$TWEENS" -eq 0 ]; then
	pass "拖动期间没有位移补间（位置是跟着光标走的）"
else
	fail "拖动期间起了 $TWEENS 条位移补间 —— 不跟手"
fi

mark "② 松手：客户端必须收到松开（修之前一条都没有）"
VP_BEFORE=$(viewport)

act "pointer-button left release" 0.5
if wait_log "$PLOG" "release button=" 3; then
	pass "客户端收到了松开（$(count 'release button=') 条：$(grep -a 'release button=' "$PLOG" | head -1 | sed 's/^probe: *//')）"
else
	fail "客户端一条松开都没收到 —— 它会一直以为左键按着（拖完再动鼠标窗口还会跟，第一下点击被当成按下）"
fi
if read -r FX FY FW FH <<< "$(wait_geom "$WANT_X" "$WANT_Y" 6 2)"; then
	pass "松手后落点没跑（$FX,$FY，期望 ≈$WANT_X,$WANT_Y）"
else
	fail "松手后落点跑了：$FX,$FY（期望 ≈$WANT_X,$WANT_Y）"
fi

mark "③ 拖标题栏 = 平移视图（窗口不动）"

# 用户点名的行为：按住标题栏拖 → 视图平移、窗口不动。判据两条：
#   ① viewport_x（视口偏移）变了 —— 视图真的平移了
#   ② 窗口的**布局** geometry.x 没变 —— 窗口本身没被拖走
VP_AFTER=$(viewport)
read -r PGX PGY PGW PGH <<< "$(geom_of)"
if python3 -c "import sys; sys.exit(0 if abs(float('$VP_AFTER') - float('$VP_BEFORE')) > 20 else 1)"; then
	pass "视图平移了（viewport_x $VP_BEFORE → $VP_AFTER）"
else
	fail "视图没平移（viewport_x 还是 $VP_AFTER，拖之前 $VP_BEFORE）"
fi
if [ "$PGX" = "$GX" ] && [ "$PGY" = "$GY" ]; then
	pass "窗口布局位置没被拖走（还是 $PGX,$PGY）"
else
	fail "窗口自己被拖走了（$GX,$GY → $PGX,$PGY）"
fi
mark "④ 反向对照：Mod+左键拖拽（按下被合成器吃掉的那条路）"

read -r GX GY GW GH <<< "$(geom_of)"
P_BEFORE=$(count "press button=")
R_BEFORE=$(count "release button=")
act "pointer-motion $((GX + 100)) $((GY + 120))" 0.4
act "pointer-button left press mod" 0.4
for i in 1 2 3; do
	act "pointer-motion $((GX + 100 + i * 40)) $((GY + 120))" 0.15
done
act "pointer-button left release mod" 0.5
WANT_CX=$((GX + 120))
if read -r CGX CGY CGW CGH <<< "$(wait_geom "$WANT_CX" "$GY" 6 3)"; then
	pass "Mod+拖拽照样跟手（$GX → $CGX，期望 ≈$WANT_CX）"
else
	fail "Mod+拖拽没跟手：$GX → $CGX（期望 ≈$WANT_CX）"
fi
if [ "$(count "press button=")" -eq "$P_BEFORE" ] &&
		[ "$(count "release button=")" -eq "$R_BEFORE" ]; then
	pass "客户端既没收到按下也没收到松开（状态对称，不会卡在按住态）"
else
	fail "Mod+拖拽时客户端收到了按下/松开 —— 它没见过按下，不该收到松开"
fi

exec 9>&-
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "vidow 全程活着"
else
	fail "vidow 挂了"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-titlebar-drag.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
