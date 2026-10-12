#!/bin/bash
# 焦点跳转 / 激活请求：窗口规则管「这个应用开窗口抢不抢焦点」「别人请求激活它时
# 理不理」，激活时先把窗口所在的工作区端出来，以及激活令牌多久作废。
#
# 要解决的两类毛病（都来自真实客户端）：
#   A) 抢焦点：Electron 系应用会为了通知 / 剪贴板开一个看不见的辅助窗口，一开就把
#      焦点从你正在打字的窗口上抢走。规则 focus_on_open = false 让它别抢。
#   B) 顶不上来：点了通知，窗口不动。三个原因，都在这条链路上：
#      B1 令牌过期 —— wlroots 默认 30 秒作废，而通知常常是几十分钟前弹的、
#         现在才点；令牌没了请求直接被丢掉（activation_token_timeout 默认不过期）。
#      B2 窗口在藏起来的工作区上 —— 以前激活只把键盘焦点挪过去，窗口还是藏的，
#         看起来就是「点了没反应」。现在先把它的工作区端出来（跨屏也一样）。
#      B3 serial 校验不过 —— 客户端 set_serial 给的 serial 要是「从来没发给过它」
#         （工具包偷懒写 0），wlroots 那份 xdg-activation 就不登记这个令牌，后面
#         activate 当成未知令牌丢掉。niri 专门有个开关管这种情况
#         （honor-xdg-activation-with-invalid-serial，在它配置的 debug 段里），
#         vidow 同名选项 honor_xdg_activation_with_invalid_serial：默认不理
#         （= wlroots 一直以来的行为），打开就照样理。为此协议实现从 wlroots
#         手里收回到 activation.c —— 校验规则得在自己手里才配得动。
#
# 判据（日志 + 客户端自己的输出 + 像素）：
#   1) 基线：新窗口默认抢焦点（焦点日志一行一行看得见换人）
#   2) focus_on_open = false：开窗口不抢焦点，但窗口照常画出来（规则按 app_id=demo 匹配）
#   3) 激活请求（xdg-activation，tools/activate.c）把藏起来的工作区端出来 +
#      给焦点 + 窗口真的画在屏幕上（像素）；跨屏 / 特殊工作区同理
#   4) 令牌过期：activation_token_timeout = 1 秒 + 等 2 秒再请求 → 请求被丢；
#      默认（0 = 不过期）同一个请求能过
#   5) focus_on_activate = false：激活请求直接不理（日志明说）
#   6) 窗口还没上屏就请求激活：记下来，等它出来再聚焦
#   7) 守护进程式激活：令牌不带 surface，被激活的应用在 activate 里带自己的
#      → 认的是 activate 里那个（通知点击就是这个流程）
#   8) serial 校验不过（set_serial(0, seat)）：默认令牌照发、激活被忽略；
#      开关打开 → 同一个请求就理它（对照实验，跟 5) 一样是「配一行就变」）
#   9) 同一个开关的**按应用**版本：窗口规则 honor_invalid_serial = true 只给这个应用
#      开口子（同一个时序，规则外的应用还是不理）—— 不想全局放开就用它
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-focus-test
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

# 这个 pid 是不是**本测试起的**：沿父进程链往上找 $$。只看命令行会误伤 —— 从别的
# 脚本里调这个测试时，外面那层 shell 的命令行里可能就带着 build/activate 这几个字，
# 误杀了会让测试自己莫名其妙被 SIGTERM 掉。
is_ours() {
	local pid=$1
	local i
	for i in $(seq 1 10); do
		if [ "$pid" = "$$" ]; then return 0; fi
		if [ -z "$pid" ] || [ "$pid" = "0" ] || [ "$pid" = "1" ]; then return 1; fi
		pid=$(awk '/^PPid:/{print $2}' "/proc/$pid/status" 2>/dev/null)
	done
	return 1
}

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*wall.py*|*build/activate*|*build/frames*) ;;
		*) continue ;;
		esac
		is_ours "$pid" && kill "$pid" 2>/dev/null
	done 2>/dev/null
}

command -v grim >/dev/null || { echo "test-focus.sh: 需要 grim"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-focus.sh: 需要 python3-gobject"; exit 1; }

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
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

start_vidow() { # start_vidow [test-actions]
	export WLR_HEADLESS_OUTPUTS=1
	unset VIDOW_TEST_OUTPUTS
	rm -f "$LOG"
	if [ -n "${1:-}" ]; then
		export VIDOW_TEST_ACTION="$1"
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

stop_vidow() {
	kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.6
}

mkpipe() {
	rm -f "$RUNDIR/actions"
	mkfifo "$RUNDIR/actions"
	exec 9<> "$RUNDIR/actions"
}
act() { echo "$1" >&9; sleep 1.2; }

shoot() { grim "$RUNDIR/$1.png" >/dev/null 2>&1; }

# 窗口的画面点：map 日志给了表面尺寸（客户端画的是固定 360x240 的图案），
# place 日志给了它在布局里的位置。取表面里 (w/4, h/4) —— 那个象限是纯红。
view_point() { # view_point <标题> → "x y"（布局坐标，探测/点击都用它）
	python3 - "$LOG" "$1" <<'PY'
import re, sys
log, title = sys.argv[1], sys.argv[2]
text = open(log, encoding="utf-8", errors="replace").read()
place = re.findall(r"place %s -> (-?\d+),(-?\d+) (\d+)x(\d+)" % re.escape(title), text)
surf = re.findall(r"map %s: surface (\d+)x(\d+)" % re.escape(title), text)
if not place or not surf:
	print("NOPOINT")
	sys.exit(2)
x, y = int(place[-1][0]), int(place[-1][1])
w, h = int(surf[-1][0]), int(surf[-1][1])
print("%d %d" % (x + w // 4, y + h // 4))
PY
}

click_at() { # click_at <布局 x> <布局 y>
	local nx=$(( $1 * 10000 / 1280 ))
	local ny=$(( $2 * 10000 / 720 ))
	./build/vdrag "$nx" "$ny" "$nx" "$ny" >/dev/null 2>&1
	sleep 0.5
}

# 等客户端把激活请求发出去（它自己打印 activate committed）
wait_committed() { # wait_committed <文件> [超时秒]
	local file="$1" limit="${2:-12}"
	for _ in $(seq 1 $((limit * 4))); do
		grep -aq "^activate committed" "$file" 2>/dev/null && return 0
		sleep 0.25
	done
	return 1
}

open_pattern() { # open_pattern <标题>
	python3 tools/pattern.py "$1" >/dev/null 2>&1 &
	for _ in $(seq 1 24); do
		grep -aq "place $1 -> " "$LOG" && break
		sleep 0.25
	done
	sleep 0.5
}

# ------------------------------------------------------------ 1) 基线
mark "基线：没有规则 —— 新窗口默认抢焦点"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
EOF
start_vidow
open_pattern a1
open_pattern a2
cp "$LOG" "$RUNDIR/log-1.txt"
stop_clients
stop_vidow

# ------------------------------------------------------------ 2) 开窗口不抢焦点
mark "规则 focus_on_open = false：窗口照常出现，但不抢键盘"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
vidow.rule{ app_id = "demo", focus_on_open = false }
for i, r in ipairs(vidow.rules()) do
	vidow.log(string.format("rule %d: app_id=%s focus_on_open=%s focus_on_activate=%s",
		i, r.app_id, tostring(r.focus_on_open), tostring(r.focus_on_activate)))
end
EOF
start_vidow
open_pattern a1
./build/activate demo 演示 30 --serial=none > "$RUNDIR/act-2.txt" 2>&1 &
for _ in $(seq 1 24); do
	grep -aq "place 演示 -> " "$LOG" && break
	sleep 0.25
done
sleep 0.8
shoot s2
grep -ao "map 演示: surface [0-9]*x[0-9]*" "$LOG" > "$RUNDIR/surf-2.txt"
view_point 演示 > "$RUNDIR/point-2.txt"
cp "$LOG" "$RUNDIR/log-2.txt"
stop_clients
stop_vidow

# ------------------------------------------------------------ 3) 激活：藏起来的工作区
mark "激活请求：窗口在藏起来的工作区上 —— 先端出来再给焦点"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
vidow.rule{ app_id = "demo", focus_on_open = false }
EOF
mkpipe
start_vidow "file:$RUNDIR/actions"
./build/activate demo 演示 8 --serial=click > "$RUNDIR/act-3.txt" 2>&1 &
for _ in $(seq 1 24); do
	grep -aq "place 演示 -> " "$LOG" && break
	sleep 0.25
done
sleep 0.5
POINT=$(view_point 演示)
echo "  窗口画面点：$POINT"
click_at $POINT
grep -aE "点击 button" "$LOG" | tail -1 > "$RUNDIR/click-3.txt"
act "move 2"
act "workspace 1"
shoot s3-hidden
cp "$LOG" "$RUNDIR/log-3a.txt"
wait_committed "$RUNDIR/act-3.txt" 12 || true
sleep 1.2
shoot s3-shown
cp "$LOG" "$RUNDIR/log-3.txt"
view_point 演示 > "$RUNDIR/point-3.txt"
stop_clients
stop_vidow

# ------------------------------------------------------------ 4) 激活：特殊工作区
mark "激活请求：窗口在特殊工作区上 —— 把那块特殊工作区开到它那块屏上"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
vidow.workspace_special("scratch")
vidow.rule{ app_id = "demo", focus_on_open = false }
EOF
mkpipe
start_vidow "file:$RUNDIR/actions"
./build/activate demo 演示 8 --serial=click > "$RUNDIR/act-4.txt" 2>&1 &
for _ in $(seq 1 24); do
	grep -aq "place 演示 -> " "$LOG" && break
	sleep 0.25
done
sleep 0.5
POINT=$(view_point 演示)
click_at $POINT
act "move scratch"
shoot s4-hidden
wait_committed "$RUNDIR/act-4.txt" 12 || true
sleep 1.2
shoot s4-shown
cp "$LOG" "$RUNDIR/log-4.txt"
view_point 演示 > "$RUNDIR/point-4.txt"
stop_clients
stop_vidow

# ------------------------------------------------------------ 5) 令牌过期
mark "激活令牌：过期（1 秒）就被丢掉；默认不过期（0）就放行"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
activation_token_timeout = 1
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
EOF
start_vidow
./build/activate demo 演示 2 --serial=none > "$RUNDIR/act-5a.txt" 2>&1 &
wait_committed "$RUNDIR/act-5a.txt" 10 || true
sleep 1.5
cp "$LOG" "$RUNDIR/log-5a.txt"
stop_clients
stop_vidow

cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
activation_token_timeout = 0
EOF
start_vidow
./build/activate demo 演示 2 --serial=none > "$RUNDIR/act-5b.txt" 2>&1 &
wait_committed "$RUNDIR/act-5b.txt" 10 || true
sleep 1.5
cp "$LOG" "$RUNDIR/log-5b.txt"
stop_clients
stop_vidow

# ------------------------------------------------------------ 6) 不理激活请求
mark "规则 focus_on_activate = false：激活请求直接不理"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
vidow.rule{ app_id = "demo", focus_on_activate = false }
EOF
start_vidow
open_pattern a1
./build/activate demo 演示 2 --serial=none > "$RUNDIR/act-6.txt" 2>&1 &
wait_committed "$RUNDIR/act-6.txt" 10 || true
sleep 1.5
cp "$LOG" "$RUNDIR/log-6.txt"
stop_clients
stop_vidow

# ------------------------------------------------------------ 7) 还没上屏就请求
mark "窗口还没上屏就请求激活：记下来，等它出来再聚焦"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
EOF
start_vidow
./build/activate demo 演示 0 --serial=none --before-map > "$RUNDIR/act-7.txt" 2>&1 &
for _ in $(seq 1 24); do
	grep -aq "place 演示 -> " "$LOG" && break
	sleep 0.25
done
sleep 1.0
cp "$LOG" "$RUNDIR/log-7.txt"
stop_clients
stop_vidow

# ------------------------------------------------- 8) 守护进程式激活（令牌不带 surface）
mark "守护进程式激活：令牌不带 surface，被激活的应用在 activate 里带自己的"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
EOF
start_vidow
open_pattern a1
./build/activate demo 演示 0 --serial=none --no-token-surface \
	> "$RUNDIR/act-8.txt" 2>&1 &
wait_committed "$RUNDIR/act-8.txt" 10 || true
sleep 1.0
cp "$LOG" "$RUNDIR/log-8.txt"
stop_clients
stop_vidow

# ------------------------------------------- 9) serial 校验不过（默认不理）
mark "serial 校验不过（set_serial(0, seat)）：默认令牌照发，但激活被忽略"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
EOF
start_vidow
open_pattern a1
./build/activate demo 演示 0 --serial=zero > "$RUNDIR/act-9.txt" 2>&1 &
wait_committed "$RUNDIR/act-9.txt" 10 || true
sleep 1.0
cp "$LOG" "$RUNDIR/log-9.txt"
stop_clients
stop_vidow

# ------------------------------------------- 10) 同一个请求 + 开关打开
mark "honor_xdg_activation_with_invalid_serial = 1：同一个请求就理它了"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
honor_xdg_activation_with_invalid_serial = true
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
EOF
start_vidow
open_pattern a1
./build/activate demo 演示 0 --serial=zero > "$RUNDIR/act-10.txt" 2>&1 &
wait_committed "$RUNDIR/act-10.txt" 10 || true
sleep 1.0
cp "$LOG" "$RUNDIR/log-10.txt"
stop_clients
stop_vidow

# ---------------------------------- 11) 窗口规则 honor_invalid_serial（按应用开口子）
mark "窗口规则 honor_invalid_serial = true：只给这个应用开口子，别的应用还是不理"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
vidow.rule{ app_id = "demo", honor_invalid_serial = true }
for i, r in ipairs(vidow.rules()) do
	vidow.log(string.format("rule %d: app_id=%s honor_invalid_serial=%s",
		i, r.app_id, tostring(r.honor_invalid_serial)))
end
EOF
start_vidow
open_pattern a1
./build/activate demo 演示 0 --serial=zero > "$RUNDIR/act-11a.txt" 2>&1 &
wait_committed "$RUNDIR/act-11a.txt" 10 || true
sleep 1.0
cp "$LOG" "$RUNDIR/log-11a.txt"
./build/activate other 别家 0 --serial=zero > "$RUNDIR/act-11b.txt" 2>&1 &
wait_committed "$RUNDIR/act-11b.txt" 10 || true
sleep 1.0
cp "$LOG" "$RUNDIR/log-11.txt"
stop_clients
stop_vidow

leftover=0
for pid in $VIDOW_PIDS; do
	kill -0 "$pid" 2>/dev/null && leftover=$((leftover + 1))
done
[ "$leftover" -eq 0 ] && pass "测试起的合成器都收干净了" ||
	fail "还有 $leftover 个测试起的 vidow 没退"

# ---------------------------------------------------------------- 量结果
python3 - "$RUNDIR" <<'ASSERT_PY'
import re
import sys
from PIL import Image

rundir = sys.argv[1]
failed = []

def log(name):
	return open("%s/%s" % (rundir, name), encoding="utf-8",
		errors="replace").read()

def out(name):
	return open("%s/%s" % (rundir, name), encoding="utf-8",
		errors="replace").read()

def check(ok, what):
	print(("  PASS  " if ok else "  FAIL  ") + what)
	if not ok:
		failed.append(what)

def pixel(png, point):
	x, y = (int(v) for v in point.split())
	im = Image.open("%s/%s.png" % (rundir, png)).convert("RGB")
	return im.getpixel((x, y))

def point_of(name):
	text = open("%s/%s" % (rundir, name), encoding="utf-8",
		errors="replace").read().strip()
	return text

RED = (255, 0, 0)
# 窗口没画出来时看到的是兜底色（background_color 默认 #14161c = 20,22,28）
DESKTOP = (20, 22, 28)

def near(got, want, tol=24):
	return max(abs(a - b) for a, b in zip(got, want)) <= tol

print()
print("== 1) 基线：新窗口默认抢焦点 ==")
b = log("log-1.txt")
check(re.search(r"焦点: \(无\) -> a1", b) is not None,
	"第一个窗口拿到焦点")
check(re.search(r"焦点: a1 -> a2", b) is not None,
	"第二个窗口把焦点抢过去（默认行为）")
check("不抢焦点" not in b, "基线里没有「不抢焦点」这回事")

print()
print("== 2) focus_on_open = false：不抢键盘，但窗口照常出现 ==")
b = log("log-2.txt")
check(re.search(r"rule: app_id=demo .*focus_on_open=false", b)
	is not None, "规则读进来了（日志里写了 focus_on_open=false）")
check(re.search(r"rule 1: app_id=demo focus_on_open=false focus_on_activate=nil",
	b) is not None, "没写的字段查回来是 nil（「没管它」≠「管了、给了默认值」）")
check("map: 演示 不抢焦点（窗口规则 focus_on_open = false）" in b,
	"开窗口时日志明说不抢焦点")
check(re.search(r"焦点: .* -> 演示", b) is None,
	"焦点从头到尾没给过这个窗口")
check(re.search(r"焦点: \(无\) -> a1", b) is not None, "a1 还是焦点窗口")
p = point_of("point-2.txt")
check(near(pixel("s2", p), RED), "窗口照常画在屏幕上（像素 %s）" % (pixel("s2", p),))

print()
print("== 3) 激活请求：藏起来的工作区 ==")
b = log("log-3a.txt")
check(re.search(r"点击 button \d+ 在 \d+,\d+：命中 演示", b) is not None,
	"先点一下窗口（真实应用就是拿这次点击的 serial 去请求激活的）")
check(re.search(r"activate click \d+", out("act-3.txt")) is not None,
	"客户端收到了点击（座位能力里得有指针，否则它永远收不到）")
b = log("log-3.txt")
check(re.search(r"activate serial [1-9]\d*（点击事件）", out("act-3.txt"))
	is not None, "激活请求带上了真实 serial")
check("activation: 激活 演示" in b, "合成器收到了激活请求（xdg-activation）")
check(re.search(r"激活: 演示 在藏起来的 2 上，先把它端出来", b) is not None,
	"先把窗口那块工作区端出来")
check(re.search(r"workspace: 切到 \S+ 上的第 2 块", b) is not None,
	"工作区真的切过去了")
check(re.search(r"焦点: .* -> 演示", b) is not None, "焦点给了它")
p = point_of("point-3.txt")
check(near(pixel("s3-hidden", p), DESKTOP),
	"激活之前它确实藏起来了（像素 %s）" % (pixel("s3-hidden", p),))
check(near(pixel("s3-shown", p), RED),
	"激活之后它画在屏幕上（像素 %s）" % (pixel("s3-shown", p),))

print()
print("== 4) 激活请求：特殊工作区 ==")
b = log("log-4.txt")
check("activation: 激活 演示" in b, "合成器收到了激活请求")
check(re.search(r"激活: 演示 在特殊工作区 scratch 上，把它开到 HEADLESS-1", b)
	is not None, "把特殊工作区开到窗口那块屏上")
check(re.search(r"workspace: 打开特殊工作区 scratch（显示在 HEADLESS-1 上", b)
	is not None, "日志里写清了它显示在哪块屏上")
p = point_of("point-4.txt")
check(near(pixel("s4-hidden", p), DESKTOP),
	"激活之前它藏在特殊工作区里（像素 %s）" % (pixel("s4-hidden", p),))
check(near(pixel("s4-shown", p), RED),
	"激活之后它画在屏幕上（像素 %s）" % (pixel("s4-shown", p),))

print()
print("== 5) 激活令牌过期 ==")
a = log("log-5a.txt")
check("config: activation_token_timeout=1 秒" in a, "配置读进来了（1 秒）")
check("activation: 激活令牌 会过期" in a, "令牌会过期（日志明说）")
check("activation: 激活 演示" not in a,
	"等 2 秒再请求：令牌已作废，请求被丢掉")
check(re.search(r"令牌 \w+ 过期了（activation_token_timeout = 1 秒", a) is not None,
	"日志里看得到它是「过期了」才被丢的（能查得出原因）")
b = log("log-5b.txt")
check("config: activation_token_timeout=0 秒" in b, "配置读进来了（0 = 不过期）")
check("activation: 激活令牌 不过期" in b, "令牌不过期（日志明说）")
check("activation: 激活 演示" in b,
	"同一个「等 2 秒再请求」现在能过 —— 这就是「点了通知窗口不动」的修法")
check(re.search(r"焦点: .* -> 演示", b) is not None, "焦点给了它")

print()
print("== 6) focus_on_activate = false ==")
b = log("log-6.txt")
check(re.search(r"rule: app_id=demo .*focus_on_activate=false", b)
	is not None, "规则读进来了（focus_on_activate=false）")
check("激活: 演示 不理（窗口规则 focus_on_activate = false）" in b,
	"激活请求被规则挡下（日志明说）")
check(b.rfind("焦点: ") < b.find("激活: 演示 不理"),
	"激活请求之后焦点没再动过（请求被规则挡下的那一行是最后一条）")

print()
print("== 7) 窗口还没上屏就请求激活 ==")
b = log("log-7.txt")
check("激活: 演示 还没上屏，等它出来再聚焦" in b, "请求先记下来了")
check(re.search(r"焦点: \(无\) -> 演示", b) is not None,
	"窗口一上屏就把焦点兑现了")
check(b.find("激活: 演示 还没上屏，等它出来再聚焦") <
	b.find("焦点: (无) -> 演示"),
	"顺序是「先记下请求、窗口出来才聚焦」（而不是请求时就把焦点挪过去）")

print()
print("== 8) 守护进程式激活：令牌不带 surface ==")
b = log("log-8.txt")
check("activate token-surface 不带" in out("act-8.txt"),
	"客户端只在 activate 里带 surface（通知守护进程就这么干）")
check(re.search(r"activation: 令牌 \w+ 建好（app_id=demo 没给 serial）", b) is not None,
	"令牌建好了（没给 serial 就不校验）")
check("activation: 激活 演示" in b, "合成器认了这个请求")
check(re.search(r"焦点: a1 -> 演示", b) is not None,
	"焦点从 a1 换到被激活的窗口 —— 认的是 activate 里那个 surface")

print()
print("== 9) serial 校验不过：默认不理 ==")
b = log("log-9.txt")
check(re.search(r"^activate token \w+$", out("act-9.txt"), re.M) is not None,
	"客户端照样拿到了令牌（协议要求回 done，不像 wlroots 那样让它干等）")
check("activation: 令牌的 serial 0 不是发给这个客户端的" in b,
	"合成器看出来了：这个 serial 从来没发给过它")
check(re.search(r"的 serial 0 校验不过 → 不理", b) is not None,
	"激活请求被忽略（日志明说原因）")
check("activation: 激活 演示" not in b, "没走到「激活」那一步")
check(b.rfind("焦点: ") < b.find("的 serial 0 校验不过 → 不理"),
	"激活被忽略之后焦点没再动过（演示 拿到焦点只是开窗口时抢的那一次）")

print()
print("== 10) honor_xdg_activation_with_invalid_serial = 1 ==")
b = log("log-10.txt")
check("config: honor_xdg_activation_with_invalid_serial=on" in b,
	"配置读进来了")
check("activation: serial 校验不过的激活请求：照样理" in b, "日志明说这个开关开着")
check(re.search(r"的 serial 0 校验不过：照样理它", b) is not None,
	"同一个令牌，这次照样理")
check("activation: 激活 演示" in b, "同一个请求（serial 0）现在被认了")
check(re.search(r"焦点: a1 -> 演示", b) is not None, "焦点真的给了它")

print()
print("== 11) 窗口规则 honor_invalid_serial = true（按应用开口子）==")
b = log("log-11a.txt")
check(re.search(r"rule: app_id=demo .*honor_invalid_serial=true", b)
	is not None, "规则读进来了（日志里写了 honor_invalid_serial=true）")
check(re.search(r"rule 1: app_id=demo honor_invalid_serial=true", b) is not None,
	"vidow.rules() 查得回 honor_invalid_serial=true")
check("但这个窗口的规则写着 honor_invalid_serial = true → 照样理" in b,
	"规则匹配上的窗口：serial 0 也照样理（日志明说是因为规则，不是全局开关）")
check("activation: 激活 演示" in b, "激活真的走到「顶窗口」那一步")
check(re.search(r"焦点: a1 -> 演示", b) is not None, "焦点给了它")
b = log("log-11.txt")
check("activation: 激活 别家" not in b,
	"同一个时序、规则外的应用（别家）还是不理 —— 是「按应用」不是「全开」")
check(re.search(r"的 serial 0 校验不过 → 不理", b) is not None,
	"规则外的应用走的是「不理」那条（日志明说）")

print()
print("== 汇总 ==")
if failed:
	print("  %d 项没过：" % len(failed))
	for what in failed:
		print("    - %s" % what)
	sys.exit(1)
print("  全部通过")
ASSERT_PY
STATUS=$?
[ "$STATUS" -eq 0 ] && pass "焦点 / 激活 判据全部通过" ||
	fail "有判据没过（见上面的清单）"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "test-focus.sh: 全部通过"
else
	echo "test-focus.sh: 有失败项"
fi
exit "$FAILED"
