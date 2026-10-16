#!/bin/bash
# niri 风格的 overview：Mod+O 把这块屏上的整条列带缩小铺满屏幕看一眼全景，
# 点谁选谁，Esc（或再按一次 Mod+O）缩回去。
#
# 判据（日志 + 像素）：
#   1) 进入：`overview: 进入（… N 个窗口，占 WxH 缩到 S，铺在工作区 …）`，
#      每个窗口一行「窗口 X -> 屏幕 x,y wxh（缩放 S）」，缩放 < 1 且所有矩形都在屏内
#   2) 缩放是对的：overview 里的窗口尺寸 = 原尺寸 × S（±2px），窗口之间间距同样按 S 缩
#   3) 窗口真的画在那些位置：按日志坐标采样四象限颜色（红绿蓝白），而且是**没被压暗**的
#   4) 背景被压暗了（壁纸那块比正常时暗），窗口本身不暗（遮罩夹在壁纸和窗口之间 ——
#      这里踩过一次：遮罩层建了两次跑到窗口上面去了）
#   5) Esc 退出：日志 `overview: 退出`，画面**逐像素**回到进 overview 之前那张
#   6) 点选：点某个窗口 → `overview: 选中 X` + 退出 + 焦点换成它（那一列被滚进视野）
#   7) 两块屏时只看当前那块：另一块屏上的窗口淡出（那片区域不是 pattern 的颜色）
#   8) 退出不反弹：Esc 之后 t 单调从 1 降到 0（动画引擎的 t 永远 0→1，方向得由
#      on 决定；以前直接把 t 当进度用，退出会把「进入」重放一遍再瞬间跳回去）
#   9) 遮罩颜色可配（overview_dim_color）：默认压暗、alpha 0 = 不压暗、换色就染色，
#      三种配色下窗口本身都不暗
#  10) 全景里的壁纸模糊（overview_blur）：开了之后壁纸的硬边摊开（跨边缘最大跳变
#      掉到 1/4 以下），窗口本身不跟着糊
#  11) 全景用另一张壁纸（图层规则 overview = "only"）：namespace 匹配上的那块
#      background 层 surface 桌面上一点不画、进全景才露出来、退出又收起；规则热改
#      （only ↔ off）当场生效；vidowmsg background / layers 如实报出它
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-overview-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
VIDOW_PIDS=""

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
	rm -rf "$RUNDIR"
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

command -v grim >/dev/null || { echo "test-overview.sh: 需要 grim"; exit 1; }
command -v wtype >/dev/null || { echo "test-overview.sh: 需要 wtype"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-overview.sh: 需要 python3-gobject"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 150
window_close_ms = 150
window_move_ms = 150
-- 键位全是配置给的（合成器不内置）：这个测试按 Mod+O 进全景，就自己绑上
vidow.bind("Mod+O", "toggle-overview")
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

start_vidow() { # start_vidow <outputs>
	export WLR_HEADLESS_OUTPUTS=$1
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	VIDOW_PIDS="$VIDOW_PIDS $VIDOW_PGID"
	for _ in $(seq 1 40); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
}

# 光标移到某块屏中间（归一化 0..10000，按整个布局宽算）
point_at() { # point_at <layout_x> <layout_w> <layout_h>
	local nx=$(( $1 * 10000 / $2 ))
	./build/vdrag "$nx" 5000 "$nx" 5000 >/dev/null 2>&1
	sleep 0.3
}

# ---------------------------------------------------------------- 单屏
mark "单屏：3 个窗口 + 壁纸"
start_vidow 1
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
for n in p-a p-b p-c; do
	python3 tools/pattern.py "$n" >/dev/null 2>&1 &
	sleep 0.7
done
sleep 1.2
grep -aq "place p-" "$LOG" || { fail "三个窗口没摆上"; exit 1; }

grim "$RUNDIR/normal.png"
SNAP=$RUNDIR/normal.txt
grep -aE "place p-[abc] -> " "$LOG" > "$SNAP"

wtype -M logo -k o >/dev/null 2>&1
sleep 0.9
grim "$RUNDIR/overview.png"

wtype -k Escape >/dev/null 2>&1
sleep 0.9
grim "$RUNDIR/back.png"

# 再进一次，点最左边那个窗口
wtype -M logo -k o >/dev/null 2>&1
sleep 0.9
read -r CX CY CW CH <<<"$(grep -a "overview: 窗口" "$LOG" | tail -3 |
	sed 's/.*屏幕 //; s/（.*//; s/,/ /; s/x/ /' | sort -k1 -n | head -1)"
echo "  点选目标：$CX,$CY ${CW}x${CH}"
NX=$(( (CX + CW / 2) * 10000 / 1280 ))
NY=$(( (CY + CH / 2) * 10000 / 720 ))
./build/vdrag "$NX" "$NY" "$NX" "$NY" >/dev/null 2>&1
sleep 1.2
grim "$RUNDIR/picked.png"
grep -aE "place p-[abc] -> " "$LOG" > "$RUNDIR/picked.txt"
grep -aE "place p-[abc] -> " "$LOG" > "$RUNDIR/picked.txt"

cp "$LOG" "$RUNDIR/log-single.txt"
stop_clients
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6


# ---------------------------------------------------- 迷你 / 图标态 + 命中盒（A + B）
# A：全景里 mini / 图标态的窗口**点得中**。以前这里是 vidow_focus_view()：它开头就把
#    MINI / ICON 挡回去了（不变量：focused 永远不指小窗），于是点小窗只有一行「选中」
#    日志、焦点一点没动。现在和 Alt+Tab 提交 / IPC focus-window 走同一个入口
#    （vidow_focus_view_expand_mini：先摊成普通窗口再给焦点）。
# B：**命中盒必须和画出来的格子一致**。命中走 wlroots 的场景图：盒子 = 内容节点的
#    dst_width/dst_height（scene_node_get_size），再被表面尺寸卡一遍边界。全景里内容
#    节点的尺寸/偏移没跟着行缩放走的时候，一格能吞掉整排（实测 3 列时最后一个窗口的
#    盒子是 646..1291，而它的格子只到 934），用户看到的就是「点谁都不是谁」。
mark "全景里点小窗（A）+ 命中盒和画出来的格子一致（B）"
start_vidow 1
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.9
for n in wa wb wm; do
	python3 tools/pattern.py "$n" >/dev/null 2>&1 &
	sleep 0.8
done
sleep 1.2

wid() {
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if w["title"] == sys.argv[1]:
        print(w["id"]); break' "$1"
}
wfield() {
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if w["title"] == sys.argv[1]:
        print(w.get(sys.argv[2])); break' "$1" "$2"
}
wbox() { # 小窗那块盒子：迷你态在 float_geometry，图标态在 geometry
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json,sys
for w in json.load(sys.stdin):
    if w["title"] == sys.argv[1]:
        g = w.get("float_geometry") or w.get("geometry") or {}
        print(g.get("x",0), g.get("y",0), g.get("width",0), g.get("height",0)); break' "$1"
}
what() { # <x> <y> → 命中的窗口名（没命中就是空）
	./build/vidowmsg pick-window "$1" "$2" 2>/dev/null | python3 -c '
import json,sys
print((json.load(sys.stdin).get("window") or {}).get("title", ""))'
}
click_at() { # <x> <y>：光标挪过去按下再松开（全景里 = 点选）
	local nx=$(( $1 * 10000 / 1280 )) ny=$(( $2 * 10000 / 720 ))
	./build/vdrag "$nx" "$ny" "$nx" "$ny" >/dev/null 2>&1
}
ov_rect() {
	grep -a "overview: 窗口 $1 -> 屏幕 " "$LOG" | tail -1 |
		sed 's/.*屏幕 //; s/（.*//; s/,/ /; s/x/ /'
}
check_cell() { # <title> <x> <y> <说明>
	local got
	got=$(what "$2" "$3")
	[ "$got" = "$1" ] && pass "$4（$1 @ $2,$3）" ||
		fail "$4：$2,$3 命中「$got」（应该是 $1）"
}
check_picked() { # <title> <说明> [expand]：选中日志 + 缩回去 + 焦点换成它
	local f mini
	grep -aq "overview: 选中 $1" "$LOG" && pass "$2：日志记了「选中 $1」" ||
		fail "$2：日志里没有「overview: 选中 $1」"
	awk -v t="$1" '$0 ~ ("overview: 选中 " t) {seen=1}
		seen && /overview: 退出/ {found=1} END {exit !found}' "$LOG" &&
		pass "$2：选中之后就缩回去了" || fail "$2：选中之后没退出 overview"
	f=$(wfield "$1" focused)
	[ "$f" = "True" ] && pass "$2：焦点真的换成 $1 了" ||
		fail "$2：焦点没换（$1 focused=$f）"
	[ "${3:-}" = "expand" ] || return 0
	mini=$(wfield "$1" mini)
	case "$mini" in
	none | free) pass "$2：小窗摊成普通窗口了（mini=$mini）" ;;
	*) fail "$2：还是小窗态（mini=$mini），没展开" ;;
	esac
}

# wm 变迷你态、焦点留给 wa（这样「点 wm 换焦点」才有对照）
./build/vidowmsg focus-window --id "$(wid wm)" >/dev/null 2>&1
sleep 0.4
./build/vidowmsg set-mini-state --id "$(wid wm)" --state mini >/dev/null 2>&1
sleep 0.8
./build/vidowmsg focus-window --id "$(wid wa)" >/dev/null 2>&1
sleep 0.5
[ "$(wfield wm mini)" = "mini" ] && pass "wm 进了迷你态、焦点在 wa" ||
	fail "wm 没进迷你态（mini=$(wfield wm mini)）"

wtype -M logo -k o >/dev/null 2>&1
sleep 1.0
read -r AX AY AW AH <<<"$(ov_rect wa)"
read -r BX BY BW BH <<<"$(ov_rect wb)"
echo "  全景里的格子：wa $AX,$AY ${AW}x${AH} / wb $BX,$BY ${BW}x${BH}"
[ -n "${AX:-}" ] && [ -n "${BX:-}" ] && pass "两条普通列都报了格子" ||
	fail "普通列的格子没报全（wa $AX,$AY ${AW}x${AH} / wb $BX,$BY ${BW}x${BH}）"

# 3 / 4 / 5 / 6：命中盒的判据（都用 pick-window —— 合成器真正的命中那条路）。
#
# 命中盒的语义（照现有实现，说清）：盒子 = **表面脚印 × 行缩放**（wlroots 的
# scene_node_get_size() 拿内容节点的 dst，再被表面尺寸卡一遍边界）。表面脚印比窗口
# 矩形大一圈 CSD 留白（pattern.py 每边 20px，缩到 0.46 之后每边 ≈9px），所以相邻两列
# 的盒子在缝里会重叠 —— 这和 t=0 桌面上是同一个语义（那儿留白 20px、缝只有 10px，
# 重叠得更多），带内谁在上由层级定（焦点列在上），不算错。
python3 - "$RUNDIR" "$LOG" "$AX" "$AY" "$AW" "$AH" "$BX" "$BY" "$BW" "$BH" <<'PY2'
import json, re, subprocess, sys

rundir, log, ax, ay, aw, ah, bx, by, bw, bh = sys.argv[1:]
cells = [("wa", int(ax), int(ay), int(aw), int(ah)),
         ("wb", int(bx), int(by), int(bw), int(bh))]
text = open(log, encoding="utf-8", errors="replace").read()
m = re.search(r"overview: 窗口 \S+ -> 屏幕 \d+,\d+ \d+x\d+（缩放 ([\d.]+)）", text)
scale = float(m.group(1)) if m else 1.0
win = json.loads(subprocess.run(["./build/vidowmsg", "windows"],
                                capture_output=True, text=True).stdout)
pad = 0
for w in win:
    # 只按普通窗口算：小窗 / 图标态的 geometry 是那块小盒子（353x196），和表面
    # 尺寸（665x740）差得远，混进来会把留白圈撑成一百多像素、判据就形同虚设。
    if str(w.get("mini", "none")) not in ("none", "free"):
        continue
    s, g = w.get("surface") or {}, w.get("geometry") or {}
    if s and g:
        pad = max(pad, (s["width"] - g["width"]) / 2.0,
                  (s["height"] - g["height"]) / 2.0)
margin = int(pad * scale) + 1
print("  命中盒 = 格子 ±%dpx（缩放后的 CSD 留白：%d × %.3f）" % (margin, pad, scale))

failed = []

def check(ok, what):
    print(("  PASS  " if ok else "  FAIL  ") + what)
    if not ok:
        failed.append(what)

def pick(x, y):
    out = subprocess.run(["./build/vidowmsg", "pick-window", str(x), str(y)],
                         capture_output=True, text=True).stdout
    try:
        return ((json.loads(out).get("window") or {}).get("title") or "")
    except Exception:
        return ""

def neighbours(t):
    out = []
    for n, x, y, w, h in cells:
        if n != t:
            out.append((n, x - margin, y - margin, w + 2 * margin, h + 2 * margin))
    return out

for t, x, y, w, h in cells:
    cx, cy = x + w // 2, y + h // 2
    check(pick(cx, cy) == t,
          "格子中心命中它自己（%s @ %d,%d）" % (t, cx, cy))
    for px, py, where in ((x + 4, y + 4, "左上"), (x + w - 5, y + 4, "右上"),
                          (x + 4, y + h - 5, "左下"), (x + w - 5, y + h - 5, "右下")):
        got = pick(px, py)
        near = [n for n, nx, ny, nw, nh in neighbours(t)
                if nx <= px < nx + nw and ny <= py < ny + nh]
        if near:
            check(got in (t,) + tuple(near),
                  "格子%s内缩 4px：%d,%d 命中 %s（这点在 %s 的留白圈里，"
                  "落两者之一都算对）" % (where, px, py, got or "空", "/".join(near)))
        else:
            check(got == t,
                  "格子%s内缩 4px 命中它自己（%s @ %d,%d）"
                  % (where, t, px, py))

gx = (cells[0][1] + cells[0][3] + cells[1][1]) // 2
got = pick(gx, cells[0][2] + cells[0][4] // 2)
check(got in (cells[0][0], cells[1][0]),
      "两列之间的缝命中相邻那列（x=%d → %s）" % (gx, got or "空"))

for px, py in ((4, 716), (1276, 4), (1276, 716)):
    got = pick(px, py)
    check(got == "", "空白处不命中窗口（%d,%d → %s）" % (px, py, got or "空"))

sys.exit(1 if failed else 0)
PY2
[ $? -ne 0 ] && FAILED=1

# 1：点迷你窗那块 → 焦点换成它
read -r WX WY WW WH <<<"$(wbox wm)"
MCX=$((WX + WW / 2)); MCY=$((WY + WH / 2))
GOT=$(what "$MCX" "$MCY")
[ "$GOT" = "wm" ] && pass "迷你窗那块也画出来了、中心命中它自己（$MCX,$MCY）" ||
	fail "迷你窗中心 $MCX,$MCY 命中「$GOT」（应该是 wm）"
click_at "$MCX" "$MCY"
sleep 1.3
check_picked wm "点迷你窗" expand

# 1（图标态）：同一条路再走一遍
./build/vidowmsg set-mini-state --id "$(wid wm)" --state icon >/dev/null 2>&1
sleep 0.9
./build/vidowmsg focus-window --id "$(wid wa)" >/dev/null 2>&1
sleep 0.5
wtype -M logo -k o >/dev/null 2>&1
sleep 1.0
read -r WX WY WW WH <<<"$(wbox wm)"
MCX=$((WX + WW / 2)); MCY=$((WY + WH / 2))
GOT=$(what "$MCX" "$MCY")
[ "$GOT" = "wm" ] && pass "图标那块中心命中它自己（$MCX,$MCY）" ||
	fail "图标中心 $MCX,$MCY 命中「$GOT」（应该是 wm）"
read -r AX AY AW AH <<<"$(ov_rect wa)"
check_cell wa "$((AX + AW / 2))" "$((AY + AH / 2))" "格子中心命中它自己（屏上有图标）"
click_at "$MCX" "$MCY"
sleep 1.3
check_picked wm "点图标态" expand

# 2：普通窗照旧（屏上刚有小窗折腾过：先让焦点停在 wb，再点 wa 的格子）
./build/vidowmsg focus-window --id "$(wid wb)" >/dev/null 2>&1
sleep 0.5
wtype -M logo -k o >/dev/null 2>&1
sleep 1.0
read -r AX AY AW AH <<<"$(ov_rect wa)"
[ "$(wfield wb focused)" = "True" ] && pass "进第二次全景前焦点在 wb" ||
	fail "进第二次全景前焦点不在 wb（wb focused=$(wfield wb focused)）"
click_at "$((AX + AW / 2))" "$((AY + AH / 2))"
sleep 1.3
check_picked wa "点普通窗（wa）"

cp "$LOG" "$RUNDIR/log-miniov.txt"
stop_clients
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6

# ---------------------------------------------------------------- 双屏
mark "双屏：只缩当前那块屏，另一块的窗口淡出"
start_vidow 2
# 光标放左边那块屏 → 窗口开在那儿；然后把光标挪到右边，再开一个
point_at 640 2560 720
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
python3 tools/pattern.py left-1 >/dev/null 2>&1 &
sleep 1.0
point_at 1920 2560 720
sleep 0.3
python3 tools/pattern.py right-1 >/dev/null 2>&1 &
sleep 1.2
grim "$RUNDIR/two-normal.png"
point_at 640 2560 720
wtype -M logo -k o >/dev/null 2>&1
sleep 0.9
grim "$RUNDIR/two-overview.png"
cp "$LOG" "$RUNDIR/log-two.txt"
stop_clients
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6

# ---------------------------------------------------------------- 退出不反弹 + 遮罩配色
mark "退出动画不反弹 + 遮罩颜色可配（overview_dim_color）"
start_vidow 1
python3 tools/wall.py >/dev/null 2>&1 &
sleep 1.0
grim "$RUNDIR/wall-only.png"     # 只有壁纸：金色方块的基准
# 列窄一点（0.25），overview 缩下去之后两边留得出壁纸，好采样
./build/vidowmsg set-config window_move_ms 150 >/dev/null 2>&1
for n in p-a p-b p-c; do
	python3 tools/pattern.py "$n" >/dev/null 2>&1 &
	sleep 0.8
done
sleep 1.2

# 退出：把动画拉长到 600ms，Esc 之后每 50ms 采一次 t —— 必须是单调往下走。
# （以前这里是「重放一遍进入动画」：t 从 0 重新涨到 1，看起来就是闪弹一下。）
./build/vidowmsg set-config window_move_ms 600 >/dev/null 2>&1
wtype -M logo -k o >/dev/null 2>&1
sleep 1.0
: > "$RUNDIR/exit-t.txt"
wtype -k Escape >/dev/null 2>&1
for _ in $(seq 1 14); do
	./build/vidowmsg overview 2>/dev/null |
		python3 -c "import json,sys; print('%.3f' % json.load(sys.stdin)['t'])" \
		>> "$RUNDIR/exit-t.txt"
	sleep 0.05
done
sleep 0.7
./build/vidowmsg set-config window_move_ms 150 >/dev/null 2>&1

dim_shot() { # dim_shot <名字> [颜色]
	if [ -n "${2:-}" ]; then
		./build/vidowmsg set-config overview_dim_color "$2" >/dev/null 2>&1
		sleep 0.3
	fi
	wtype -M logo -k o >/dev/null 2>&1
	sleep 0.9
	grim "$RUNDIR/$1.png"
	wtype -k Escape >/dev/null 2>&1
	sleep 0.9
}
# 这一段量的是「遮罩把壁纸压到多少」，得先关掉壁纸模糊（默认是开的），不然
# 采到的金色方块是糊过的边。模糊单独一轮量（见下面「全景里的壁纸模糊」）。
./build/vidowmsg set-config overview_blur false >/dev/null 2>&1
sleep 0.3
dim_shot dim-default
dim_shot dim-off "#00000000"
dim_shot dim-red "#ff000099"
./build/vidowmsg get-config overview_dim_color > "$RUNDIR/dim-cfg.json" 2>&1
./build/vidowmsg set-config overview_dim_color "紫色" > "$RUNDIR/dim-bad.txt" 2>&1
./build/vidowmsg set-config overview_dim_color "#05050899" >/dev/null 2>&1

# 全景里的壁纸模糊：同一屏、同一个遮罩，只切 overview_blur。
ov_shot() { # ov_shot <名字> <true|false>
	./build/vidowmsg set-config overview_blur "$2" >/dev/null 2>&1
	sleep 0.4
	wtype -M logo -k o >/dev/null 2>&1
	sleep 1.0
	grim "$RUNDIR/$1.png"
	wtype -k Escape >/dev/null 2>&1
	sleep 0.9
}
ov_shot ov-blur-on true
ov_shot ov-blur-off false
./build/vidowmsg get-config overview_blur > "$RUNDIR/blur-cfg.json" 2>&1
cp "$LOG" "$RUNDIR/log-dim.txt"
stop_clients
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6

# ---------------------------------------------------------------- 全景用另一张壁纸
mark "全景用另一张壁纸（图层规则 overview = \"only\"）"
# 两块 background 层 surface：wall.py 画桌面，第二块（namespace wall2）只在全景里
# 露面。规则写在配置里（14-keys.lua 那一族），namespace 匹配。
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 150
window_close_ms = 150
window_move_ms = 150
background_color = "#101010"
overview_blur = true
vidow.bind("Mod+O", "toggle-overview")
vidow.rule{ namespace = "^wall2$", overview = "only" }
EOF
start_vidow 1
python3 tools/wall.py >/dev/null 2>&1 &
sleep 1.2
grim "$RUNDIR/ovbg-wall.png"
python3 tools/wall.py "#ff00ff" wall2 >/dev/null 2>&1 &
sleep 1.5
grim "$RUNDIR/ovbg-desk.png"
./build/vidowmsg background > "$RUNDIR/ovbg-bg.json" 2>&1
./build/vidowmsg layers > "$RUNDIR/ovbg-layers.json" 2>&1
wtype -M logo -k o >/dev/null 2>&1
sleep 1.1
grim "$RUNDIR/ovbg-overview.png"
wtype -k Escape >/dev/null 2>&1
sleep 1.1
grim "$RUNDIR/ovbg-back.png"
# 热改规则：only -> always，那块就变成普通壁纸（盖住桌面）
sed -i 's/overview = "only"/overview = "always"/' \
	"$CFGDIR/.config/vidow/config.lua"
./build/vidowmsg reload-config >/dev/null 2>&1
sleep 1.4
grim "$RUNDIR/ovbg-always.png"
sed -i 's/overview = "always"/overview = "only"/' \
	"$CFGDIR/.config/vidow/config.lua"
./build/vidowmsg reload-config >/dev/null 2>&1
sleep 1.4
grim "$RUNDIR/ovbg-only2.png"
# 取值写错要当场报错，而且报出三个能写的名字（老名字 off / hide 也得被拒）
printf 'vidow.rule{ namespace = "^wall2$", overview = "hide" }\n' \
	>> "$CFGDIR/.config/vidow/config.lua"
./build/vidowmsg reload-config > "$RUNDIR/ovbg-bad.txt" 2>&1
sleep 0.6
cp "$LOG" "$RUNDIR/log-ovbg.txt"
stop_clients
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6

leftover=0
for pid in $VIDOW_PIDS; do
	kill -0 "$pid" 2>/dev/null && leftover=$((leftover + 1))
done
[ "$leftover" -eq 0 ] && pass "测试起的合成器都收干净了" ||
	fail "还有 $leftover 个测试起的 vidow 没退"

# ---------------------------------------------------------------- 量结果
python3 - "$LOG" "$SNAP" "$RUNDIR" <<'PY'
import json, re, sys
from PIL import Image

rundir = sys.argv[3]
def image(name):
    return Image.open("%s/%s.png" % (rundir, name)).convert("RGB")

def places(path):
    out = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"place (\S+) -> (-?\d+),(-?\d+) (\d+)x(\d+)", line)
        if m:
            out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return out

# 单屏那段日志（双屏那段会覆盖 LOG，所以最后读到的只有双屏 —— 单屏的留在文件里）
single = open("%s/log-single.txt" % rundir, encoding="utf-8",
              errors="replace").read()
two = open("%s/log-two.txt" % rundir, encoding="utf-8",
           errors="replace").read()
failed = []

def check(ok, what):
    print(("  PASS  " if ok else "  FAIL  ") + what)
    if not ok:
        failed.append(what)

quad = [((1, 1), (255, 0, 0)), ((3, 1), (0, 255, 0)),
        ((1, 3), (0, 0, 255)), ((3, 3), (255, 255, 255))]

def quad_ok(im, rect):
    x, y, w, h = rect
    if x < 0 or y < 0 or x + w > im.size[0] or y + h > im.size[1]:
        return None
    for (fx, fy), want in quad:
        got = im.getpixel((x + w * fx // 4, y + h * fy // 4))
        if max(abs(c - w2) for c, w2 in zip(got, want)) > 24:
            return False
    return True

print()
print("== 单屏 ==")
m = re.search(r"overview: 进入（(\S+) 上 (\d+) 个窗口，占 (\d+)x(\d+) 缩到 ([\d.]+)，"
              r"铺在工作区 (\d+)x(\d+) 里）", single)
check(m is not None, "Mod+O 进入 overview，日志报出缩放和铺法")
scale = 0.0
if m:
    n, sw, sh, scale, ww, wh = (m.group(2), int(m.group(3)), int(m.group(4)),
                                float(m.group(5)), int(m.group(6)), int(m.group(7)))
    check(int(n) >= 3, "数到了 %s 个窗口" % n)
    check(0.0 < scale < 1.0, "缩放 %s < 1（是缩小不是放大）" % scale)
    check(sw * scale <= ww + 1 and sh * scale <= wh + 1,
          "整条列带 %dx%d 缩成 %.0fx%.0f 装进了工作区 %dx%d"
          % (sw, sh, sw * scale, sh * scale, ww, wh))

rects = {}
for mm in re.finditer(r"overview: 窗口 (\S+) -> 屏幕 (-?\d+),(-?\d+) (\d+)x(\d+)"
                      r"（缩放 ([\d.]+)）", single):
    rects[mm.group(1)] = tuple(int(v) for v in mm.groups()[1:5])
check(len(rects) >= 3, "每个窗口都报了它在 overview 里的位置（%d 个）" % len(rects))
check(all(0 <= r[0] and r[0] + r[2] <= 1280 and 0 <= r[1] and r[1] + r[3] <= 720
          for r in rects.values()),
      "所有窗口都落在屏幕里（不会缩到一半被切掉）")

before = places("%s/normal.txt" % rundir)
check(all(t in before for t in rects), "拔线前的位置都记下来了")

if before and rects:
    ok_size = all(abs(rects[t][2] - before[t][2] * scale) <= 2 and
                  abs(rects[t][3] - before[t][3] * scale) <= 2
                  for t in rects if t in before)
    check(ok_size, "overview 里的尺寸 = 原尺寸 × %s（±2px）" % scale)

    order_b = sorted(before, key=lambda t: before[t][0])
    order_o = sorted(rects, key=lambda t: rects[t][0])
    check(order_b == order_o, "窗口的先后顺序没变（%s）" % " < ".join(order_o))
    if len(order_b) >= 2:
        gap_b = before[order_b[1]][0] - before[order_b[0]][0]
        gap_o = rects[order_o[1]][0] - rects[order_o[0]][0]
        check(abs(gap_o - gap_b * scale) <= 3,
              "窗口之间的间距也跟着缩（%d -> %d ≈ %d × %s）"
              % (gap_b, gap_o, gap_b, scale))

im_ov = image("overview")
ok_px = [t for t in rects if quad_ok(im_ov, rects[t])]
check(len(ok_px) >= 3,
      "overview 里窗口的四象限颜色正确（%d 个按日志坐标采样通过）" % len(ok_px))
check(all(im_ov.getpixel((rects[t][0] + rects[t][2] // 4,
                          rects[t][1] + rects[t][3] // 4)) == (255, 0, 0)
          for t in rects if quad_ok(im_ov, rects[t])),
      "窗口没有被压暗（遮罩夹在壁纸和窗口之间，不是盖在窗口上）")

im_norm = image("normal")
bg_norm = im_norm.getpixel((4, 716))
bg_ov = im_ov.getpixel((4, 716))
check(sum(bg_ov) < sum(bg_norm),
      "壁纸被压暗了（%s -> %s）" % (bg_norm, bg_ov))

# Esc 退出：画面逐像素回到进 overview 之前
im_back = image("back")
diff = sum(1 for y in range(720) for x in range(1280)
           if im_back.getpixel((x, y)) != im_norm.getpixel((x, y)))
# 缩下去再回来，内容的场景尺寸要按比例算两遍（round），边缘允许有个位数的抗锯齿差
check(diff <= 200, "Esc 退出后画面回到原样（%d 个像素不同，只允许取整带来的零星差异）"
      % diff)

# 点选
m = re.search(r"overview: 选中 (\S+)", single)
check(m is not None, "点某个窗口 = 选中它")
check(re.search(r"overview: 选中 \S+\n.*overview: 退出", single, re.S) is not None or
      single.count("overview: 退出") >= 2,
      "选中之后就缩回去了（不用再按 Esc）")

picked = places("%s/picked.txt" % rundir)
if m and picked:
    t = m.group(1)
    r = picked.get(t)
    check(r is not None, "选中的窗口 %s 有新摆位（焦点真换过去了）" % t)
    if r:
        check(r[0] >= 0 and r[0] + r[2] <= 1280,
              "选中的那一列被滚进画面（x=%d 宽 %d）" % (r[0], r[2]))
        others = [v for k, v in picked.items() if k != t]
        check(any(v[0] < 0 or v[0] + v[2] > 1280 for v in others),
              "没选的列滚出画面了（还是那条带，没有重新排）")

print()
print("== 双屏 ==")
m = re.search(r"overview: 进入（(\S+) 上 (\d+) 个窗口", two)
check(m is not None, "进入 overview（只看当前那块屏 %s）" % (m.group(1) if m else "?"))
m2 = re.search(r"overview: 窗口 (\S+) -> 屏幕 (-?\d+),(-?\d+) (\d+)x(\d+)", two)
check(m2 is not None and m2.group(1).startswith("left"),
      "只报了当前那块屏上的窗口：%s" % (m2.group(1) if m2 else "无"))
check(re.search(r"overview: 窗口 right-1 -> ", two) is None,
      "另一块屏上的窗口不进 overview 的清单")

im_two = image("two-overview")
im_two_norm = image("two-normal")
# 另一块屏的窗口在 normal 里是 pattern 的颜色，overview 里应该被淡掉（只剩壁纸/遮罩）
def find_pattern(im, x0, x1):
    for y in range(120, 700, 7):
        for x in range(x0, x1, 7):
            if im.getpixel((x, y)) in ((255, 0, 0), (0, 255, 0), (0, 0, 255),
                                       (255, 255, 255)):
                return (x, y)
    return None

right_before = find_pattern(im_two_norm, 1300, 2560)
right_after = find_pattern(im_two, 1300, 2560)
check(right_before is not None, "另一块屏上本来有窗口（%s）" % (right_before,))
check(right_after is None, "overview 里另一块屏上的窗口淡出了（现在只剩壁纸）")

print()
print("== 退出不反弹 ==")
# 动画引擎的 t 永远 0 → 1，方向得由 on 决定（见 overview_update）。以前直接把
# t 当进度用，退出 = 把「进入」重放一遍再瞬间跳回去 —— 就是用户报的「点选之后
# 闪弹一下」。这里量退出过程中 t 的走向：必须从 1 单调降到 0。
ts = [float(x) for x in
      open("%s/exit-t.txt" % rundir, encoding="utf-8").read().split()]
check(len(ts) >= 8, "退出过程中采到 %d 个 t" % len(ts))
if len(ts) >= 8:
    check(ts[0] > 0.9, "按下 Esc 的那一刻还是展开状态（t=%.3f）" % ts[0])
    back = [i for i in range(1, len(ts)) if ts[i] > ts[i - 1] + 0.02]
    check(not back,
          "t 一路往下走，没有回头（%s）" % " ".join("%.2f" % v for v in ts))
    check(ts[-1] < 0.05, "最后收干净了（t=%.3f）" % ts[-1])
    mid = [v for v in ts if 0.2 < v < 0.8]
    check(len(mid) >= 2, "中间真的在动，不是一帧跳过去（%d 个中间态）" % len(mid))

print()
print("== 遮罩颜色可配（overview_dim_color）==")
wall = image("wall-only")


def is_gold(p):
    r, g, b = p
    return r > 170 and r - b > 110 and g - b > 80 and g > 120


# 壁纸基准：只有壁纸那张里金色方块的均值
base_pts = [(x, y) for y in range(2, 718, 2) for x in range(2, 1278, 2)
            if is_gold(wall.getpixel((x, y)))]
check(len(base_pts) > 500, "壁纸基准采样点 %d 个（金色方块）" % len(base_pts))


def still_gold(p):  # 压暗之后的金色：还是 r > g > b，只是暗了
    r, g, b = p
    return r > 70 and r > g > b and r - b > 40 and g - b > 25


def mean(im, pts):
    s = [0, 0, 0]
    for p in pts:
        c = im.getpixel(p)
        for i in range(3):
            s[i] += c[i]
    return tuple(round(v / len(pts)) for v in s)


base = mean(wall, base_pts)
check(abs(base[0] - 255) < 12 and base[1] > 190 and base[2] < 90,
      "壁纸金色基准 %s" % (base,))

dim = {}
for name in ("dim-default", "dim-off", "dim-red"):
    im = image(name)
    pts = [p for p in base_pts if still_gold(im.getpixel(p))]
    if len(pts) < 200:
        check(False, "%s：采样点不够（%d）" % (name, len(pts)))
        continue
    dim[name] = (mean(im, pts), len(pts))

if len(dim) == 3:
    d, n = dim["dim-default"]
    check(sum(d) < sum(base) * 0.6,
          "默认压暗：壁纸 %s -> %s（%.0f%%，%d 个点）"
          % (base, d, 100 * sum(d) / sum(base), n))
    check(abs(d[0] - d[1]) < 40 and d[2] < 60,
          "默认遮罩是近黑偏蓝，不是染了色（%s）" % (d,))

    o, n = dim["dim-off"]
    check(abs(o[0] - base[0]) <= 12 and abs(o[1] - base[1]) <= 12 and
          abs(o[2] - base[2]) <= 12,
          "overview_dim_color = #00000000（alpha 0）时壁纸原样：%s -> %s（%d 个点）"
          % (base, o, n))

    r_, n = dim["dim-red"]
    check(r_[0] > 200 and r_[0] > r_[1] * 2 and r_[1] < base[1] - 40,
          "#ff000099 时壁纸被染红：%s（红 %d 绿 %d 蓝 %d，%d 个点）"
          % (r_, r_[0], r_[1], r_[2], n))

    # 遮罩在窗口下面：三个配色下窗口都是原色
    for name in ("dim-default", "dim-off", "dim-red"):
        im = image(name)
        px = [im.getpixel((x, y)) for y in range(300, 420, 20)
              for x in range(40, 240, 20)]
        check(any(p == (255, 0, 0) for p in px),
              "%s：窗口本身没被压暗（采样里还有纯红 255,0,0）" % name)

cfg = open("%s/dim-cfg.json" % rundir, encoding="utf-8", errors="replace").read()
check("#ff000099" in cfg, "get-config 读回改过的值：%s" % " ".join(cfg.split()))
bad = open("%s/dim-bad.txt" % rundir, encoding="utf-8", errors="replace").read()
check("改不了" in bad or "不合法" in bad,
      "非法颜色被拒（%s）" % bad.strip().splitlines()[0][:60])

dim_log = open("%s/log-dim.txt" % rundir, encoding="utf-8",
               errors="replace").read()
check("Assertion" not in dim_log and "abort" not in dim_log.lower(),
      "遮罩那一段日志里没有 assert / abort")

print()
print("== 全景里的壁纸模糊（overview_blur）==")
# 判据：壁纸自己那些硬边（wall.py 的金色方块）在全景里有没有被糊开 —— 跨边缘
# 扫一条线，量相邻采样点的最大跳变。原图是硬边（一下跳完），糊过就摊成好几步。
def is_gold(p):
    r, g, b = p
    return r > 170 and r - b > 110 and g - b > 80 and g > 120


edge = None
for y in range(2, 300, 2):
    for x in range(2, 1270, 2):
        if (is_gold(wall.getpixel((x, y))) and
                not is_gold(wall.getpixel((x + 6, y))) and
                not is_gold(wall.getpixel((x + 14, y)))):
            edge = (x, y)
            break
    if edge is not None:
        break
check(edge is not None, "壁纸里找到一条硬边（%s）" % (edge,))


def edge_step(im, edge):
    x, y = edge
    vals = [im.getpixel((x + dx, y)) for dx in range(-14, 22, 3)]
    return max(max(abs(a - b) for a, b in zip(p, q))
               for p, q in zip(vals, vals[1:])), vals


if edge is not None:
    off, _ = edge_step(image("ov-blur-off"), edge)
    on, _ = edge_step(image("ov-blur-on"), edge)
    check(off > 40, "overview_blur = false 时壁纸还是硬边（跳变 %d）" % off)
    check(on * 4 < off,
          "overview_blur = true 时壁纸被糊开（跳变 %d → %d，不到 1/4）" % (off, on))

    # 窗口本身不能跟着糊：全景里窗口还是原色
    im = image("ov-blur-on")
    px = [im.getpixel((x, y)) for y in range(200, 500, 20)
          for x in range(240, 1200, 20)]
    check(any(p == (255, 0, 0) for p in px) and
          any(p == (0, 0, 255) for p in px),
          "缩小的窗口本身没被糊（采样里还有纯红 / 纯蓝）")

blur_cfg = open("%s/blur-cfg.json" % rundir, encoding="utf-8",
                errors="replace").read()
check("false" in blur_cfg,
      "get-config 读回 overview_blur：%s" % " ".join(blur_cfg.split()))

print()
print("== 全景用另一张壁纸（图层规则 overview = \"only\"）==")
# 两块 background 层 surface 叠着：wall.py（渐变，桌面那张）和 wall2（纯洋红）。
# 规则 overview = "only" 让 wall2 只在全景里露面 —— 桌面上它连**画**都不该画
# （节点是关的，所以也不进窗口的模糊底衬），全景里才露出来（压暗 + 糊过）。


def is_magenta(p):
    r, g, b = p
    return r > 200 and b > 200 and g < 90


def is_gold(p):
    r, g, b = p
    return r > 170 and r - b > 110 and g - b > 80 and g > 120


def frac(im, pred, step=3):
    hit = tot = 0
    for y in range(0, im.size[1], step):
        for x in range(0, im.size[0], step):
            tot += 1
            if pred(im.getpixel((x, y))):
                hit += 1
    return 100.0 * hit / tot


desk = image("ovbg-desk")
gold_pct = frac(desk, is_gold)
mag_pct = frac(desk, is_magenta)
check(gold_pct > 5, "桌面上还是 wall.py 那张壁纸（金色 %.0f%%）" % gold_pct)
check(mag_pct == 0,
      "overview = \"only\" 那块桌面上一点没画（洋红 %.0f%%）" % mag_pct)

ov = image("ovbg-overview")
# 全景里那张是**底衬**（模糊那份画在遮罩下面）：压暗 + 糊过，取的是色相关系。
dim_mag = frac(ov, lambda p: p[0] > 50 and p[2] > 50 and
               abs(p[0] - p[2]) < 70 and p[1] < p[0] - 25 and p[1] < p[2] - 25)
check(dim_mag > 50, "全景里露出来的就是它（压暗过的洋红 %.0f%%）" % dim_mag)

back = image("ovbg-back")
check(frac(back, is_magenta) == 0 and frac(back, is_gold) > 5,
      "退出全景之后又收起来了（洋红 0%%，金色 %.0f%%）" % frac(back, is_gold))

# 热改规则：规则得当场生效，不能等下次开关全景
always = image("ovbg-always")
always_pct = frac(always, is_magenta)
check(always_pct > 50,
      "规则热改成 \"always\" 之后它当场变成普通壁纸，盖住桌面（洋红 %.0f%%）"
      % always_pct)
only2 = image("ovbg-only2")
check(frac(only2, is_magenta) == 0,
      "再改回 \"only\" 也当场生效（洋红 %.0f%%）" % frac(only2, is_magenta))

# 查询接口：两块都算 client，但「现在真的画着」的只有一块
bg = json.loads(open("%s/ovbg-bg.json" % rundir, encoding="utf-8",
                     errors="replace").read())
check(bg.get("clients") == 2, "background 报出两块壁纸 client（%s）"
      % bg.get("clients"))
check(bg.get("visible") == 1, "其中「画着的」只有一块（visible=%s）"
      % bg.get("visible"))
check(bg.get("outputs", [{}])[0].get("covered") is True,
      "covered 只看画着的那块（wall.py 铺满了）")

lay = json.loads(open("%s/ovbg-layers.json" % rundir, encoding="utf-8",
                      errors="replace").read())
lay = lay["layers"] if isinstance(lay, dict) and "layers" in lay else lay
by_ns = {l.get("namespace"): l for l in lay if l.get("layer") == "background"}
check(by_ns.get("wall2", {}).get("overview") == "only",
      "layers 报出 wall2 的 overview 规则（%s）"
      % by_ns.get("wall2", {}).get("overview"))
check(by_ns.get("wall2", {}).get("shown") is False and
      by_ns.get("wall", {}).get("shown") is True,
      "shown 如实反映「现在画没画」（wall2=%s wall=%s）"
      % (by_ns.get("wall2", {}).get("shown"),
         by_ns.get("wall", {}).get("shown")))

ovbg_log = open("%s/log-ovbg.txt" % rundir, encoding="utf-8",
                errors="replace").read()
check("全景里 = 只在全景里显示" in ovbg_log and "全景里 = 照常" in ovbg_log,
      "规则生效有日志（rule: layer wall2 全景里 = …）")
bad = open("%s/ovbg-bad.txt" % rundir, encoding="utf-8",
           errors="replace").read() + ovbg_log
check('\"always\"、\"only\" 或 \"never\"' in bad,
      "取值写错当场报错，并报出三个能写的名字（老名字 off / hide 也拒）")
check("Assertion" not in ovbg_log and "abort" not in ovbg_log.lower(),
      "这一段日志里没有 assert / abort")

check("Assertion" not in single and "abort" not in single.lower() and
      "Assertion" not in two and "abort" not in two.lower(),
      "日志里没有 assert / abort")

print()
print("结论：%s" % ("全部通过" if not failed else "%d 项失败" % len(failed)))
sys.exit(1 if failed else 0)
PY
rc=$?
[ $rc -ne 0 ] && FAILED=1

# ---------------------------------------------------------------- 过冲缓动
mark "过冲缓动（easing 让 t 冲过 1）按 Mod+O 不许把合成器打死"
# 用户真机复现：`easing = "out_back 0.18"`（t 冲到 1.18）时进 overview，
# ov->t 跟着冲到 1.18 -> wlr_scene_buffer_set_opacity 断言 -> **整个合成器死**。
# 根因：overview_update() 把缓动后的 t 当进度直接喂 set_opacity，而注释里
# 「t 永远 0 → 1」只在不过冲的曲线下成立。判据 = 进程活着 + 状态照常 + 日志无 assert。
start_vidow 1
python3 tools/wall.py >/dev/null 2>&1 &
sleep 1.0
python3 tools/pattern.py ov-back >/dev/null 2>&1 &
sleep 1.2
./build/vidowmsg set-config easing "out_back 0.18" >/dev/null 2>&1
sleep 0.3
wtype -M logo -k o >/dev/null 2>&1
sleep 1.2
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "过冲曲线（out_back 0.18）下进 overview：合成器还活着"
else
	fail "过冲曲线下进 overview：合成器被打死了（opacity 断言）"
fi
./build/vidowmsg overview > "$RUNDIR/ov-overshoot.json" 2>&1
python3 - "$RUNDIR/ov-overshoot.json" <<'PY'
import json, sys
try:
	d = json.load(open(sys.argv[1], encoding="utf-8"))
except Exception as e:                                  # 进程死了就没有 JSON
	print("  FAIL  过冲曲线下 overview 状态查不出来：%s" % e)
	sys.exit(1)
ok = d.get("active") is True and 0.0 < (d.get("scale") or 0.0) <= 1.0
print("  %s  过冲曲线下 overview 状态照常（active=%s scale=%s rows=%s）" %
      ("PASS" if ok else "FAIL", d.get("active"), d.get("scale"), d.get("rows")))
sys.exit(0 if ok else 1)
PY
[ $? -ne 0 ] && FAILED=1
if grep -ai "Assertion\|abort" "$LOG" >/dev/null 2>&1; then
	fail "过冲这一段日志里有 assert / abort"
else
	pass "过冲这一段日志里没有 assert / abort"
fi
wtype -k Escape >/dev/null 2>&1
sleep 1.0
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "退出来也还活着（过冲曲线进出各一次）"
else
	fail "退出 overview 时死了"
fi

# ---------------------------------------------------------------- 滚轮巡航
mark "滚轮：一次一格 = 一块工作区（不是写死 60 像素），横轴不吃这条"
# 用户：「overview滚轮操作应该是巡航一次滚到一个工作区」。以前每次写死 60 屏幕像素
# （诊断实测 570.7→630.7 = +60）：行距 334 的时候要滚 6 下才走一行，而且滚到哪儿停下
# 全看滚了几格，永远对不齐行。现在一格 = 一行（行距 row_h + OV_ROW_GAP）。
# 判据：往上滚两格，每格位移 = 行距（±2px）；往下滚到底后不再增长；横向轴（滚轮
# 左右倾）一格都不动（以前它也滚同一个列表 —— 概览里没有横向可滚的东西）。
ovfield() {
	./build/vidowmsg overview 2>/dev/null |
		python3 -c "import sys,json;print(json.load(sys.stdin).get('$1'))"
}
start_vidow 1
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
for n in 1 2 3 4; do
	python3 tools/pattern.py "r-$n" >/dev/null 2>&1 &
	sleep 1.0
	if [ "$n" != 4 ]; then
		./build/vidowmsg action "workspace down" >/dev/null 2>&1
		sleep 0.7
	fi
done
sleep 1.0
./build/vidowmsg action "toggle-overview" >/dev/null 2>&1
sleep 1.2
ROWS=$(ovfield rows)
ROWH=$(ovfield row_h)
PITCH=$(python3 -c "print($ROWH + 12.0)")
echo "   概览里 rows=$ROWS row_h=$ROWH → 行距 $PITCH"
if [ "${ROWS:-0}" -ge 3 ] 2>/dev/null; then
	pass "有 $ROWS 行（>=3，够测踏步）"
else
	fail "行数不够（$ROWS），测不了踏步"
fi

PREV=$(ovfield list_scroll)
for k in 1 2; do
	./build/vidowmsg pointer-scroll 0 -1 >/dev/null 2>&1
	sleep 0.6
	NOW=$(ovfield list_scroll)
	python3 - "$PREV" "$NOW" "$PITCH" <<'PY'
import sys
a, b, p = float(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
d = a - b
# 一格 = 一个行距（±2px）。两个正常例外：① 从「夹在 max 上」的位置起滚时，第一格
# 先吸附到下面的整数倍上（位移不足一个行距，实测 630.72 → 337.68 = 293）；② 到顶。
on_grid = abs(b) < 0.5 or abs(b / p - round(b / p)) < 0.02
ok = on_grid and 0.0 < d <= p + 2.0
print("  %s  往上滚一格走了 %.1f 像素（行距 %.1f，落点 %s）"
      % ("PASS" if ok else "FAIL", d, p,
         "行距整数倍" if on_grid else "不在整数倍上"))
sys.exit(0 if ok else 1)
PY
	[ $? -ne 0 ] && FAILED=1
	PREV=$NOW
done

# 判据：滚动是**缓动**过去的，不是瞬移（用户：「overview 滚动需要过渡动画」）。
# 发一次滚轮后连采 8 帧 list_scroll：必须采到至少一个**严格在起点与目标之间**的
# 值（瞬移的话每次采样都已经是目标值）；等缓动跑完再采一次，应该正好落在目标上。
S0=$(ovfield list_scroll)
./build/vidowmsg pointer-scroll 0 1 >/dev/null 2>&1
MIDS=""
for _ in 1 2 3 4 5 6 7 8; do MIDS="$MIDS $(ovfield list_scroll)"; done
sleep 0.8
SEND=$(ovfield list_scroll)
python3 - "$S0" "$SEND" $MIDS <<'ZZPY'
import sys
s0, end = float(sys.argv[1]), float(sys.argv[2])
mids = [float(x) for x in sys.argv[3:]]
moved = end > s0 + 1.0
between = [m for m in mids if s0 + 0.5 < m < end - 0.5]
if not moved:
	print("  FAIL  这一格没滚起来（%.1f → %.1f）" % (s0, end))
	sys.exit(1)
if not between:
	print("  FAIL  没有中间态：起点 %.1f 目标 %.1f，8 帧采到 %s（瞬移的话每帧都等于目标）"
	      % (s0, end, " ".join("%.1f" % m for m in mids[:4])))
	sys.exit(1)
print("  PASS  缓动：起点 %.1f → 目标 %.1f，途中采到 %d 个中间态（%s）"
      % (s0, end, len(between), " ".join("%.1f" % b for b in between[:4])))
ZZPY
[ $? -ne 0 ] && FAILED=1

for _ in $(seq 1 8); do
	./build/vidowmsg pointer-scroll 0 1 >/dev/null 2>&1
	sleep 0.3
done
E1=$(ovfield list_scroll)
./build/vidowmsg pointer-scroll 0 1 >/dev/null 2>&1
sleep 0.5
E2=$(ovfield list_scroll)
if [ "$E1" = "$E2" ]; then
	pass "往下滚到底就停住，再滚不涨（停在 $E1）"
else
	fail "到底还会继续涨：$E1 → $E2"
fi

H1=$(ovfield list_scroll)
./build/vidowmsg pointer-scroll 1 0 >/dev/null 2>&1
sleep 0.5
H2=$(ovfield list_scroll)
if [ "$H1" = "$H2" ]; then
	pass "横向轴（滚轮左右倾）不滚列表（$H1 → $H2）"
else
	fail "横向轴把列表滚了：$H1 → $H2"
fi
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "滚了这么多次还活着"
else
	fail "滚轮把合成器滚死了"
fi
./build/vidowmsg action "toggle-overview" >/dev/null 2>&1
sleep 0.8
if [ "$(ovfield active)" = "False" ]; then
	pass "退出概览（active=false）"
else
	fail "退不出概览（active=$(ovfield active)）"
fi
# ---------------------------------------------------------------- overview 不跨屏
mark "overview 不跨屏：开 overview 前后，隔壁屏的非背景像素不增加"
# 用户：「overview 窗口太多会跨屏」。条带比屏宽的那些列本来就在行矩形外（场景没有
# 裁剪节点，见 overview.c 里 row->rect_x 那段注释），不裁就画到隔壁显示器上 ——
# 全景开在**左屏**（光标在哪块屏，全景锁哪块屏），溢出的列会落到右屏地界。
# 亮像素数（任一通道 > 200）：预览里的图案是纯色高亮，壁纸 / 压暗遮罩都是暗的。
# 不能按「非洋红」数 —— 右屏没铺壁纸（wall.py 只画光标那块屏），整屏都算「非背景」。
bright() {
	python3 - "$1" <<'PYNB'
import sys
from PIL import Image
import numpy as np
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
print(int((a.max(axis=2) > 200).sum()))
PYNB
}
stop_clients
start_vidow 2
point_at 640 2560 720
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
point_at 1920 2560 720
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
point_at 640 2560 720
sleep 0.3
for n in ov-1 ov-2 ov-3 ov-4 ov-5 ov-6 ov-7; do
	python3 tools/pattern.py "$n" >/dev/null 2>&1 &
	sleep 0.6
done
sleep 1.5
grim -t ppm -g "1280,0 1280x720" - > "$RUNDIR/ov-before.ppm" 2>/dev/null
point_at 640 2560 720
sleep 0.3
wtype -M logo -k o >/dev/null 2>&1
sleep 1.2
grim -t ppm -g "1280,0 1280x720" - > "$RUNDIR/ov-after.ppm" 2>/dev/null
wtype -k Escape >/dev/null 2>&1
sleep 0.6
if [ ! -s "$RUNDIR/ov-before.ppm" ] || [ ! -s "$RUNDIR/ov-after.ppm" ]; then
	fail "截图是空的（before/after）"
else
	B=$(bright "$RUNDIR/ov-before.ppm")
	A=$(bright "$RUNDIR/ov-after.ppm")
	echo "  右屏亮像素：开全景前 $B → 开全景后 $A"
	# 断言**完全相等**：右屏只该有壁纸，开全景不该动它一个像素。漏过去的
	# 预览会把洋红换成别的颜色（计数涨）或盖住金色方块（计数跌）—— 两头都能
	# 抓到；而压暗只是把洋红变暗洋红，计数不变。
	if [ "$A" = "$B" ]; then
		pass "overview 没往隔壁屏画东西（右屏 $B 个亮像素，前后一致）"
	else
		fail "overview 漏到隔壁屏：右屏亮像素 $B → $A（差 $((A - B))）"
	fi
fi
stop_clients
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6


echo
if [ $FAILED -eq 0 ]; then
	echo "test-overview.sh: OK"
else
	echo "test-overview.sh: 有失败项"
fi
exit $FAILED
