#!/bin/bash
# Alt+Tab 的窗口切换器（niri 的 recent windows，实现见 src/mru.c）。
#
# 判据：
#   1) 台账：窗口依次开出来 → `vidowmsg mru` 的 recent 顺序 = 最后开的在最前
#   2) 按住显示：按住 Alt 连点 Tab 期间 active/showing 为真、**焦点先不动**，
#      日志 `mru: 面板显形`，每个窗口一条预览（几何都在屏内）
#   3) 连点 Tab 往前走：选中项按**屏幕顺序**（左→右）一格一格走；一进来选中的
#      那格还是「最近用过的那扇」（两个顺序分开）
#   4) 松开提交：松开 Alt → 日志 `mru: 提交`，焦点落到选中的那个
#   5) Esc 取消：焦点原地不动
#   6) 快速点一下（按住时间 < open_delay）：窗口切了，但面板从没显形（画面没动）
#   7) strip 一行：所有预览同一个 y；装不下时横向滚，选中的那个完整可见
#   8) grid：按 per_row 换行、不滚动、全在屏内；装不下时预览整体变小
#   9) 像素：面板显形后壁纸被压暗，预览那块还是亮的（能看到窗口的颜色）
#  10) 卡片（样式照 portal 的 item）：每项背后一张超椭圆圆角卡片，比预览大一圈
#      （内边距）；相邻不重叠；选中的那张描边是强调色，别的是卡片描边色；
#      圆角那四段弧真的画出来了（角节点没被关掉）、角尖是切掉的
#  11) 鼠标悬停：点亮指针底下那一张，**选中项不动**；挪到空白处悬停清掉
#  12) 鼠标点击：点哪张卡片就切到哪个窗口（不用等松修饰键），面板当场收起，
#      松手被吃掉（没漏给客户端）
#  14) 工作区范围：默认 scope = workspace（只看当前工作区），换成 all 之后别的
#      工作区的窗口也进清单
#  13) 浮层：切换器**不动窗口** —— 面板开着期间每个窗口的位置/尺寸一字不变；
#      卡片里画的是窗口内容（缩略图，全亮），桌面那层被压暗（面板浮在窗口之上）；
#      预览走装饰管线 → 形状和真窗口一样（圆角 / 描边 / 阴影）
#  15) 滚轮 = 选中项 ±1（日志 `mru: 滚轮 → #`）；**到边停住、不循环**（日志
#      `mru: 滚轮到边`）—— 故意和 Alt+Tab 的绕回不一样；滚轮不改布局、不换工作区
#      （测试配置里就写着 `Alt+wheel-up/down` = workspace up/down，改前真的会跳）
#  16) 模态（B1/B2）：面板按着时字母键进不了客户端；open_delay 期的点击 / 移动
#      也不透给窗口（模态从**按下 Alt** 那一刻算起，不等面板显形）
#  17) 模态（B3/B4/B5）：Mod+滚轮不换列；切换器自己的键照常（方向键 ±1 且到边停住、
#      Esc / Return、点卡片）；别的绑定被白名单挡住（日志「切换器开着期间不生效」）
#  注：手势（触控板滑 / 捏）也按同一套挡了（src/gesture.c 每个入口一道闸），但这份
#      脚本里没配手势绑定，没法在这儿判 —— 手势本身的回归在 test-gesture.sh。
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
	[ -n "${KEYPROBE_PID:-}" ] && kill "$KEYPROBE_PID" 2>/dev/null
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
		*pattern.py*|*wall.py*|*keyprobe.py*) kill "$pid" 2>/dev/null ;;
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
mru = { open_delay_ms = 0, debounce_ms = 0, card_padding = 12 }
vidow.bind("Alt+Tab", "next-window")
vidow.bind("Alt+Shift+Tab", "previous-window")
-- 用户现场那两条（configs/14-keys.lua 的默认键位里就有）：按住 Alt 滚滚轮 =
-- 换工作区。A3 / B3 拿它们当**证伪手段** —— 改前按住 Alt 滚一下，工作区真的会跳。
vidow.bind("Alt+wheel-up", "workspace up")
vidow.bind("Alt+wheel-down", "workspace down")
-- 「Mod+滚轮 = 换列」在切换器开着时的那一份：切换器自己按着 Alt，所以写成
-- Mod+Alt+滚轮（只写 Mod 的话按着 Alt 根本匹配不上，测不到东西）。
vidow.bind("Mod+Alt+wheel-up", "focus-column left")
vidow.bind("Mod+Alt+wheel-down", "focus-column right")
-- B5 的对照组：一条「别的动作」的绑定，切换器开着时必须被白名单挡住。
-- 用 left（不是 right）：探针窗口是最后开的、就在最右边，right 到头是空转，
-- 焦点不变分不清「被挡」和「到头」。
vidow.bind("Alt+j", "focus-column left")
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
# 同上，但把一串值（tuple / list）空格分开打出来，好喂给 read。
mj() { mru_json | python3 -c "import json,sys; d=json.load(sys.stdin); print(*($1))" 2>/dev/null; }
focused_title() { ./build/vidowmsg --raw focused 2>/dev/null |
	python3 -c "import json,sys; d=json.load(sys.stdin)['result']; w=d.get('window') or {}; print(w.get('title') or '(无)')" 2>/dev/null; }
# 工作区矩形（逻辑坐标）
work_box() { ./build/vidowmsg --raw outputs 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']
b=d[0]['layout']
print(b['x'], b['y'], b['width'], b['height'])
" 2>/dev/null; }

# IPC 注入（鼠标那条路和真设备同一条：move-cursor / pointer-button）
msg() { ./build/vidowmsg --raw "$@" >/dev/null 2>&1; }
# 截图里 (x, y) 那一点的颜色，打 "R G B"
win_geo() { ./build/vidowmsg --raw windows 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']
for w in d:
    if w.get('popup'):
        continue
    g = w.get('geometry') or {}
    print(w.get('title'), g.get('x'), g.get('y'), g.get('width'), g.get('height'))
" 2>/dev/null; }
# 截图里 (x, y) 那一点的亮度（三个通道之和）
pix_sum() { # pix_sum <png> <x> <y>
	python3 - "$1" "$2" "$3" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print(sum(im.getpixel((int(sys.argv[2]), int(sys.argv[3])))))
PY
}
pix() { # pix <png> <x> <y>
	python3 - "$1" "$2" "$3" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print(*im.getpixel((int(sys.argv[2]), int(sys.argv[3]))))
PY
}
# 颜色像不像（每个通道容差 tol）
color_like() { # color_like "<R G B>" "<R G B>" <tol>
	python3 -c "
import sys
a = [int(v) for v in sys.argv[1].split()]
b = [int(v) for v in sys.argv[2].split()]
sys.exit(0 if len(a) == 3 and len(b) == 3 and
         all(abs(x - y) <= int(sys.argv[3]) for x, y in zip(a, b)) else 1)
" "$1" "$2" "$3"
}

# 按住 Alt + N 次 Tab，停住 hold_ms 让我检查，然后松开 Alt（后台跑）
hold_tabs() { # hold_tabs <tab 次数> <按住多久再松手 ms>
	local n=$1 hold=$2
	local args=(-M alt)
	for _ in $(seq 1 "$n"); do args+=(-k Tab -d 120); done
	args+=(-s "$hold" -m alt)
	wtype "${args[@]}" >/dev/null 2>&1 &
	WTYPE_PID=$!
}

# 按住 Alt + 一次 Tab（把切换器打开），**再把别的修饰键也按上**，停住 hold_ms 一起松开。
# B3 用：切换器开着 + Mod 也按着（「Mod+滚轮 = 换列」那一份绑定才有机会命中）。
# 顺序不能反：先按 Mod 再按 Tab 的话，那一串修饰键是「Mod+Alt+Tab」，
# `Alt+Tab` 绑定匹配不上（mods 要正好相等）—— 切换器根本不会开。
hold_alt_tab_mod() { # hold_alt_tab_mod <hold_ms> <修饰键…>
	local hold=$1
	shift
	local args=(-M alt -k Tab -d 150)
	for m in "$@"; do args+=(-M "$m"); done
	args+=(-s "$hold")
	for m in "$@"; do args+=(-m "$m"); done
	args+=(-m alt)
	wtype "${args[@]}" >/dev/null 2>&1 &
	WTYPE_PID=$!
}

# 按住 Alt，依次敲一串键（第一个一般是 Tab：把切换器打开），停住 hold_ms 再松开 Alt。
# **必须在同一个 wtype 里敲**：修饰键状态是**每块键盘**的，另起一个 wtype 敲出来的
# 键在 vidow 那边 mods = 0，`Alt+…` 的绑定根本匹配不上（B5 要的就是「被白名单挡住
# 的那条绑定本来会命中」）。
hold_alt_keys() { # hold_alt_keys <hold_ms> <键…>
	local hold=$1
	shift
	local args=(-M alt)
	for k in "$@"; do args+=(-k "$k" -d 150); done
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
before_geo=$(win_geo)
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

# 浮层：面板开着的时候窗口一个都没动（不搬、不缩、不藏）
during_geo=$(win_geo)
if [ -n "$before_geo" ] && [ "$before_geo" = "$during_geo" ]; then
	pass "窗口一个都没动（浮层，不是把窗口摆进预览里）"
else
	fail "窗口被动了：$(diff <(echo "$before_geo") <(echo "$during_geo") | head -4)"
fi

# 一进来选中的是「最近用过的那扇」= 当前窗口 p5 之前的那扇 p4（**不是**屏幕顺序的
# 第一个 p1）—— 面板的顺序和选中的那一格是两件事
sel_title=$(mq 'next((e["title"] for e in d["entries"] if e["selected"]), "(无)")')
[ "$sel_title" = "p4" ] && pass "选中 = p4（最近用过的那扇，不是屏幕最左边那扇）" \
	|| fail "选中 = $sel_title（id $sel）"

# 面板顺序 = 屏幕顺序：entries 里窗口的 x 必须递增（滚出去的不在清单里，所以只
# 断言「顺序」，不断言「有几个」）
x_asc=$(mq "all(d['entries'][i]['x'] <= d['entries'][i+1]['x'] for i in range(len(d['entries']) - 1))")
[ "$x_asc" = "True" ] && pass "卡片从左到右按屏幕顺序排（$(mq '" ".join(e["title"] for e in d["entries"])')）" \
	|| fail "卡片顺序和屏幕对不上：$(mq "[(e['title'], e['x']) for e in d['entries']]")"

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

# 卡片：每项背后一张（超椭圆圆角的底 + 描边），比预览大一圈；相邻两张不重叠。
card_geo=$(mq "all(e['cw'] == e['w'] + 24 and e['ch'] == e['h'] + 24 and e['cx'] == e['x'] - 12 and e['cy'] == e['y'] - 12 for e in d['entries'])")
[ "$card_geo" = "True" ] && pass "卡片 = 预览 + 内边距（每边 12）" \
	|| fail "卡片几何不对：$(mq "[(e['title'], e['x'], e['y'], e['w'], e['h'], e['cx'], e['cy'], e['cw'], e['ch']) for e in d['entries']]")"
no_overlap=$(mq "all(a['cx'] + a['cw'] <= b['cx'] or b['cx'] + b['cw'] <= a['cx'] or a['cy'] + a['ch'] <= b['cy'] or b['cy'] + b['ch'] <= a['cy'] for i, a in enumerate(d['entries']) for b in d['entries'][i + 1:])")
[ "$no_overlap" = "True" ] && pass "相邻卡片不重叠" \
	|| fail "有卡片叠在一起了"
read -r sel_cx sel_cy sel_cw <<<"$(mj "next((e['cx'], e['cy'], e['cw']) for e in d['entries'] if e['selected'])")"
read -r uns_cx uns_cy uns_cw <<<"$(mj "next((e['cx'], e['cy'], e['cw']) for e in d['entries'] if not e['selected'])")"
# 卡片那条上边框：选中的那张 = 强调色（默认 #3daee9，描边 2px），别的那张 = 卡片
# 描边色（#3b4045，描边 1px —— 所以只取第一行，第二行已经是底了）。
sel_edge=$(pix "$RUNDIR/during.png" $((sel_cx + sel_cw / 2)) $((sel_cy + 1)))
uns_edge=$(pix "$RUNDIR/during.png" $((uns_cx + uns_cw / 2)) $((uns_cy)))
color_like "$sel_edge" "61 174 233" 14 \
	&& pass "选中的那张卡片描边是强调色（$sel_edge）" \
	|| fail "选中卡片描边颜色不对（$sel_edge，期望 61 174 233）"
color_like "$uns_edge" "59 64 69" 14 \
	&& pass "没选中的那张描边是卡片描边色（$uns_edge）" \
	|| fail "没选中卡片描边颜色不对（$uns_edge，期望 59 64 69）"

# 圆角：角上那段弧是**角节点**画的（直边矩形从 cy+12 / cx+12 才开始），所以量
# (cx, cy+6) 这一点 = 只有角节点能画的地方；角尖 (cx, cy) 必须是空的（被圆角切掉）。
# 之前角节点被误关掉过：弧没了、但直边还在，看着就是「缺角」—— 就靠这两条兜住。
sel_arc=$(pix "$RUNDIR/during.png" "$sel_cx" $((sel_cy + 6)))
color_like "$sel_arc" "61 174 233" 20 \
	&& pass "选中那张的圆角弧画出来了（$sel_arc）" \
	|| fail "选中那张圆角缺了（$sel_arc，期望强调色 61 174 233）"
uns_arc=$(pix "$RUNDIR/during.png" "$uns_cx" $((uns_cy + 6)))
color_like "$uns_arc" "59 64 69" 20 \
	&& pass "没选中那张的圆角弧画出来了（$uns_arc）" \
	|| fail "没选中那张圆角缺了（$uns_arc，期望 59 64 69）"
sel_tip=$(pix "$RUNDIR/during.png" "$sel_cx" "$sel_cy")
color_like "$sel_tip" "61 174 233" 20 || color_like "$sel_tip" "45 61 71" 20
if [ $? -ne 0 ]; then
	pass "角尖是切掉的（$sel_tip 既不是描边也不是底）"
else
	fail "角尖没切：$sel_tip 像描边或底，说明圆角是方的"
fi

# 缩略图：卡片里那块画的是**窗口内容**（面板自己画一份缩小版），而且没被压暗层盖
# 住（压暗层在卡片下面）。取左上象限那一点：四象限窗口的左上是红的。
read -r t1_x t1_y t1_w t1_h <<<"$(mj "next((e['x'], e['y'], e['w'], e['h']) for e in d['entries'] if e['selected'])")"
t1_px=$(pix "$RUNDIR/during.png" $((t1_x + t1_w / 4)) $((t1_y + t1_h * 2 / 5)))
read -r t1_r t1_g t1_b <<<"$t1_px"
if [ "$t1_r" -ge 200 ] || [ "$t1_g" -ge 200 ] || [ "$t1_b" -ge 200 ]; then
	pass "卡片里画的是窗口内容、而且是全亮的（$t1_px）"
else
	fail "卡片里没有缩略图 / 被压暗了（$t1_px，期望窗口那种纯色）"
fi

# 预览的形状 = 窗口的形状：预览走的是**装饰管线**（和屏幕上那扇窗口同一条），所以
# 角上是切掉的（角尖那点是卡片底，不是窗口内容）。直接摆客户端 buffer 的话角尖
# 就是内容本身 —— 这条正好把两条路分开。
prev_tip=$(pix "$RUNDIR/during.png" "$t1_x" "$t1_y")
prev_in=$(pix "$RUNDIR/during.png" $((t1_x + 6)) $((t1_y + 6)))
read -r pt_r pt_g pt_b <<<"$prev_tip"
read -r pi_r pi_g pi_b <<<"$prev_in"
if [ $((pt_r + pt_g + pt_b)) -lt 220 ] && [ $((pi_r + pi_g + pi_b)) -gt 300 ]; then
	pass "预览也是圆角（角尖 $prev_tip 是切掉的，里面 $prev_in 是窗口内容）"
else
	fail "预览角没切（角尖 $prev_tip / 里面 $prev_in）—— 可能退回方角那条路了"
fi

# 面板浮在窗口之上：卡片底下那些窗口被压暗层盖住了 —— 拿「窗口自己那块地儿、又在
# 面板外面」的一点量（工作区底边那条：卡片是竖着居中的，底下留着空）。
dim_x=$((wx + 30))
dim_y=$((wy + wh - 8))
b_sum=$(pix_sum "$before_png" "$dim_x" "$dim_y")
d_sum=$(pix_sum "$RUNDIR/during.png" "$dim_x" "$dim_y")
if [ "$b_sum" -gt 200 ] && [ "$d_sum" -lt $((b_sum * 7 / 10)) ]; then
	pass "窗口那块被压暗了（$b_sum -> $d_sum，面板压在窗口之上）"
else
	fail "窗口没被压暗（$b_sum -> $d_sum，量的是 $dim_x,$dim_y）—— 面板可能跑到窗口下面去了"
fi

# ---------------------------------------------------------------- 松开提交

mark "3) 松开 Alt：提交 + 焦点移动"
wait $WTYPE_PID 2>/dev/null
sleep 0.8
grep -aq "mru: 提交" "$LOG" && pass "日志：提交" || fail "日志里没有「提交」"
[ "$(focused_title)" = "p4" ] && pass "焦点给到选中的 p4" \
	|| fail "焦点是 $(focused_title)，不是 p4"
[ "$(mq 'd["showing"]')" = "False" ] && pass "面板关掉了" || fail "面板还开着"

mark "4) 连点 Tab：顺着屏幕顺序走（不是按使用时间）"
# 先收小一点，让 5 张卡片全都在屏里：不然「滚到选中的那张可见」会把左边/右边的
# 卡片挤出清单，清单长度就跟着选中项变（第 7 轮专门测那个滚动）
./build/vidowmsg set-config mru_max_scale 0.25 >/dev/null 2>&1
./build/vidowmsg set-config mru_max_height 200 >/dev/null 2>&1
hold_tabs 2 2500
sleep 0.7
# 面板顺序 = **屏幕顺序**（开窗顺序就是从左到右）：p1 p2 p3 p4 p5
order=$(mq '" ".join(e["title"] for e in d["entries"])')
[ "$order" = "p1 p2 p3 p4 p5" ] && pass "面板顺序 = 屏幕顺序（$order）" \
	|| fail "面板顺序不是屏幕顺序：$order"
# 初始选中的还是「最近用过的那扇」= p5（清单最后一格，不是第一格 p1），再按一下
# Tab = 往右一格 → 到头绕回第一格 p1
sel2=$(mq 'next((e["title"] for e in d["entries"] if e["selected"]), "(无)")')
[ "$sel2" = "p1" ] && pass "初始选中 p5（最近用过的那扇）→ 再走一格绕回 p1" \
	|| fail "两次 Tab 之后选中 = $sel2（期望 p1：p5 是清单最后一格）"
wait $WTYPE_PID 2>/dev/null
sleep 0.8
[ "$(focused_title)" = "p1" ] && pass "松开后焦点 = p1" || fail "松开后焦点 = $(focused_title)"

mark "5) Esc 取消：焦点原地不动"
hold_tabs 1 2000
# Esc 在松开 Alt 之前按下去（另起一个虚拟键盘按 Escape）
sleep 0.6
wtype -k Escape >/dev/null 2>&1
sleep 0.8
grep -aq "mru: 取消" "$LOG" && pass "日志：取消" || fail "日志里没有「取消」"
wait $WTYPE_PID 2>/dev/null
sleep 0.6
[ "$(focused_title)" = "p1" ] && pass "焦点还是 p1（没被取消那一下搬走）" \
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

mark "8) 鼠标悬停：点亮底下那一张，选中项不动"
# 收小一点，让条子上下留出空白（好试「挪到空白处」）
./build/vidowmsg set-config mru_max_scale 0.5 >/dev/null 2>&1
./build/vidowmsg set-config mru_max_height 300 >/dev/null 2>&1
hold_tabs 1 3000
sleep 0.7
sel_before=$(mq 'd["selected"]')
read -r hv_x hv_y hv_id <<<"$(mj "next((e['cx'] + e['cw'] // 2, e['cy'] + e['ch'] // 2, e['id']) for e in d['entries'] if not e['selected'])")"
msg move-cursor "$hv_x" "$hv_y"
sleep 0.4
[ "$(mq 'd["hover"]')" = "$hv_id" ] && pass "悬停点亮指针底下那张（#$hv_id）" \
	|| fail "悬停 = $(mq 'd["hover"]')，期望 #$hv_id"
[ "$(mq 'd["selected"]')" = "$sel_before" ] && pass "选中项没被鼠标顶掉（还是 #$sel_before）" \
	|| fail "选中项被鼠标改成 $(mq 'd["selected"]') 了"
[ "$(mq "next((e['title'] for e in d['entries'] if e['hover']), '(无)')")" = "$(mq "next((e['title'] for e in d['entries'] if e['id'] == $hv_id), '(无)')")" ] \
	&& pass "悬停的就是命中的那一项" || fail "悬停项和命中项对不上"
# 悬停的描边：默认和选中那张**同宽**（2px），但颜色是强调色半透明 —— 所以比选中
# 那张淡。两条都量像素（描边色是可配的，默认值就写在这儿）。
grim "$RUNDIR/hover.png" >/dev/null 2>&1
read -r hx hy hw <<<"$(mj "next((e['cx'], e['cy'], e['cw']) for e in d['entries'] if e['hover'])")"
read -r sx2 sy2 sw2 <<<"$(mj "next((e['cx'], e['cy'], e['cw']) for e in d['entries'] if e['selected'])")"
hv_edge=$(pix "$RUNDIR/hover.png" $((hx + hw / 2)) $((hy + 1)))
sel_edge2=$(pix "$RUNDIR/hover.png" $((sx2 + sw2 / 2)) $((sy2 + 1)))
color_like "$hv_edge" "53 117 152" 30 \
	&& pass "悬停那张的描边点亮了（$hv_edge）" \
	|| fail "悬停描边颜色不对（$hv_edge，期望 ≈ 53 117 152）"
read -r he_r he_g he_b <<<"$hv_edge"
read -r se_r se_g se_b <<<"$sel_edge2"
if [ "$he_g" -lt "$se_g" ] && [ "$he_r" -lt "$se_r" ]; then
	pass "悬停的描边比选中的淡（悬停 $hv_edge < 选中 $sel_edge2）"
else
	fail "悬停描边没比选中淡（$hv_edge vs $sel_edge2）"
fi

# 挪到卡片外面（条子上方的空白）→ 悬停清掉
msg move-cursor $((wx + ww / 2)) $((wy + 4))
sleep 0.4
[ "$(mq 'd["hover"]')" = "0" ] && pass "挪到空白处 → 悬停清掉" \
	|| fail "挪开了还亮着（hover = $(mq 'd["hover"]')）"
[ "$(mq 'd["selected"]')" = "$sel_before" ] && pass "选中项还是没动" \
	|| fail "选中项变成 $(mq 'd["selected"]') 了"
wait $WTYPE_PID 2>/dev/null
sleep 0.8

mark "9) 鼠标点击：点哪张卡片就切到哪个窗口"
hold_tabs 1 4000
sleep 0.7
read -r ck_x ck_y ck_title <<<"$(mj "next((e['cx'] + e['cw'] // 2, e['cy'] + e['ch'] // 2, e['title']) for e in d['entries'] if not e['selected'])")"
msg move-cursor "$ck_x" "$ck_y"
sleep 0.3
msg pointer-button press left
sleep 0.5
msg pointer-button release left
sleep 0.8
grep -aq "mru: 点选 $ck_title" "$LOG" && pass "日志：点选 $ck_title" \
	|| fail "日志里没有「点选 $ck_title」"
[ "$(focused_title)" = "$ck_title" ] && pass "点完焦点 = $ck_title" \
	|| fail "点完焦点 = $(focused_title)，期望 $ck_title"
[ "$(mq 'd["showing"]')" = "False" ] && pass "点完面板就收了" || fail "点完面板还开着"
wait $WTYPE_PID 2>/dev/null
sleep 0.8
[ "$(focused_title)" = "$ck_title" ] && pass "松开 Alt 之后焦点没被搬走" \
	|| fail "松开 Alt 之后焦点变成 $(focused_title) 了"
./build/vidowmsg state >/dev/null 2>&1 && pass "vidow 存活（松手被吃掉，没漏给客户端）" \
	|| fail "vidow 挂了"

mark "10) grid：换行铺在屏幕里，装不下就整体缩小"
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

# ---------------------------------------------------------------- 工作区范围

mark "11) 工作区范围：默认只看当前工作区（mru_scope = workspace）"
# 在第 2 块工作区开一个 p6：默认范围下它不该进清单
./build/vidowmsg focus-workspace 2 >/dev/null 2>&1
sleep 0.6
python3 tools/pattern.py p6 >/dev/null 2>&1 &
sleep 1.6
./build/vidowmsg focus-workspace 1 >/dev/null 2>&1
sleep 0.8
hold_tabs 1 2500
sleep 0.7
scope=$(mq 'd["scope"]')
cnt=$(mq 'd["count"]')
[ "$scope" = "workspace" ] && pass "默认范围 = workspace" || fail "默认范围 = $scope"
[ "$cnt" = "5" ] && pass "只看当前工作区：清单 5 个（不含别的区的 p6）" \
	|| fail "清单 $cnt 个（期望 5）：$(mq '" ".join(e["title"] for e in d["entries"])')"
wait $WTYPE_PID 2>/dev/null
sleep 0.6
# 换成 all：别的区的 p6 也进来
./build/vidowmsg set-config mru_scope all >/dev/null 2>&1
hold_tabs 1 2500
sleep 0.7
cnt=$(mq 'd["count"]')
[ "$cnt" = "6" ] && pass "scope = all：清单 6 个（p6 也进来）" \
	|| fail "清单 $cnt 个（期望 6）"
wait $WTYPE_PID 2>/dev/null
sleep 0.6
./build/vidowmsg set-config mru_scope workspace >/dev/null 2>&1

# ------------------------------------------------- 滚轮 / 模态（A1~A4、B1~B5）

# 选中项在清单里的**下标**（entries 是屏幕顺序，和 Tab 的步进同一个顺序）
sel_idx() { mq "[e['id'] for e in d['entries']].index(d['selected'])"; }
ent_n() { mq "len(d['entries'])"; }

mark "12) A1/A2 滚轮 = 选中项 ±1；到边停住，不循环"
# 收小一点：5 张卡片全在清单里（不然选中项一动排版跟着滚、清单长度也变，下标对不上）
./build/vidowmsg set-config mru_max_scale 0.25 >/dev/null 2>&1
./build/vidowmsg set-config mru_max_height 200 >/dev/null 2>&1
./build/vidowmsg set-config mru_layout strip >/dev/null 2>&1
hold_tabs 1 14000
sleep 0.7
ni_0=$(sel_idx)
nn=$(ent_n)
geo_0=$(win_geo)
nlog_0=$(grep -ac "mru: 滚轮 → #" "$LOG")
# 往下滚一格 = 往后一项（和连点 Tab / 面板里往右一格同一个方向）
msg pointer-scroll 0 1
sleep 0.4
ni_1=$(sel_idx)
[ "$ni_1" = "$((ni_0 + 1))" ] \
	&& pass "往下滚一格：选中项下标 $ni_0 → $ni_1（+1）" \
	|| fail "往下滚一格没 +1：$ni_0 → $ni_1"
nlog_1=$(grep -ac "mru: 滚轮 → #" "$LOG")
[ "$nlog_1" -gt "$nlog_0" ] \
	&& pass "日志：mru: 滚轮 → #（$nlog_0 → $nlog_1）" \
	|| fail "日志里没有「mru: 滚轮 → #」"
# 往上滚一格 = 往前一项（回到刚才那格）
msg pointer-scroll 0 -1
sleep 0.4
[ "$(sel_idx)" = "$ni_0" ] && pass "往上滚一格：回到下标 $ni_0" \
	|| fail "往上滚一格没回到 $ni_0（$(sel_idx)）"

# 到第一个：连发足够多次一定贴到 0；再往上滚两次必须**不动**（不绕回最后一格）
for _ in $(seq 1 $((nn + 1))); do msg pointer-scroll 0 -1; sleep 0.12; done
sleep 0.3
[ "$(sel_idx)" = "0" ] && pass "往上滚到头 = 清单第一格" \
	|| fail "往上滚到头停在 $(sel_idx)（期望 0）"
edge_0=$(grep -ac "mru: 滚轮到边" "$LOG")
msg pointer-scroll 0 -1; sleep 0.2
msg pointer-scroll 0 -1; sleep 0.3
[ "$(sel_idx)" = "0" ] && pass "到第一格再往上滚：不动（没绕回最后一格）" \
	|| fail "到第一格还往上滚，选中项跑到 $(sel_idx) 了"
edge_1=$(grep -ac "mru: 滚轮到边" "$LOG")
[ "$edge_1" = "$((edge_0 + 2))" ] \
	&& pass "日志：到边两次（mru: 滚轮到边，$edge_0 → $edge_1）" \
	|| fail "「滚轮到边」日志对不上：$edge_0 → $edge_1（期望 +2）"
# 到最后一格同理
for _ in $(seq 1 $((nn + 1))); do msg pointer-scroll 0 1; sleep 0.12; done
sleep 0.3
[ "$(sel_idx)" = "$((nn - 1))" ] \
	&& pass "往下滚到头 = 清单最后一格（$((nn - 1))）" \
	|| fail "往下滚到头停在 $(sel_idx)（期望 $((nn - 1))）"
edge_2=$(grep -ac "mru: 滚轮到边" "$LOG")
msg pointer-scroll 0 1; sleep 0.3
[ "$(sel_idx)" = "$((nn - 1))" ] && pass "到最后一格再往下滚：不动（没绕回第一格）" \
	|| fail "到最后一格还往下滚，选中项跑到 $(sel_idx) 了"
edge_3=$(grep -ac "mru: 滚轮到边" "$LOG")
[ "$edge_3" = "$((edge_2 + 1))" ] && pass "日志：又到边一次（$edge_2 → $edge_3）" \
	|| fail "「滚轮到边」日志对不上：$edge_2 → $edge_3"
# A4：滚轮一个字都没喂给客户端（窗口几何逐字段不变 = 面板只是换了个选中项）
geo_1=$(win_geo)
[ "$geo_0" = "$geo_1" ] && pass "滚轮全程窗口一个都没动（也没喂客户端）" \
	|| fail "有窗口被滚轮动了：$(diff <(echo "$geo_0") <(echo "$geo_1") | head -4)"
wait $WTYPE_PID 2>/dev/null
sleep 0.8

mark "13) A3 滚轮不换工作区 / 不动布局（Alt+滚轮在默认配置里 = 换工作区）"
# 先站定第 1 块工作区：往下滚**本来**就是「换到下一块」（改前真的会跳）
./build/vidowmsg focus-workspace 1 >/dev/null 2>&1
sleep 0.6
hold_tabs 1 4000
sleep 0.7
f_0=$(focused_title)
win_0=$(./build/vidowmsg windows 2>/dev/null)
ws_0=$(./build/vidowmsg workspaces 2>/dev/null)
msg pointer-scroll 0 1; sleep 0.5
msg pointer-scroll 0 -1; sleep 0.5
msg pointer-scroll 0 1; sleep 0.5
win_1=$(./build/vidowmsg windows 2>/dev/null)
ws_1=$(./build/vidowmsg workspaces 2>/dev/null)
if [ -n "$ws_0" ] && [ "$ws_0" = "$ws_1" ]; then
	pass "滚动前后 workspaces 逐字不变（工作区没跳 —— Alt+wheel = workspace up/down 没跑）"
else
	fail "工作区真的跳了（Alt+wheel = workspace up/down 跑了）：$(diff <(echo "$ws_0") <(echo "$ws_1") | head -6)"
fi
if [ -n "$win_0" ] && [ "$win_0" = "$win_1" ]; then
	pass "滚动前后 windows 逐字不变（窗口没动 / 没换屏 / 焦点没搬）"
else
	fail "windows 变了：$(diff <(echo "$win_0") <(echo "$win_1") | head -6)"
fi
[ "$(focused_title)" = "$f_0" ] && pass "焦点也没动（还是 $f_0）" \
	|| fail "焦点被滚轮搬走了：$f_0 → $(focused_title)"
wait $WTYPE_PID 2>/dev/null
sleep 0.8
# 改前这一条真的会换工作区：把现场收回工作区 1，后面的判据才算数
./build/vidowmsg focus-workspace 1 >/dev/null 2>&1
sleep 0.6

mark "14) B3 Mod+滚轮也不换列 / 不换工作区"
hold_alt_tab_mod 5000 logo
sleep 0.7
[ "$(mq 'd["active"]')" = "True" ] && pass "切换器开着 + Mod 也按着（active=true）" \
	|| fail "切换器没开着（active=$(mq 'd["active"]')）"
f_0=$(focused_title)
col_0=$(grep -ac "action: focus-column" "$LOG")
geo_0=$(win_geo)
msg pointer-scroll 0 1; sleep 0.25
msg pointer-scroll 0 -1; sleep 0.25
col_1=$(grep -ac "action: focus-column" "$LOG")
geo_1=$(win_geo)
[ "$f_0" = "$(focused_title)" ] && pass "焦点没动（Mod+滚轮没换列：还是 $f_0）" \
	|| fail "焦点被换列动作挪走了：$f_0 → $(focused_title)"
[ "$col_1" = "$col_0" ] && pass "日志里没有新的 focus-column 动作行（$col_0 → $col_1）" \
	|| fail "focus-column 动作真的跑了：$col_0 → $col_1"
[ "$geo_0" = "$geo_1" ] && pass "窗口 / 列一字不变" \
	|| fail "布局变了：$(diff <(echo "$geo_0") <(echo "$geo_1") | head -4)"
wait $WTYPE_PID 2>/dev/null
sleep 0.8

mark "15) B1 面板按着时字母键进不了客户端（wtype，真键盘那条路）"
cat > "$RUNDIR/keyprobe.py" <<'PY'
#!/usr/bin/env python3
# 客户端探针：把自己收到的按键 / 鼠标按下打出来（一行一条）。「面板开着时输入是
# 模态的」就拿它当判据 —— 客户端收到了 = 漏了。照 test-mini.sh 那个探针的形状。
import sys
import time
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk  # noqa: E402


def log(m):
    print("%s %.3f" % (m, time.time()), flush=True)


class Probe(Gtk.Window):
    def __init__(self):
        super().__init__(title=sys.argv[1] if len(sys.argv) > 1 else 'keyprobe')
        self.set_default_size(320, 240)
        self.add_events(Gdk.EventMask.KEY_PRESS_MASK |
                        Gdk.EventMask.BUTTON_PRESS_MASK |
                        Gdk.EventMask.BUTTON_RELEASE_MASK |
                        Gdk.EventMask.SCROLL_MASK)
        self.connect('key-press-event', lambda w, e: log("key"))
        self.connect('button-press-event', lambda w, e: log("press"))
        self.connect('button-release-event', lambda w, e: log("release"))
        self.connect('scroll-event', lambda w, e: log("scroll"))
        self.connect('destroy', Gtk.main_quit)


Probe().show_all()
Gtk.main()
PY
KEYLOG="$RUNDIR/keyprobe.log"
rm -f "$KEYLOG"
python3 "$RUNDIR/keyprobe.py" keyprobe > "$KEYLOG" 2>&1 &
KEYPROBE_PID=$!
sleep 1.8
kp_id=$(./build/vidowmsg windows 2>/dev/null | python3 -c "
import json,sys
for w in json.load(sys.stdin):
    if w.get('title') == 'keyprobe':
        print(w['id']); break
" 2>/dev/null)
[ -n "$kp_id" ] && pass "探针窗口起来了（id $kp_id）" || fail "探针窗口没起来"
./build/vidowmsg focus-window --id "$kp_id" >/dev/null 2>&1
sleep 0.5
# 对照组：没有切换器的时候按键进得来（证明探针是活的）
wtype -k a >/dev/null 2>&1
sleep 0.5
k_a=$(grep -ac "^key" "$KEYLOG")
[ "$k_a" -ge 1 ] && pass "对照组：没有切换器时按键进得来（$k_a 次）" \
	|| fail "探针压根收不到按键（探针不活 / 焦点不在它身上）"
# 切换器按着（面板开着）：同一个键必须进不去
hold_tabs 1 6000
sleep 0.7
wtype -k a >/dev/null 2>&1
sleep 0.6
k_b=$(grep -ac "^key" "$KEYLOG")
[ "$k_b" = "$k_a" ] && pass "切换器按着时按键**进不去**（还是 $k_b 次）" \
	|| fail "按键漏给客户端了：$k_a → $k_b"
# 收尾：Esc 取消（焦点原地不动）→ 松开 Alt
wtype -k Escape >/dev/null 2>&1
sleep 0.4
wait $WTYPE_PID 2>/dev/null
sleep 0.8
[ "$(focused_title)" = "keyprobe" ] && pass "取消之后焦点没动（还是探针）" \
	|| fail "焦点变成 $(focused_title) 了"

mark "16) B2 open_delay 期点击不穿透（模态从按下 Alt 那一刻算起）"
./build/vidowmsg set-config mru_open_delay_ms 3000 >/dev/null 2>&1
read -r kp_x kp_y <<<"$(./build/vidowmsg windows 2>/dev/null | python3 -c "
import json,sys
for w in json.load(sys.stdin):
    if w.get('title') == 'keyprobe':
        g = w['geometry']
        print(g['x'] + g['width'] // 2, g['y'] + g['height'] // 2); break
" 2>/dev/null)"
[ -n "${kp_x:-}" ] && pass "拿到探针窗口中心（$kp_x,$kp_y）" || fail "算不出探针窗口中心"
msg move-cursor "$kp_x" "$kp_y"
sleep 0.4
press_0=$(grep -ac "^press" "$KEYLOG")
geo_0=$(win_geo)
# 按住 Alt，**等切换器接管（active=true）就点** —— 这时面板还没显形（open_delay
# 3000ms）。等 active 是必须的：另起的 wtype 要几十毫秒才连上，不等的话这一下点在
# 「Alt 还没按下去」的时候，本来就该进客户端（那不是穿透，是测量点错了）。
hold_tabs 1 9000
for _ in $(seq 1 80); do
	[ "$(mq 'd["active"]')" = "True" ] && break
	sleep 0.05
done
act=$(mq 'd["active"]')
[ "$act" = "True" ] && pass "切换器已经接管（active=true，面板还没显形）" \
	|| fail "切换器没接管（active=$act）"
msg pointer-button press left
msg pointer-button release left
sleep 0.3
act=$(mq 'd["active"]')
shw=$(mq 'd["showing"]')
[ "$act" = "True" ] && [ "$shw" = "False" ] \
	&& pass "点的时候面板还没显形（active=true / showing=false）" \
	|| fail "时间点不对：active=$act showing=$shw（open_delay 没生效？）"
press_1=$(grep -ac "^press" "$KEYLOG")
[ "$press_1" = "$press_0" ] && pass "open_delay 期的按下没漏给客户端（还是 $press_1 次）" \
	|| fail "点击穿透进客户端了：$press_0 → $press_1"
[ "$(focused_title)" = "keyprobe" ] && pass "点击没把焦点搬走" \
	|| fail "焦点变成 $(focused_title) 了"
[ "$geo_0" = "$(win_geo)" ] && pass "窗口一个都没动（没有 interactive / drag）" \
	|| fail "窗口被动了：$(diff <(echo "$geo_0") <(echo "$(win_geo)") | head -4)"
# 面板显形之后：点空白 = 既有行为不变（vidow_mru_click 那条路），客户端也一个字没收到
sleep 3.0
[ "$(mq 'd["showing"]')" = "True" ] && pass "等一会儿面板显形了（open_delay 到点）" \
	|| fail "面板没显形（showing = $(mq 'd["showing"]')）"
msg move-cursor $((wx + ww / 2)) $((wy + 4))
sleep 0.3
press_2=$(grep -ac "^press" "$KEYLOG")
blank_0=$(grep -ac "mru: 点在空白处" "$LOG")
msg pointer-button press left
msg pointer-button release left
sleep 0.4
blank_1=$(grep -ac "mru: 点在空白处" "$LOG")
[ "$blank_1" = "$((blank_0 + 1))" ] \
	&& pass "面板显形后点空白：既有行为不变（日志 mru: 点在空白处（窗口不动））" \
	|| fail "点空白没走到切换器（$blank_0 → $blank_1）"
[ "$(grep -ac '^press' "$KEYLOG")" = "$press_2" ] \
	&& pass "点空白也没漏给客户端（还是 $press_2 次）" \
	|| fail "点空白穿透进客户端了：$press_2 → $(grep -ac '^press' "$KEYLOG")"
[ "$(focused_title)" = "keyprobe" ] && pass "点空白没换焦点" \
	|| fail "点空白把焦点换了（$(focused_title)）"
wtype -k Escape >/dev/null 2>&1
sleep 0.4
wait $WTYPE_PID 2>/dev/null
sleep 0.8
./build/vidowmsg set-config mru_open_delay_ms 0 >/dev/null 2>&1

mark "17) B4 切换器自己的键照常：方向键 / Return / 点卡片"
hold_tabs 1 12000
sleep 0.7
i_0=$(sel_idx)
wtype -k Right >/dev/null 2>&1
sleep 0.4
i_1=$(sel_idx)
[ "$i_1" = "$((i_0 + 1))" ] && pass "方向键 Right：选中项 +1（$i_0 → $i_1）" \
	|| fail "Right 没让选中项 +1：$i_0 → $i_1"
wtype -k Up >/dev/null 2>&1
sleep 0.4
[ "$(sel_idx)" = "$i_0" ] && pass "方向键 Up：往回一格（$i_1 → $i_0）" \
	|| fail "Up 没让选中项 -1：$i_1 → $(sel_idx)"
n_ent=$(ent_n)
for _ in $(seq 1 $((n_ent + 1))); do wtype -k Left >/dev/null 2>&1; sleep 0.12; done
sleep 0.3
[ "$(sel_idx)" = "0" ] && pass "方向键 Left 连按到头 = 第一格" \
	|| fail "Left 连按停在 $(sel_idx)（期望 0）"
wtype -k Left >/dev/null 2>&1
sleep 0.3
[ "$(sel_idx)" = "0" ] && pass "到第一格再按 Left：不动（和滚轮同一套 clamp）" \
	|| fail "Left 绕回到 $(sel_idx) 了"
# 点卡片中心：直接切过去（不用等松修饰键）—— 和改前同一条路
read -r ck2_x ck2_y ck2_title <<<"$(mj "next((e['cx'] + e['cw'] // 2, e['cy'] + e['ch'] // 2, e['title']) for e in d['entries'] if not e['selected'])")"
msg move-cursor "$ck2_x" "$ck2_y"
sleep 0.3
msg pointer-button press left
sleep 0.4
msg pointer-button release left
sleep 0.8
grep -aq "mru: 点选 $ck2_title" "$LOG" && pass "点卡片中心：日志 mru: 点选 $ck2_title" \
	|| fail "日志里没有「点选 $ck2_title」"
[ "$(focused_title)" = "$ck2_title" ] && pass "点完焦点 = $ck2_title" \
	|| fail "点完焦点 = $(focused_title)，期望 $ck2_title"
[ "$(mq 'd["showing"]')" = "False" ] && pass "点完面板收了" || fail "点完面板还开着"
wait $WTYPE_PID 2>/dev/null
sleep 0.8
[ "$(focused_title)" = "$ck2_title" ] && pass "松开 Alt 之后焦点没被搬走" \
	|| fail "松开 Alt 之后焦点变成 $(focused_title) 了"

mark "18) B5 别的键位被挡（日志 + 动作真的没发生）"
# 先把焦点放到**最右边**那扇（探针窗口）：Alt+j = focus-column left 才有地方可走
# （焦点压在最左边那列时，「往左一列」是空转，分不清「被挡」和「到头」）
./build/vidowmsg focus-window --id "$kp_id" >/dev/null 2>&1
sleep 0.5
blk_0=$(grep -ac "mru: 绑定 .*切换器开着期间不生效" "$LOG")
# 同一个 wtype 里：Alt 按下 → Tab（开切换器）→ j（测试配置里 Alt+j = focus-column left）
hold_alt_keys 8000 Tab j
sleep 0.7
f_0=$(focused_title)
sleep 0.5
[ "$(focused_title)" = "$f_0" ] && pass "焦点没动（Alt+j 的动作没跑，还是 $f_0）" \
	|| fail "动作真的跑了：$f_0 → $(focused_title)"
blk_1=$(grep -ac "mru: 绑定 .*切换器开着期间不生效" "$LOG")
[ "$blk_1" -gt "$blk_0" ] \
	&& pass "日志：绑定被白名单挡住（$blk_0 → $blk_1）" \
	|| fail "日志里没有「切换器开着期间不生效」"
grep -a "mru: 绑定" "$LOG" | tail -1 | sed 's/^/       /'
wait $WTYPE_PID 2>/dev/null
sleep 0.8
# 白名单里的那条照常生效（Alt+Tab 连点走的就是它：闸没把 next-window 误伤）
hold_alt_keys 6000 Tab Tab
sleep 0.7
[ "$(mq 'd["active"]')" = "True" ] && pass "切换器开着（两次 Alt+Tab 之后 active=true）" \
	|| fail "切换器没开着（active=$(mq 'd["active"]')）"
grep -aq "mru: 走到 #" "$LOG" && pass "日志：走了格子（mru: 走到 #，白名单里的动作照常跑）" \
	|| fail "白名单没放行 next-window（日志里没有「mru: 走到 #」）"
wtype -k Escape >/dev/null 2>&1
sleep 0.4
wait $WTYPE_PID 2>/dev/null
sleep 0.8

mark "19) 收尾"
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
