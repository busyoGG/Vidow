#!/bin/bash
# 拖一个和别的窗口重叠的窗口时，模糊金字塔不该每帧重新导入贴图。
#
# 用户报的「有背景的时候拖拽还是会卡」：拖一个**与别的窗口重叠**的浮动窗口时，
# 每帧要重建两份私有底衬 + 跑两遍 kawase 链条。链条每一趟都要把上一趟的结果当源
# 采样，老写法是每趟 wlr_texture_from_buffer() 导入一张、用完销毁 —— 真后端上
# （GBM/dmabuf）每次都是一次 EGLImage 创建 + BO 映射/解映射。基准实测（headless
# 3840x2160@2，壁纸 + 两个重叠浮动窗口，33 帧/秒 地拖）：
#
#   修之前：每帧 2 次重建 / 2 遍链条 / **贴图 +23 次 -23 次**（30 帧的窗口累计 +690）
#   修之后：每帧 2 次重建 / 2 遍链条 / **贴图 +0 -0**（金字塔缓冲复用，贴图跟着复用）
#
# 判据（VIDOW_RENDER_STATS=1 打的那行「渲染统计」，只看拖拽期间那几行）：
#   1) 拖拽期间真的在重建私有底衬（own ≥ 10）—— 不然说明没复现「重叠」这个前提。
#      两个窗口都得是半透明的：不透明的窗口现在不算模糊（content.a == 1 的地方模糊
#      被乘成 0，看不出来），vidow 不再为它抓私有底衬 —— 实测同样这条拖拽，底下换回
#      不透明的 pattern.py 之后 own 从 104 掉到 3（那次是真没活干，不是漏抓）。
#   2) 拖拽期间贴图导入次数为 0 —— 这是这条测试要守的东西
#   3) 重建次数 / 指针步数 < 1.5 —— 只有「它底下那个窗口」该重建；被拖的窗口自己
#      每步都重抓就是白工（实测：修前 2.23 次/步，修后 0.52）。步长用 20ms 是为了让
#      每一步都渲染出一帧，不然帧数比步数少，比值会被稀释（4ms 步长下修前只有 0.79）
#   4) vidow 全程活着
#
# 用法：./test-blurcache.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurcache-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENTS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENTS; do kill "$p" 2>/dev/null; done
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in *sheet.py*|*pattern.py*|*wall.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-blurcache.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
-- 指针手势（拖动 / 改列宽 / 滚轮换列）现在是键位：合成器不内置，测试也得自己绑。
vidow.bind("Mod+mouse-left", "interactive-move")
vidow.bind("Mod+mouse-right", "interactive-resize")
vidow.bind("Mod+wheel-up", "focus-column left")
vidow.bind("Mod+wheel-down", "focus-column right")
vidow.output{ name = "HEADLESS-1", scale = 2, mode = "2560x1440" }
blur_enabled = true
blur_all = true
blur_levels = 4
blur_offset = 1.5
jelly = true
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug VIDOW_RENDER_STATS=1
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

mark "拖重叠窗口：每帧重建底衬，但不该重新导入贴图"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
grep -aq "vidow ready" "$LOG" || {
	echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.2

float_at() { # <标题> <x> <y> <宽> <高>
	local t="$1" x="$2" y="$3" w="$4" h="$5"
	for _ in $(seq 1 30); do
		./build/vidowmsg windows 2>/dev/null | grep -q "\"$t\"" && break
		sleep 0.3
	done
	for _ in $(seq 1 12); do
		./build/vidowmsg toggle-floating --title "$t" >/dev/null 2>&1
		local f=$(./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
print(next((w["floating"] for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), "?"))' "$t")
		[ "$f" = "True" ] && break
		sleep 0.3
	done
	./build/vidowmsg set-window-size --title "$t" "$w" "$h" >/dev/null 2>&1
	sleep 0.3
	./build/vidowmsg set-window-position --title "$t" "$x" "$y" >/dev/null 2>&1
	sleep 0.6
}

# 底下那个窗口必须是**半透明**的：不透明的窗口（pattern.py 那种）现在根本不算模糊
# —— 装饰着色器里 content.a == 1 的地方模糊被乘成 0，一个像素都透不出来，vidow 直接
# 跳过它那趟（见 fx.c 的 view_blur_visible）。拿它当「重叠」的靶子的话，这趟拖拽里
# 一条私有底衬都不会重建，前提判据就废了。
setsid nohup python3 ./tools/sheet.py wback 0.2 0.25 0.3 0.9 > "$RUNDIR/pat.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6
float_at wback 200 150 600 500
setsid nohup python3 ./tools/sheet.py wtop 0 0 0 0.5 > "$RUNDIR/sheet.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6
float_at wtop 400 300 600 500
sleep 1.5

# 拖 wtop：从窗口里起拖，往右下走 20 步、每步 ~30ms（约 33 帧/秒 的拖拽）
WXWY=$(./build/vidowmsg windows | python3 -c '
import sys, json
w = next(w for w in json.load(sys.stdin) if w["title"] == "wtop")
print(w["geometry"]["x"], w["geometry"]["y"])')
set -- $WXWY
CX=$(( $1 + 300 )); CY=$(( $2 + 250 ))
exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }
OFF=$(wc -c < "$LOG")
act "pointer-motion $CX $CY" 0.2
act "pointer-button left press mod" 0.2
for i in $(seq 1 100); do
	act "pointer-motion $((CX + i * 6)) $((CY + i * 4))" 0.02
done
act "pointer-button left release mod" 0.5
exec 9>&-

if grep -aq "grab move: wtop" "$LOG"; then
	pass "拖拽真的起来了（grab move: wtop）"
else
	fail "没起来拖拽（没有 grab move 日志）"
fi

tail -c +$((OFF + 1)) "$LOG" | grep -a "渲染统计" > "$RUNDIR/stats.txt"
LINES=$(grep -c . "$RUNDIR/stats.txt")
if [ "$LINES" -lt 2 ]; then
	fail "拖拽期间没有渲染统计输出（VIDOW_RENDER_STATS 没生效？）"
else
	pass "拖拽期间拿到 $LINES 行渲染统计"
fi

SUM=$(python3 - "$RUNDIR/stats.txt" <<'PY'
import re, sys
own = new = 0
for line in open(sys.argv[1]):
    m = re.search(r'own (\d+)', line)
    if m:
        own += int(m.group(1))
    m = re.search(r'贴图 \+(\d+)/-(\d+)', line)
    if m:
        new += int(m.group(1))
print(own, new)
PY
)
set -- $SUM
OWN=$1; NEW=$2
if [ "$OWN" -ge 10 ] 2>/dev/null; then
	pass "拖拽期间重建了私有底衬 $OWN 次（重叠那条路真的在跑）"
else
	fail "拖拽期间只有 $OWN 次私有底衬重建 —— 没复现「重叠」这个前提，后面那条判据不算数"
fi
STEPS=$(tail -c +$((OFF + 1)) "$LOG" | grep -ac "'pointer-motion .* 完成")
if [ "$STEPS" -ge 50 ] 2>/dev/null; then
	pass "拖拽步数记下来了（$STEPS 步）"
else
	fail "只数到 $STEPS 步指针动作 —— 拖拽没跑够，下面那条比值不算数"
fi
# 重建次数 / 步数：拖一个重叠窗口时**只有它底下那个窗口**该重建（它底下的画面真的在
# 变），被拖的那个自己的底衬抓的时候把它藏起来了、不该跟着变（只在区域跨网格时才重
# 建）。修之前（1ead9df）实测 2.23 次/步（两个窗口每步各一次）；修之后 0.52 次/步。
RATIO=$(python3 -c "print('%.2f' % ($OWN / max($STEPS, 1)))")
if python3 -c "import sys; sys.exit(0 if $OWN < 1.5 * max($STEPS, 1) else 1)"; then
	pass "拖拽期间私有底衬重建 $OWN 次 / $STEPS 步 = $RATIO 次/步（< 1.5；修前实测 2.23）"
else
	fail "拖拽期间私有底衬重建 $OWN 次 / $STEPS 步 = $RATIO 次/步 —— 被拖的窗口自己也每步重抓了"
fi

if [ "$NEW" = "0" ]; then
	pass "一次贴图都没重新导入（优化前同样的拖拽实测 +690/-690，每帧 23 次）"
else
	fail "拖拽期间又导入了 $NEW 张贴图 —— 金字塔的贴图没复用起来（每帧该是 0）"
fi

# 只看**测试自己起的那个**：以前这里是 `pgrep -x vidow`，而用户桌面会话也叫
# 这个名字 —— 测试自己的 vidow 早死了，这条照样 PASS（永远测不出来的假绿）。
if { [ -n "${VIDOW_PID:-}" ] && kill -0 "$VIDOW_PID" 2>/dev/null; } ||
{ [ -n "${VIDOW_PGID:-}" ] && kill -0 -- "-$VIDOW_PGID" 2>/dev/null; }; then
	pass "vidow 还活着"
else
	fail "vidow 死了（$(tail -3 "$LOG" | tr '\n' '|')）"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-blurcache.sh: OK"
else
	echo "test-blurcache.sh: 有失败"
fi
exit "$FAILED"
