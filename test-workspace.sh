#!/bin/bash
# 工作区：普通工作区各自一叠列 / 窗口，切过去才可见；特殊工作区不进列表、跟随
# 激活的那块屏；键位由配置自己绑（合成器不预设）；热插拔时工作区跟着列走。
#
# 判据（日志 + 像素 + Lua 查询）：
#   1) 配置建的工作区都在，特殊工作区**不在** vidow.workspace_list() 里
#   2) 混着开窗口：每个窗口落在「当时显示着的那块工作区」，切回来后只有它的窗口
#      画在屏幕上（藏起来的那块上的窗口逐点不是 pattern 的颜色）
#   3) 切工作区：日志报可见窗口数，Lua 查询里 active 标记跟着走
#   4) 特殊工作区：搬进去的窗口在普通工作区看不见；打开后只显示它、显示在**聚焦
#      的那块屏**上；再开一次收起来，普通工作区原样回来
#   5) 一块屏拔掉：屏上的工作区和它的窗口跟着挪到剩下的屏上，仍然可见
#   6) 键位：vidow.bind() 绑的键真的在键位表里（日志里一行一条）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-workspace-test
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
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -TERM -- "-$pid" 2>/dev/null
	done
	sleep 0.4
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -KILL -- "-$pid" 2>/dev/null
	done
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

command -v grim >/dev/null || { echo "test-workspace.sh: 需要 grim"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-workspace.sh: 需要 python3-gobject"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 80
window_close_ms = 80
window_move_ms = 80
-- 切工作区的过渡关掉：这个测试断言的是**稳定之后**的状态（日志里那行
-- 「全屏 N 个可见」是切换当下数出来的，过渡里旧的那块还画着、数字会多一个；
-- 截图也在过渡结束前抓）。过渡本身是动画，不在这儿测。
workspace_switch_ms = 0
-- 工作区由用户建；键位由用户绑。
for i = 1, 4 do
	vidow.workspace_create(tostring(i))
end
vidow.workspace_special("scratch")

vidow.bind("Mod+1", "workspace 1")
vidow.bind("Mod+2", "workspace 2")
vidow.bind("Mod+Shift+1", "move 1")
vidow.bind("Mod+grave", "special scratch")
vidow.bind("Mod+Shift+grave", "move scratch")
vidow.bind("Mod+Tab", "workspace next")

-- 用户自己的选择器就是这么写的：只列普通工作区。
local names = {}
for _, ws in ipairs(vidow.workspace_list()) do
	names[#names + 1] = ws.name .. (ws.active and "*" or "")
	vidow.log(string.format("ws %s special=%s active=%s visible=%s views=%d",
		ws.name, tostring(ws.special), tostring(ws.active),
		tostring(ws.visible), ws.views))
end
vidow.log("list=" .. table.concat(names, ","))
vidow.log("active=" .. tostring(vidow.workspace_active()))
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

start_vidow() { # start_vidow <outputs> [test-actions]
	export WLR_HEADLESS_OUTPUTS=$1
	rm -f "$LOG"
	if [ -n "${VIDOW_TEST_OUTPUTS_X:-}" ]; then
		export VIDOW_TEST_OUTPUTS="$VIDOW_TEST_OUTPUTS_X"
	else
		unset VIDOW_TEST_OUTPUTS
	fi
	if [ -n "${2:-}" ]; then
		export VIDOW_TEST_ACTION="$2"
	else
		unset VIDOW_TEST_ACTION
	fi
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

point_at() { # point_at <layout_x> <layout_w>  (光标移到那块屏中间)
	local nx=$(( $1 * 10000 / $2 ))
	./build/vdrag "$nx" 5000 "$nx" 5000 >/dev/null 2>&1
	sleep 0.3
}

# 一个 pattern 窗口画在哪块屏上：在它自己的 place 矩形里采样四象限。
quad_check() { # quad_check <png> <view> <expect: shown|hidden>
	python3 - "$RUNDIR" "$1" "$2" "$3" <<'PY'
import re, sys
from PIL import Image
rundir, png, view, expect = sys.argv[1:5]
im = Image.open("%s/%s" % (rundir, png)).convert("RGB")
rect = None
for line in open("%s/log" % rundir, encoding="utf-8", errors="replace"):
	m = re.search(r"place %s -> (-?\d+),(-?\d+) (\d+)x(\d+)" % re.escape(view), line)
	if m:
		rect = tuple(int(v) for v in m.groups())
if rect is None:
	print("NORECT")
	sys.exit(2)
x, y, w, h = rect
quad = [((1, 1), (255, 0, 0)), ((3, 1), (0, 255, 0)),
        ((1, 3), (0, 0, 255)), ((3, 3), (255, 255, 255))]
hits = 0
samples = 0
for (fx, fy), want in quad:
	px, py = x + w * fx // 4, y + h * fy // 4
	if px < 0 or py < 0 or px >= im.size[0] or py >= im.size[1]:
		print("OFF")
		sys.exit(2)
	got = im.getpixel((px, py))
	samples += 1
	if max(abs(a - b) for a, b in zip(got, want)) <= 24:
		hits += 1
shown = hits >= 3
print("SHOWN" if shown else "HIDDEN")
sys.exit(0 if shown == (expect == "shown") else 1)
PY
}

shoot() { grim "$RUNDIR/$1.png" >/dev/null 2>&1; }

# 动作管道：一行一条，vidow 立刻执行 —— 测试不用碰键盘，时序也可控
mkpipe() {
	rm -f "$RUNDIR/actions"
	mkfifo "$RUNDIR/actions"
	exec 9<> "$RUNDIR/actions"
}
act() { echo "$1" >&9; sleep 1.0; }

# ---------------------------------------------------------------- 单屏工作区
mark "单屏：工作区 1 / 2 各开各的窗口"
mkpipe
start_vidow 1 "file:$RUNDIR/actions"
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
python3 tools/pattern.py a1 >/dev/null 2>&1 &
for _ in $(seq 1 24); do
	grep -aq "place a1 -> " "$LOG" && break
	sleep 0.25
done
sleep 1.0
shoot ws1
grep -aE "place [ab][0-9] -> |config.lua: |workspace: |keybind: |test-action" \
	"$LOG" > "$RUNDIR/log-wsA.txt"

# 切到工作区 2，在那边开两个（这样两块工作区的列数不一样，像素能分开认）
act "workspace 2"
python3 tools/pattern.py b1 >/dev/null 2>&1 &
sleep 0.9
python3 tools/pattern.py b2 >/dev/null 2>&1 &
for _ in $(seq 1 24); do
	grep -aq "place b2 -> " "$LOG" && break
	sleep 0.25
done
sleep 1.0
shoot ws2
cp "$LOG" "$RUNDIR/log-wsB.txt"

# 切回工作区 1：a1 应该原样回来，b1/b2 不该画
act "workspace 1"
sleep 0.8
shoot ws1b
cp "$LOG" "$RUNDIR/log-wsC.txt"

# 再切到 2：它的两列还在（列宽、顺序都留着）
act "workspace 2"
sleep 0.8
cp "$LOG" "$RUNDIR/log-wsD.txt"
stop_clients
exec 9>&-
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6

# ---------------------------------------------------------------- 特殊工作区 + 双屏
mark "双屏：特殊工作区跟随聚焦的屏"
mkpipe
start_vidow 2 "file:$RUNDIR/actions"
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
point_at 640 2560
python3 tools/pattern.py left-a >/dev/null 2>&1 &
sleep 0.9
point_at 1920 2560
sleep 0.3
python3 tools/pattern.py right-a >/dev/null 2>&1 &
sleep 1.0
shoot two-normal
act "move scratch"
shoot two-moved
act "special scratch"
shoot two-special
act "special scratch"
shoot two-after
cp "$LOG" "$RUNDIR/log-two.txt"
stop_clients
exec 9>&-
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6

# ---------------------------------------------------------------- 热插拔
mark "热插拔：屏拔了工作区跟着列走"
mkpipe
# 钩子要在进程起来之前设好（它在 output_init 里读一次）
# 拔屏的延时留够：gone-1 那个窗口要**先 map 上来**（GTK 起窗口偶尔要 1 秒多，
# 实测有过「窗口刚 map，屏已经被拔了」→ 拔掉的那块屏上 0 列，测试当场红）
VIDOW_TEST_OUTPUTS_X="6500:remove:right"
start_vidow 2 "file:$RUNDIR/actions"
python3 tools/wall.py >/dev/null 2>&1 &
sleep 0.8
point_at 640 2560
python3 tools/pattern.py keep-1 >/dev/null 2>&1 &
sleep 0.9
point_at 1920 2560
sleep 0.3
python3 tools/pattern.py gone-1 >/dev/null 2>&1 &
sleep 1.0
shoot hotplug-before
sleep 4.0
shoot hotplug-gone
cp "$LOG" "$RUNDIR/log-hotplug.txt"
# 拔完再切回工作区 1：那块工作区里的窗口还在，切回来就露出来
act "workspace 1"
shoot hotplug-back
cp "$LOG" "$RUNDIR/log-hotplug2.txt"
stop_clients
exec 9>&-
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
sleep 0.6

leftover=0
for pid in $VIDOW_PIDS; do
	kill -0 "$pid" 2>/dev/null && leftover=$((leftover + 1))
done
[ "$leftover" -eq 0 ] && pass "测试起的合成器都收干净了" ||
	fail "还有 $leftover 个测试起的 vidow 没退"

# ---------------------------------------------------------------- 量结果
python3 - "$RUNDIR" <<'ASSERT_PY'
import re, sys
from PIL import Image

rundir = sys.argv[1]
failed = []

def log(name):
	return open("%s/%s" % (rundir, name), encoding="utf-8", errors="replace").read()

def check(ok, what):
	print(("  PASS  " if ok else "  FAIL  ") + what)
	if not ok:
		failed.append(what)

def image(name):
	return Image.open("%s/%s.png" % (rundir, name)).convert("RGB")

def places(text, prefix):
	out = {}
	for line in text.splitlines():
		m = re.search(r"place (\S+) -> (-?\d+),(-?\d+) (\d+)x(\d+)", line)
		if m and m.group(1).startswith(prefix):
			out[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
	return out

def rect_of(text, view):
	r = None
	for line in text.splitlines():
		m = re.search(r"place %s -> (-?\d+),(-?\d+) (\d+)x(\d+)" % view, line)
		if m:
			r = tuple(int(v) for v in m.groups())
	return r

QUAD = [((1, 1), (255, 0, 0)), ((3, 1), (0, 255, 0)),
        ((1, 3), (0, 0, 255)), ((3, 3), (255, 255, 255))]

def blank(im, rect):
	"""这块区域里没有任何 pattern 窗口（四象限颜色一个都不出现）。"""
	if drawn(im, rect) is None:
		return None
	return drawn(im, rect) is False

def drawn(im, rect):
	x, y, w, h = rect
	if x < 0 or y < 0 or x + w > im.size[0] or y + h > im.size[1]:
		return None
	hits = 0
	for (fx, fy), want in QUAD:
		got = im.getpixel((x + w * fx // 4, y + h * fy // 4))
		if max(abs(a - b) for a, b in zip(got, want)) <= 24:
			hits += 1
	return hits >= 3

wsA, wsB, wsC, wsD = (log("log-wsA.txt"), log("log-wsB.txt"),
	log("log-wsC.txt"), log("log-wsD.txt"))
two, hot = log("log-two.txt"), log("log-hotplug.txt")

print()
print("== 配置建的工作区 ==")
m = re.search(r"config\.lua: list=(.*)", wsA)
check(m is not None, "config.lua 跑通了 vidow.workspace_list()")
listed = m.group(1) if m else ""
names = [x.rstrip("*") for x in listed.split(",") if x]
check("scratch" not in names, "特殊工作区不进 workspace_list()（列表：%s）" % listed)
for n in ("1", "2", "3", "4"):
	check(n in names, "普通工作区 %s 在列表里" % n)
check(re.search(r"config\.lua: active=1", wsA) is not None,
	"起来之后活动工作区是 1")
check("workspace: 建特殊工作区 scratch" in wsA, "特殊工作区是配置建的")
check("workspace: 建具名工作区 4（" in wsA and
	"workspace: 建具名工作区 2（" in wsA,
	"四个普通工作区都建出来了（配置里的 1–4，全是具名的）")
for key in ("Mod+1", "Mod+2", "Mod+Shift+1", "Mod+grave", "Mod+Shift+grave",
		"Mod+Tab"):
	check(("keybind: %s ->" % key) in wsA,
		"键位 %s 由配置绑上（合成器不预设）" % key)

print()
print("== 切工作区：只显示那一块 ==")
check(re.search(r"workspace: 切到 \S+ 上的第 2 块\S*（本区 \d+ 列", wsB) is not None,
	"切到工作区 2（动作走的是和键位同一个入口）")
m = [x for x in re.finditer(
	r"workspace: 切到 \S+ 上的第 (\d+) 块\S*（本区 (\d+) 列 / (\d+) 个窗口，全屏 (\d+) 个可见）",
	wsD) if x.group(1) == "2"]
m = m[-1] if m else None
check(m is not None, "再切回工作区 2")
if m:
	check(int(m.group(2)) == 2, "工作区 2 里是 2 列（%s）" % m.group(2))
	check(int(m.group(3)) == 2, "工作区 2 里 2 个窗口都在（%s）" % m.group(3))
	check(int(m.group(4)) == 2, "切过去之后全屏只有它那 %s 个可见" % m.group(4))
m = [x for x in re.finditer(
	r"workspace: 切到 \S+ 上的第 (\d+) 块\S*（本区 (\d+) 列 / (\d+) 个窗口，全屏 (\d+) 个可见）",
	wsC) if x.group(1) == "1"]
m = m[-1] if m else None
check(m is not None, "又切回了工作区 1")
if m:
	check(int(m.group(3)) == 1, "切回 1：本区 %s 个窗口（原来那个）" % m.group(3))
	check(int(m.group(4)) == 1, "切回 1：全屏只有它 %s 个可见（2 上的藏起来了）" % m.group(4))

pa, pb = places(wsA, "a"), places(wsB, "b")
check(len(pa) == 1 and len(pb) == 2, "工作区 1 上 1 个窗口、工作区 2 上 2 个（%d + %d）"
	% (len(pa), len(pb)))
# 两块工作区的**列数**不一样，所以像素能分开认：
#   第 1 列（x≈6）两块工作区都有窗口 —— 认不出是谁，不断言；
#   第 2 列（x≈646）只有工作区 2 有 —— 那一块是「属于谁」的判据。
a1 = pa.get("a1")
b2 = pb.get("b2")
check(a1 is not None and b2 is not None, "两个窗口的位置都记下来了")
first, second = (a1, b2) if a1 is not None and b2 is not None else (None, None)
im_a, im_b, im_c = image("ws1"), image("ws2"), image("ws1b")
if first is not None:
	check(drawn(im_a, first) is True, "工作区 1 上 a1 画着")
	check(drawn(im_b, first) is True, "工作区 2 上那块位置是 b1 画的（同一个坑位）")
	check(drawn(im_c, first) is True, "切回工作区 1 后 a1 还画着（窗口没被杀）")
if second is not None:
	check(drawn(im_a, second) is False,
		"工作区 1 上第 2 列是空的（b2 属于工作区 2，不画）")
	check(drawn(im_b, second) is True, "工作区 2 上 b2 画在第 2 列")
	check(drawn(im_c, second) is False, "切回工作区 1 后 b2 不画（它属于 2）")

print()
print("== 特殊工作区：不进列表、跟随聚焦的屏 ==")
check("workspace: 把 right-a 搬到 scratch（特殊工作区）" in two,
	"把右屏那个窗口搬进了特殊工作区")
rect, rectL = rect_of(two, "right-a"), rect_of(two, "left-a")
norm, moved = image("two-normal"), image("two-moved")
check(rect is not None and drawn(norm, rect) is True, "搬之前它画在右屏上")
check(rect is not None and drawn(moved, rect) is False,
	"搬进特殊工作区之后，普通工作区上看不到它了")
check(rectL is not None and drawn(moved, rectL) is True,
	"这时候左屏那块（别的屏）照旧画着")
spec = image("two-special")
check(rect is not None and drawn(spec, rect) is True,
	"打开特殊工作区后又画上了（显示在它原来那块屏上）")
check(rectL is not None and drawn(spec, rectL) is True,
	"别的屏不受影响，还显示自己的普通工作区")
m = re.search(r"workspace: 打开特殊工作区 scratch（显示在 (\S+) 上，本区 (\d+) 个窗口，全屏 (\d+) 个可见", two)
check(m is not None, "日志报了特殊工作区显示在哪块屏上")
if m:
	check(m.group(1) == "HEADLESS-1", "显示在**聚焦**的那块屏上（%s）" % m.group(1))
	check(int(m.group(2)) == 1, "特殊工作区里 %s 个窗口" % m.group(2))
	check(int(m.group(3)) == 2, "全屏 %s 个可见（另一块屏的窗口也在）" % m.group(3))
m = re.search(r"workspace: 收起特殊工作区 scratch（本区留 (\d+) 个窗口，全屏 (\d+) 个可见）", two)
check(m is not None, "再开一次收起了特殊工作区")
if m:
	check(int(m.group(1)) == 1, "窗口留在特殊工作区里（本区 %s 个），没被搬回普通工作区" % m.group(1))
after = image("two-after")
check(rect is not None and drawn(after, rect) is False, "收起后它不画了（藏在特殊工作区里）")
check(rectL is not None and drawn(after, rectL) is True, "收起后左屏那个窗口还在画着")

print()
print("== 热插拔：屏拔了，工作区跟过去 ==")
hot2 = log("log-hotplug2.txt")
m = re.search(r"hotplug: 拔掉 (\S+)（(\d+) 列 / (\d+) 个窗口", hot)
check(m is not None, "一块屏被拔掉了（VIDOW_TEST_OUTPUTS 走真实事件路径）")
if m:
	check(int(m.group(2)) >= 1, "拔掉的屏上有 %s 列（那块屏在用它）" % m.group(2))
	check(int(m.group(3)) >= 1, "那 %s 个窗口跟着搬家（没被杀）" % m.group(3))
m = re.search(r"(\S+) 上 (\d+) 块工作区跟着列挪到 (\S+) 上住", hot)
check(m is not None, "屏上的工作区跟着列挪到了剩下的屏上")
if m:
	check(m.group(1) != m.group(3), "从 %s 挪到 %s" % (m.group(1), m.group(3)))
check("workspace: 剩下的屏先显示搬过来的那块工作区" in hot,
	"剩下的屏先显示搬过来的那块工作区（人跟着屏走）")
gone = rect_of(hot, "gone-1")
check(gone is not None, "被拔那块屏上的窗口还有位置记录（说明它还在布局里）")
im_gone = image("hotplug-gone")
check(gone is not None and drawn(im_gone, gone) is True,
	"剩下那块屏上画着它（工作区 + 窗口一起搬过来了）")
# 切回工作区 1：另一块屏上原来那个窗口还在，切回来就露出来。
# 两个窗口都在最左边那一列（几何一样），所以像素认不出是谁 —— 用第 2 列那块空区域
# 当判据：只剩一块工作区可见时，第 2 列必须是空的（壁纸）。
second_half = (640, 6, 640, 708)
check(blank(im_gone, second_half) is True,
	"拔线后第 2 列是空的（只剩搬过来的那块工作区可见）")
im_back = image("hotplug-back")
keep = rect_of(hot2, "keep-1")
check(keep is not None and drawn(im_back, keep) is True,
	"切回工作区 1：原来那块屏上的窗口还在（工作区没被拔线搞丢）")
check(blank(im_back, second_half) is True,
	"切回工作区 1 后第 2 列还是空的（搬过来的那块藏起来了）")
m = [x for x in re.finditer(
	r"workspace: 切到 \S+ 上的第 1 块\S*（本区 (\d+) 列 / (\d+) 个窗口，全屏 (\d+) 个可见）",
	hot2)]
m = m[-1] if m else None
check(m is not None and int(m.group(3)) == 1,
	"切回工作区 1 后全屏只有它 1 个可见（%s）" % (m.group(3) if m else "?"))

print()
if failed:
	print("test-workspace.sh: %d 项没通过" % len(failed))
	for f in failed:
		print("  - " + f)
	sys.exit(1)
print("全部通过 / test-workspace.sh: OK")
ASSERT_PY
