#!/bin/bash
# Alt+Tab 的窗口切换器（niri 的 recent windows，实现见 src/mru.c）。
#
# 判据：
#   1) 台账：窗口依次开出来 → `vidowmsg mru` 的 recent 顺序 = 最后开的在最前
#   2) 按住显示：按住 Alt 连点 Tab 期间 active/showing 为真、**焦点先不动**，
#      日志 `mru: 面板显形`，每个窗口一条预览（几何都在屏内）
#   3) 连点 Tab 往前走：选中项按 recent 顺序一格一格走
#   4) 松开提交：松开 Alt → 日志 `mru: 提交`，焦点落到选中的那个
#   5) Esc 取消：焦点原地不动
#   6) 快速点一下（按住时间 < open_delay）：窗口切了，但面板从没显形（画面没动）
#   7) strip 一行：所有预览同一个 y；装不下时横向滚，选中的那个完整可见
#   8) grid：按 per_row 换行、不滚动、全在屏内；装不下时预览整体变小
#   9) 像素：面板显形后壁纸被压暗，预览那块还是亮的（能看到窗口的颜色）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-mru-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
WTYPE_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	stop_clients
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*wall.py*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

command -v grim >/dev/null || { echo "test-mru.sh: 需要 grim"; exit 1; }
command -v wtype >/dev/null || { echo "test-mru.sh: 需要 wtype"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-mru.sh: 需要 python3-gobject"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# debounce = 0：测试要的是「谁被焦点过就立刻算用过」，不测 debounce 本身（那条靠
# 日志里的「只是路过」）。
# open_delay = 0：先让面板立刻显形，好做几何 / 像素断言；「快速点一下不显形」
# 那条用 set-config 现场调大再测。
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 120
window_close_ms = 120
window_move_ms = 120
mru = { open_delay_ms = 0, debounce_ms = 0, highlight_padding = 30 }
vidow.bind("Alt+Tab", "next-window")
vidow.bind("Alt+Shift+Tab", "previous-window")
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

export WLR_HEADLESS_OUTPUTS=1
rm -f "$LOG"
setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 40); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
grep -aq "vidow ready" "$LOG" || { fail "vidow 没起来"; grep -a ERROR "$LOG" | head -5; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

# ---------------------------------------------------------------- 工具

# vidowmsg --raw 打的是信封 {"ok":…,"result":…}：这里统一剥一层，后面的断言
# 直接写 d["showing"] 这种。
mru_json() { ./build/vidowmsg --raw mru 2>/dev/null |
	python3 -c "import json,sys; print(json.dumps(json.load(sys.stdin)['result']))" 2>/dev/null; }
mq() { mru_json | python3 -c "import json,sys; d=json.load(sys.stdin); print($1)" 2>/dev/null; }
focused_title() { ./build/vidowmsg --raw focused 2>/dev/null |
	python3 -c "import json,sys; d=json.load(sys.stdin)['result']; w=d.get('window') or {}; print(w.get('title') or '(无)')" 2>/dev/null; }
# 工作区矩形（逻辑坐标）
work_box() { ./build/vidowmsg --raw outputs 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']
b=d[0]['layout']
print(b['x'], b['y'], b['width'], b['height'])
" 2>/dev/null; }

# 按住 Alt + N 次 Tab，停住 hold_ms 让我检查，然后松开 Alt（后台跑）
hold_tabs() { # hold_tabs <tab 次数> <按住多久再松手 ms>
	local n=$1 hold=$2
	local args=(-M alt)
	for _ in $(seq 1 "$n"); do args+=(-k Tab -d 120); done
	args+=(-s "$hold" -m alt)
	wtype "${args[@]}" >/dev/null 2>&1 &
	WTYPE_PID=$!
}

# ---------------------------------------------------------------- 起窗口

mark "1) 台账：5 个窗口，最后开的最前"
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
for n in p1 p2 p3 p4 p5; do
	python3 tools/pattern.py "$n" >/dev/null 2>&1 &
	sleep 0.6
done
sleep 1.2
n=$(mq 'len(d["recent"])')
recent=$(mq '" ".join(e["title"] for e in d["recent"])')
[ "$n" = "5" ] && pass "台账里有 5 个窗口（$n）" || fail "台账里是 $n 个窗口"
[ "$recent" = "p5 p4 p3 p2 p1" ] && pass "recent 顺序 = $recent" \
	|| fail "recent 顺序不对：$recent"
[ "$(focused_title)" = "p5" ] && pass "当前焦点是 p5" || fail "焦点不是 p5（$(focused_title)）"

# ---------------------------------------------------------------- 按住

mark "2) 按住 Alt + Tab：面板显形、焦点先不动"
before_png="$RUNDIR/before.png"
grim "$before_png" >/dev/null 2>&1
hold_tabs 1 2500
sleep 0.7
showing=$(mq 'd["showing"]')
active=$(mq 'd["active"]')
sel=$(mq 'd["selected"]')
slots=$(mq 'd["slots"]')
[ "$active" = "True" ] && pass "按住期间 active = true" || fail "active = $active"
[ "$showing" = "True" ] && pass "面板已显形（showing = true）" || fail "showing = $showing"
[ "$slots" = "5" ] && pass "5 个窗口都有预览（slots = 5）" || fail "slots = $slots"
[ "$(focused_title)" = "p5" ] && pass "焦点还没动（还是 p5）" \
	|| fail "焦点已经动了：$(focused_title)"
grep -aq "mru: 面板显形" "$LOG" && pass "日志：面板显形" || fail "日志里没有「面板显形」"

# 选中的是「当前窗口的下一个」（p4）
sel_title=$(mq 'next((e["title"] for e in d["entries"] if e["selected"]), "(无)")')
[ "$sel_title" = "p4" ] && pass "选中 = p4（当前窗口的下一个）" \
	|| fail "选中 = $sel_title（id $sel）"

# 几何：都在屏内；strip 一行 = 同一个 y
read -r wx wy ww wh <<<"$(work_box)"
geo_ok=$(mq "all(e['x'] >= $wx and e['y'] >= $wy and e['x'] + e['w'] <= $wx + $ww and e['y'] + e['h'] <= $wy + $wh for e in d['entries'] if e['selected'] or d['total_w'] <= $ww)")
[ "$geo_ok" = "True" ] && pass "所有预览都在工作区里（$wx,$wy ${ww}x${wh}）" \
	|| fail "有预览跑到工作区外面了：$(mq "[(e['title'],e['x'],e['y'],e['w'],e['h']) for e in d['entries']]")"
rows=$(mq "len(set(e['y'] for e in d['entries']))")
[ "$rows" = "1" ] && pass "strip 是一行（y 全相同）" || fail "strip 有 $rows 个 y"

# 像素：面板显形后壁纸被压暗
grim "$RUNDIR/during.png" >/dev/null 2>&1
dim=$(python3 - "$before_png" "$RUNDIR/during.png" "$wx" "$wy" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB")
b = Image.open(sys.argv[2]).convert("RGB")
x, y = int(sys.argv[3]), int(sys.argv[4])
# 取左上角一小块（那儿是壁纸，没有窗口）
def avg(im):
    px = [im.getpixel((x + i, y + i)) for i in range(6)]
    return sum(sum(p) for p in px) / (3 * len(px))
print("%.1f %.1f" % (avg(a), avg(b)))
PY
)
set -- $dim
if [ -n "${1:-}" ]; then
	before_sum=$1; during_sum=$2
	python3 -c "import sys; sys.exit(0 if $during_sum < $before_sum * 0.6 else 1)" \
		&& pass "壁纸被压暗了（$before_sum -> $during_sum）" \
		|| fail "壁纸没被压暗（$before_sum -> $during_sum）"
else
	fail "像素比较没跑起来"
fi

# ---------------------------------------------------------------- 松开提交

mark "3) 松开 Alt：提交 + 焦点移动"
wait $WTYPE_PID 2>/dev/null
sleep 0.8
grep -aq "mru: 提交" "$LOG" && pass "日志：提交" || fail "日志里没有「提交」"
[ "$(focused_title)" = "p4" ] && pass "焦点给到选中的 p4" \
	|| fail "焦点是 $(focused_title)，不是 p4"
[ "$(mq 'd["showing"]')" = "False" ] && pass "面板关掉了" || fail "面板还开着"

mark "4) 连点 Tab 往前走两格"
hold_tabs 2 2500
sleep 0.7
sel2=$(mq 'next((e["title"] for e in d["entries"] if e["selected"]), "(无)")')
[ "$sel2" = "p3" ] && pass "两次 Tab 之后选中 = p3" || fail "两次 Tab 之后选中 = $sel2"
wait $WTYPE_PID 2>/dev/null
sleep 0.8
[ "$(focused_title)" = "p3" ] && pass "松开后焦点 = p3" || fail "松开后焦点 = $(focused_title)"

mark "5) Esc 取消：焦点原地不动"
hold_tabs 1 2000
# Esc 在松开 Alt 之前按下去（另起一个虚拟键盘按 Escape）
sleep 0.6
wtype -k Escape >/dev/null 2>&1
sleep 0.8
grep -aq "mru: 取消" "$LOG" && pass "日志：取消" || fail "日志里没有「取消」"
wait $WTYPE_PID 2>/dev/null
sleep 0.6
[ "$(focused_title)" = "p3" ] && pass "焦点还是 p3（没被取消那一下搬走）" \
	|| fail "焦点变成 $(focused_title) 了"

# ---------------------------------------------------------------- 延迟

mark "6) 快速点一下（按住时间 < open_delay）：切了窗口但面板没显形"
./build/vidowmsg set-config mru_open_delay_ms 400 >/dev/null 2>&1
n_before=$(grep -ac "mru: 面板显形" "$LOG")
wtype -M alt -k Tab -m alt >/dev/null 2>&1
sleep 0.8
n_after=$(grep -ac "mru: 面板显形" "$LOG")
[ "$n_before" = "$n_after" ] && pass "面板一次都没显形（$n_before -> $n_after）" \
	|| fail "面板显形了（$n_before -> $n_after）"
[ "$(focused_title)" = "p4" ] && pass "窗口还是切了：焦点 = p4" \
	|| fail "焦点 = $(focused_title)（期望 p4）"
./build/vidowmsg set-config mru_open_delay_ms 0 >/dev/null 2>&1

# ---------------------------------------------------------------- 排版

mark "7) strip：装不下就横向滚，选中的那个完整可见"
./build/vidowmsg set-config mru_max_scale 1.0 >/dev/null 2>&1
./build/vidowmsg set-config mru_max_height 600 >/dev/null 2>&1
hold_tabs 4 2500
sleep 0.7
total_w=$(mq 'd["total_w"]')
scroll=$(mq 'd["scroll"]')
rows=$(mq "len(set(e['y'] for e in d['entries']))")
vis=$(mq "all(e['x'] >= $wx and e['x'] + e['w'] <= $wx + $ww for e in d['entries'] if e['selected'])")
[ "$rows" = "1" ] && pass "还是一行" || fail "strip 变成了 $rows 行"
[ "$total_w" -gt "$ww" ] && pass "条子比屏幕宽（$total_w > $ww），确实滚了（scroll=$scroll）" \
	|| fail "条子没超过屏幕宽（$total_w vs $ww），这条测不到"
[ "$vis" = "True" ] && pass "选中的那个完整可见" \
	|| fail "选中的那个被切了：$(mq "[(e['title'],e['x'],e['w']) for e in d['entries'] if e['selected']]")"
wait $WTYPE_PID 2>/dev/null
sleep 0.8

mark "8) grid：换行铺在屏幕里，装不下就整体缩小"
./build/vidowmsg set-config mru_layout grid >/dev/null 2>&1
./build/vidowmsg set-config mru_per_row 2 >/dev/null 2>&1
hold_tabs 1 2500
sleep 0.7
layout=$(mq 'd["layout"]')
rows=$(mq "len(set(e['y'] for e in d['entries']))")
per_row_max=$(mq "max(sum(1 for e in d['entries'] if e['y'] == y) for y in set(e['y'] for e in d['entries']))")
zoom=$(mq "max(e['zoom'] for e in d['entries'])")
total_h=$(mq 'd["total_h"]')
[ "$layout" = "grid" ] && pass "配置生效：layout = grid" || fail "layout = $layout"
[ "$rows" = "3" ] && pass "5 个窗口按每行 2 个排成 3 行" || fail "排成了 $rows 行"
[ "$per_row_max" -le 2 ] && pass "每行不超过 2 个（最多 $per_row_max）" \
	|| fail "有一行塞了 $per_row_max 个"
[ "$total_h" -le "$wh" ] && pass "整块铺进屏幕（高 $total_h <= $wh）" \
	|| fail "整块比屏幕还高（$total_h > $wh）"
python3 -c "import sys; sys.exit(0 if $zoom < 0.99 else 1)" \
	&& pass "装不下 → 预览整体缩小（zoom = $zoom）" \
	|| fail "预览没缩小（zoom = $zoom）"
geo_ok=$(mq "all(e['x'] >= $wx and e['y'] >= $wy and e['x'] + e['w'] <= $wx + $ww and e['y'] + e['h'] <= $wy + $wh for e in d['entries'])")
[ "$geo_ok" = "True" ] && pass "所有预览都在屏幕里" || fail "有预览跑到屏幕外"
wait $WTYPE_PID 2>/dev/null
sleep 0.8

mark "9) 收尾"
./build/vidowmsg set-config mru_layout strip >/dev/null 2>&1
./build/vidowmsg set-config mru_max_scale 0.5 >/dev/null 2>&1
[ "$(mq 'd["showing"]')" = "False" ] && pass "面板已收起" || fail "面板还开着"
./build/vidowmsg state >/dev/null 2>&1 && pass "vidow 存活" || fail "vidow 挂了"

echo
if [ "$FAILED" = "0" ]; then
	echo "test-mru.sh: 全部通过"
else
	echo "test-mru.sh: 有失败（日志在 $LOG）"
	[ -n "${VIDOW_TEST_KEEP:-}" ] || echo "  （想留现场：VIDOW_TEST_KEEP=1 再跑一次）"
fi
exit "$FAILED"
