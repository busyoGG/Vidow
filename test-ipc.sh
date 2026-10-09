#!/bin/bash
# IPC（unix socket 上的 JSON 请求 / 应答 + 事件订阅）：状态输出够不够全、命令能不能
# 真的把东西动起来、错误路径是不是拒绝而不是崩。
#
# 三件事分开验：
#   A) 状态输出：窗口（标题 / app_id / 几何 / 列 / 工作区 / 输出 / 规则）、layer
#      surface（namespace / layer / 锚点 / 独占区）、工作区、输出、配置、键位、规则…
#      —— 而且几何要跟合成器自己的 place 日志**对得上**（不是自己编的数）。
#   B) 操作：焦点 / 工作区 / 浮动 / 尺寸 / 位置 / 列宽 / 关窗口 / 显示器开关 / 缩放 /
#      方向 / 模式 / 位置 / 运行时改配置 / 重载 / 光标 / overview —— 每一条都回读状态
#      确认真的变了；关键一条还要 grim 像素作证（画面真的动了）。
#   C) 事件订阅：window-opened / window-closed / window-moved / layer-mapped /
#      workspace-activated / output-changed… 以及订阅过滤器。
#
# 错误路径同样是判据：未知命令、未知 id、坏 JSON、超长一行，都必须「回一条 ok:false」
# 而不是断开或者崩掉（IPC 是外部输入的第一道门）。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-ipc-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
VIDOW_PIDS=""
SOCK=""
JF="$RUNDIR/jf.py"

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

# 这个 pid 是不是**本测试起的**：沿父进程链往上找 $$。只看命令行会误伤 —— 从别的
# 脚本里调这个测试时，外面那层 shell 的命令行里可能就带着 build/vidow 这几个字。
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
		*pattern.py*|*wall.py*|*vidowmsg*) ;;
		*) continue ;;
		esac
		is_ours "$pid" && kill "$pid" 2>/dev/null
	done 2>/dev/null
}

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

command -v grim >/dev/null || { echo "test-ipc.sh: 需要 grim"; exit 1; }
python3 -c "import gi" 2>/dev/null || { echo "test-ipc.sh: 需要 python3-gobject"; exit 1; }
python3 -c "from PIL import Image" 2>/dev/null || { echo "test-ipc.sh: 需要 python3-pillow"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-ipc.sh: 先 ninja -C build（vidowmsg 没编出来）"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
EOF

# ------------------------------------------------------------ 工具

# 一个最小 IPC 客户端：发一行 JSON，读一行应答。
#   jf.py <socket> <请求 JSON> eval <python 表达式>   → 对解析后的应答求值
#   jf.py <socket> <请求 JSON> box <标题> <字段名>    → 那个窗口的 "x y w h"
#   jf.py <socket> <请求 JSON> raw                    → 原样打印应答行
cat > "$JF" <<'PY'
import json, socket, sys

path, req = sys.argv[1], sys.argv[2]
mode = sys.argv[3] if len(sys.argv) > 3 else "raw"

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(path)
s.sendall((req + "\n").encode("utf-8"))
buf = b""
while b"\n" not in buf:
    chunk = s.recv(65536)
    if not chunk:
        break
    buf += chunk
line = buf.split(b"\n")[0].decode("utf-8", "replace")

if mode == "raw":
    sys.stdout.write(line + "\n")
    sys.exit(0)

resp = json.loads(line)
if mode == "box":
    title, field = sys.argv[4], sys.argv[5]
    win = [w for w in resp["result"] if w["title"] == title][0]
    box = win[field] or win["geometry"]
    print(box["x"], box["y"], box["width"], box["height"])
else:
    try:
        print(eval(sys.argv[4], {"resp": resp, "json": json}))
    except Exception as exc:
        print("ERR:%s" % exc)
PY

ipc() { python3 "$JF" "$SOCK" "$1" raw; }
jf() { python3 "$JF" "$SOCK" "$1" eval "$2"; }
box() { python3 "$JF" "$SOCK" "$1" box "$2" "$3"; }

# 对一个**已经拿到**的应答求值。check 只发一次请求 —— toggle 这类命令发两次就是两个
# 相反的结果，测试会自己把自己绕晕（踩过）。
eval_resp() { python3 -c '
import json, sys
resp = json.loads(sys.argv[1])
try:
    print(eval(sys.argv[2], {"resp": resp, "json": json}))
except Exception as exc:
    print("ERR:%s" % exc)' "$1" "$2"; }

# check <描述> <请求> <python 表达式> <期望>
check() {
	local desc="$1" req="$2" expr="$3" want="$4"
	local resp got
	resp=$(ipc "$req")
	got=$(eval_resp "$resp" "$expr")
	if [ "$got" = "$want" ]; then
		pass "$desc"
	else
		fail "$desc（得到 '$got'，期望 '$want'）"
		echo "     应答：$(printf '%s' "$resp" | head -c 300)"
	fi
}

# checkp <描述> <请求> <表达式> <前缀>（长文本用）
checkp() {
	local desc="$1" req="$2" expr="$3" want="$4"
	local resp got
	resp=$(ipc "$req")
	got=$(eval_resp "$resp" "$expr")
	case "$got" in
	"$want"*) pass "$desc" ;;
	*) fail "$desc（得到 '$got'，期望以 '$want' 开头）" ;;
	esac
}

start_vidow() { # start_vidow [headless 输出块数]
	export WLR_HEADLESS_OUTPUTS="${1:-1}"
	rm -f "$LOG"
	unset VIDOW_TEST_ACTION
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
	SOCK="$XDG_RUNTIME_DIR/vidow-$WAYLAND_DISPLAY.sock"
	for _ in $(seq 1 20); do
		[ -S "$SOCK" ] && break
		sleep 0.1
	done
}

stop_vidow() {
	kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.8
}

open_pattern() { # open_pattern <标题>
	python3 tools/pattern.py "$1" >/dev/null 2>&1 &
	for _ in $(seq 1 24); do
		grep -aq "place $1 -> " "$LOG" && break
		sleep 0.25
	done
	sleep 0.6
}

# 日志里的 place 几何（跟 IPC 报的对账用）
place_of() { # place_of <标题> → "x y w h"
	python3 - "$LOG" "$1" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.findall(r"place %s -> (-?\d+),(-?\d+) (\d+)x(\d+)" % re.escape(sys.argv[2]), text)
print(" ".join(m[-1]) if m else "NOPLACE")
PY
}

surface_of() { # surface_of <标题> → "w h"
	python3 - "$LOG" "$1" <<'PY'
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.findall(r"map %s: surface (\d+)x(\d+)" % re.escape(sys.argv[2]), text)
print("%s %s" % m[-1] if m else "NOSURFACE")
PY
}

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

# ------------------------------------------------------------ 1) 起来 + 版本
mark "IPC 起来了：socket 在，版本能查"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_move_ms = 60
for i = 1, 3 do vidow.workspace_create(tostring(i)) end
vidow.workspace_special("scratch")
vidow.bind("Mod+1", "workspace 1")
vidow.rule{ app_id = "pattern.py", focus_on_open = false }
vidow.log("ipc path = " .. vidow.ipc_path())
EOF
start_vidow
[ -S "$SOCK" ] && pass "socket 在 $SOCK" || fail "socket 没建出来（$SOCK）"
check "version 命令有应答" '{"cmd":"version"}' 'resp["ok"]' 'True'
checkp "version 里带 socket 路径" '{"cmd":"version"}' 'resp["result"]["socket"]' "$SOCK"
check "ping" '{"cmd":"ping"}' 'resp["result"]["pong"]' 'True'
grep -aq "ipc path = $SOCK" "$LOG" && pass "config.lua 能拿到 vidow.ipc_path()" \
	|| fail "config.lua 里的 vidow.ipc_path() 不对"
./build/vidowmsg --socket "$SOCK" version >/dev/null 2>&1 \
	&& pass "vidowmsg --socket 能连上" || fail "vidowmsg 连不上"
./build/vidowmsg --socket "$SOCK" --raw windows >/dev/null 2>&1 \
	&& pass "vidowmsg --raw 也能跑" || fail "vidowmsg --raw 不行"

# ------------------------------------------------------------ 2) 窗口状态
mark "窗口状态：标题 / app_id / 几何 / 列 / 工作区 / 输出 / 规则"
open_pattern 甲
open_pattern 乙
ID1=$(jf '{"cmd":"windows"}' '[w["id"] for w in resp["result"] if w["title"]=="甲"][0]')
ID2=$(jf '{"cmd":"windows"}' '[w["id"] for w in resp["result"] if w["title"]=="乙"][0]')
[ "$ID1" != "$ID2" ] && pass "两个窗口的 id 不一样（$ID1 / $ID2）" || fail "id 撞了"
check "窗口数 = 2" '{"cmd":"state"}' 'resp["result"]["counts"]["windows"]' '2'
check "窗口带标题 / app_id" '{"cmd":"windows"}' \
	'[w["app_id"] for w in resp["result"] if w["title"]=="甲"][0]' 'pattern.py'
check "窗口报在工作区 1 上" '{"cmd":"windows"}' \
	'[w["workspace"] for w in resp["result"] if w["title"]=="甲"][0]' '1'
checkp "窗口报的输出名" '{"cmd":"windows"}' \
	'[w["output"] for w in resp["result"] if w["title"]=="甲"][0]' 'HEADLESS'
check "窗口是平铺的" '{"cmd":"windows"}' \
	'[w["floating"] for w in resp["result"] if w["title"]=="甲"][0]' 'False'
check "窗口规则被报出来（focus_on_open = False）" '{"cmd":"windows"}' \
	'[w["rules"]["focus_on_open"] for w in resp["result"] if w["title"]=="甲"][0]' 'False'
check "窗口在列里，列 id 不是 0" '{"cmd":"windows"}' \
	'[w["column"]["id"] > 0 for w in resp["result"] if w["title"]=="甲"][0]' 'True'
check "列的 index 从 0 起" '{"cmd":"windows"}' \
	'[w["column"]["index"] for w in resp["result"] if w["title"]=="乙"][0]' '1'
check "窗口带 pid（正整数）" '{"cmd":"windows"}' \
	'[w["pid"] > 0 for w in resp["result"] if w["title"]=="甲"][0]' 'True'
check "窗口带表面尺寸" '{"cmd":"windows"}' \
	'[w["surface"]["width"] > 0 for w in resp["result"] if w["title"]=="甲"][0]' 'True'
check "columns 命令也能查列" '{"cmd":"columns"}' 'len(resp["result"])' '2'

# 几何跟合成器自己的 place 日志对账（IPC 报的必须就是它画的那个矩形）
read -r PLACE_X PLACE_Y PLACE_W PLACE_H <<< "$(place_of 甲)"
read -r SURF_W SURF_H <<< "$(surface_of 甲)"
check "几何 x 跟 place 日志一致" '{"cmd":"windows"}' \
	'[w["geometry"]["x"] for w in resp["result"] if w["title"]=="甲"][0]' "$PLACE_X"
check "几何 y 跟 place 日志一致" '{"cmd":"windows"}' \
	'[w["geometry"]["y"] for w in resp["result"] if w["title"]=="甲"][0]' "$PLACE_Y"
check "几何宽跟 place 日志一致" '{"cmd":"windows"}' \
	'[w["geometry"]["width"] for w in resp["result"] if w["title"]=="甲"][0]' "$PLACE_W"
# map 日志是那一刻的快照，之后布局的 configure 会改表面尺寸（差一个 GAP 的量级）
check "表面尺寸跟 map 日志对得上（±16）" '{"cmd":"windows"}' \
	"[abs(w['surface']['width'] - $SURF_W) <= 16 for w in resp['result'] if w['title']=='甲'][0]" \
	'True'

# ------------------------------------------------------------ 3) layer / 输出
mark "layer surface 和输出：namespace / layer / 锚点 / 独占区 / 布局"
python3 tools/wall.py >/dev/null 2>&1 &
for _ in $(seq 1 24); do
	grep -aq "layer map wall" "$LOG" && break
	sleep 0.25
done
sleep 0.6
check "layer 数量 = 1" '{"cmd":"state"}' 'resp["result"]["counts"]["layers"]' '1'
check "layer 的 namespace" '{"cmd":"layers"}' 'resp["result"][0]["namespace"]' 'wall'
check "layer 的层" '{"cmd":"layers"}' 'resp["result"][0]["layer"]' 'background'
check "layer 四边都锚上了" '{"cmd":"layers"}' \
	'all(resp["result"][0]["anchor"].values())' 'True'
check "layer 的独占区 = 0" '{"cmd":"layers"}' 'resp["result"][0]["exclusive_zone"]' '0'
check "layer 铺满输出（1280x720）" '{"cmd":"layers"}' \
	'(resp["result"][0]["geometry"]["width"], resp["result"][0]["geometry"]["height"])' \
	'(1280, 720)'
check "layer 报出键盘交互（none）" '{"cmd":"layers"}' \
	'resp["result"][0]["keyboard_interactivity"]' 'none'
check "输出报出可用区域" '{"cmd":"outputs"}' \
	'(resp["result"][0]["usable_area"]["width"], resp["result"][0]["usable_area"]["height"])' \
	'(1280, 720)'
check "输出报出聚焦状态" '{"cmd":"outputs"}' 'resp["result"][0]["focused"]' 'True'
check "输出报出活动工作区" '{"cmd":"outputs"}' 'resp["result"][0]["active_workspace"]' '1'
check "输出把它的 layer 列出来了" '{"cmd":"outputs"}' \
	'len(resp["result"][0]["layers"])' '1'
check "输出报出缩放和逻辑尺寸" '{"cmd":"outputs"}' \
	'(resp["result"][0]["scale"], resp["result"][0]["logical_width"])' '(1, 1280)'

# ------------------------------------------------------------ 4) 命中测试
mark "pick-window：这个坐标上是哪个窗口"
PX=$(( PLACE_X + PLACE_W / 4 ))
PY_=$(( PLACE_Y + PLACE_H / 4 ))
check "pick-window 点到 甲" "{\"cmd\":\"pick-window\",\"x\":$PX,\"y\":$PY_}" \
	'resp["result"]["window"]["title"]' '甲'
check "pick-window 报出表面局部坐标" "{\"cmd\":\"pick-window\",\"x\":$PX,\"y\":$PY_}" \
	'resp["result"]["surface_x"] >= 0 and resp["result"]["surface_y"] >= 0' 'True'

# ------------------------------------------------------------ 5) 窗口操作
mark "窗口操作：焦点 / 浮动 / 尺寸 / 位置 / 居中（含像素作证）"
check "focus-window 换焦点" "{\"cmd\":\"focus-window\",\"id\":$ID1}" \
	'resp["result"]["window"]["title"]' '甲'
check "焦点确实换了（focused）" '{"cmd":"focused"}' 'resp["result"]["window"]["title"]' '甲'
check "activate-window 也能顶上来" "{\"cmd\":\"activate-window\",\"id\":$ID2}" \
	'resp["result"]["window"]["title"]' '乙'
check "set-floating true 变浮动（模式 fixed）" \
	"{\"cmd\":\"set-floating\",\"id\":$ID1,\"floating\":true}" \
	'(resp["result"]["window"]["floating"], resp["result"]["window"]["float_mode"])' \
	"(True, 'fixed')"
check "set-floating false 回平铺" \
	"{\"cmd\":\"set-floating\",\"id\":$ID1,\"floating\":false}" \
	'resp["result"]["window"]["floating"]' 'False'
check "toggle-floating 翻成浮动" "{\"cmd\":\"toggle-floating\",\"id\":$ID1}" \
	'resp["result"]["window"]["floating"]' 'True'
check "toggle-floating 再翻回平铺" "{\"cmd\":\"toggle-floating\",\"id\":$ID1}" \
	'resp["result"]["window"]["floating"]' 'False'
check "平铺时没有浮动盒（float_geometry = None）" '{"cmd":"windows"}' \
	'[w["float_geometry"] for w in resp["result"] if w["title"]=="甲"][0]' 'None'
# 后面几条都在浮动状态下做
jf "{\"cmd\":\"set-floating\",\"id\":$ID1,\"floating\":true}" 'resp["ok"]' >/dev/null
check "set-window-size 800x600（浮动盒立刻是它）" \
	"{\"cmd\":\"set-window-size\",\"id\":$ID1,\"width\":800,\"height\":600}" \
	'resp["result"]["window"]["float_geometry"]["width"]' '800'
sleep 0.8
check "客户端按新尺寸提交了（±48 的 CSD 边距）" '{"cmd":"windows"}' \
	"[abs(w['surface']['width'] - 800) <= 48 for w in resp['result'] if w['title']=='甲'][0]" \
	'True'
check "set-window-position 200 150" \
	"{\"cmd\":\"set-window-position\",\"id\":$ID1,\"x\":200,\"y\":150}" \
	'(resp["result"]["window"]["float_geometry"]["x"], resp["result"]["window"]["float_geometry"]["y"])' \
	'(200, 150)'
check "center-window 之后在工作区里居中" "{\"cmd\":\"center-window\",\"id\":$ID1}" \
	'(resp["result"]["window"]["float_geometry"]["x"], resp["result"]["window"]["float_geometry"]["y"])' \
	'(240, 60)'

# 像素：浮动窗口真的画在它被摆的地方（先把它端到最上面：平铺的乙可能盖着它）
jf "{\"cmd\":\"focus-window\",\"id\":$ID1}" 'resp["ok"]' >/dev/null
grim "$RUNDIR/float.png" >/dev/null 2>&1
read -r FX FY FW FH <<< "$(box '{"cmd":"windows"}' 甲 float_geometry)"
read -r SW SH <<< "$(jf '{"cmd":"windows"}' '"%d %d" % ([w["surface"]["width"] for w in resp["result"] if w["title"]=="甲"][0], [w["surface"]["height"] for w in resp["result"] if w["title"]=="甲"][0])')"
# 内容在表面里内缩 CSD 边距 ((表面-盒子)/2)，红象限是内容的左上四分之一：
# 取那个象限的中心 —— 两种解释（盒子是表面 / 盒子是内容）都落在红里。
PX_IN=$(( (SW - FW) / 2 + FW / 4 ))
PY_IN=$(( (SH - FH) / 2 + FH / 4 ))
RES=$(python3 - "$RUNDIR/float.png" "$(( FX + PX_IN ))" "$(( FY + PY_IN ))" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y = int(sys.argv[2]), int(sys.argv[3])
r, g, b = im.getpixel((x, y))
if r > 200 and g < 80 and b < 80:
    print("OK %d,%d 是纯红" % (x, y))
else:
    print("BAD 取样点 %d,%d 的像素是 %d,%d,%d（期望纯红）" % (x, y, r, g, b))
PY
)
case "$RES" in
OK*) pass "浮动窗口真的画在 $FX,$FY（${FW}x${FH}）：$RES" ;;
*) fail "$RES（几何 $FX,$FY ${FW}x${FH}，表面 ${SW}x${SH}）" ;;
esac

# ------------------------------------------------------------ 6) 列操作
mark "列操作：列宽 / 撑满 / 循环 / 居中 / 换列"
jf "{\"cmd\":\"set-floating\",\"id\":$ID1,\"floating\":false}" 'resp["ok"]' >/dev/null
jf "{\"cmd\":\"focus-window\",\"id\":$ID2}" 'resp["ok"]' >/dev/null
check "set-column-width 0.25" "{\"cmd\":\"set-column-width\",\"id\":$ID2,\"fraction\":0.25}" \
	'resp["result"]["column"]["width_fraction"]' '0.25'
COL=$(jf "{\"cmd\":\"set-column-width\",\"id\":$ID2,\"fraction\":0.25}" \
	'resp["result"]["column"]["id"]')
check "列宽算进布局了（0.25 * 1268 ≈ 317）" '{"cmd":"columns"}' \
	"abs([c for c in resp['result'] if c['id']==$COL][0]['width'] - 317) < 8" 'True'
check "set-column-width delta 加减" "{\"cmd\":\"set-column-width\",\"id\":$ID2,\"delta\":0.25}" \
	'resp["result"]["column"]["width_fraction"]' '0.5'
check "expand-column-to-available-width 撑满" '{"cmd":"expand-column-to-available-width"}' \
	'resp["result"]["column"]["width_fraction"]' '1'
check "cycle-column-width 换一档" '{"cmd":"cycle-column-width"}' \
	'resp["result"]["column"]["width_fraction"] < 1' 'True'
check "center-column 之后视口是个数" '{"cmd":"center-column"}' \
	'resp["result"]["viewport_x"] >= 0' 'True'
CI=$(jf '{"cmd":"focused"}' 'resp["result"]["window"]["column"]["index"]')
if [ "$CI" = "0" ]; then DIR=1; else DIR=-1; fi
check "focus-column 换到隔壁那一列" "{\"cmd\":\"focus-column\",\"direction\":$DIR}" \
	"resp['result']['focused']['column']['index'] != $CI" 'True'
check "move-column 把这一列挪个位置" "{\"cmd\":\"move-column\",\"direction\":$DIR}" \
	'resp["result"]["focused"] != None' 'True'

# ------------------------------------------------------------ 7) 工作区
mark "工作区：切 / 建 / 搬窗口 / 特殊工作区 / 换屏"
check "focus-workspace 2" '{"cmd":"focus-workspace","workspace":"2"}' \
	'resp["result"]["workspace"]["name"]' '2'
check "切过去之后活动工作区是 2" '{"cmd":"outputs"}' \
	'resp["result"][0]["active_workspace"]' '2'
check "focus-workspace 没建过的名字会报错" \
	'{"cmd":"focus-workspace","workspace":"没有这块"}' 'resp["ok"]' 'False'
check "focus-workspace 3（纯数字自动建）" '{"cmd":"focus-workspace","workspace":"3"}' \
	'resp["ok"]' 'True'
check "focus-workspace prev 相对切换" '{"cmd":"focus-workspace","workspace":"prev"}' \
	'resp["ok"]' 'True'
check "index 也能选工作区" '{"cmd":"focus-workspace","index":0}' 'resp["ok"]' 'True'
check "move-window-to-workspace 把窗口搬走" \
	"{\"cmd\":\"move-window-to-workspace\",\"id\":$ID2,\"workspace\":\"2\"}" \
	'resp["result"]["window"]["workspace"]' '2'
check "搬完以后回读在新工作区" '{"cmd":"windows"}' \
	'[w["workspace"] for w in resp["result"] if w["title"]=="乙"][0]' '2'
check "move-window-to-special 搬到特殊工作区" \
	"{\"cmd\":\"move-window-to-special\",\"id\":$ID2,\"name\":\"scratch\"}" \
	'resp["result"]["window"]["workspace"]' 'scratch'
check "窗口报出它所在的是特殊工作区" '{"cmd":"windows"}' \
	'[w["workspace_special"] for w in resp["result"] if w["title"]=="乙"][0]' 'True'
check "工作区列表里能看到特殊工作区（special = true）" '{"cmd":"workspaces"}' \
	'[w["name"] for w in resp["result"] if w["special"]]' "['scratch']"
check "toggle-special 打开它" '{"cmd":"toggle-special","name":"scratch"}' \
	'resp["result"]["shown"]["name"]' 'scratch'
check "toggle-special 再点一下收起来" '{"cmd":"toggle-special","name":"scratch"}' \
	'resp["result"]["shown"]' 'None'
check "create-workspace 建一块新的" '{"cmd":"create-workspace","name":"9"}' \
	'resp["result"]["workspace"]["name"]' '9'
check "move-workspace-to-output 已经在那块屏上就报错" \
	'{"cmd":"move-workspace-to-output","workspace":"9","output":"HEADLESS-1"}' \
	'resp["ok"]' 'False'
check "特殊工作区不能换屏（它跟屏走）" \
	'{"cmd":"move-workspace-to-output","workspace":"scratch","output":"HEADLESS-1"}' \
	'resp["ok"]' 'False'

# ------------------------------------------------------------ 8) 显示器
mark "显示器：开关 / 缩放 / 方向 / 模式 / 位置 / 聚焦"
check "output-power off" '{"cmd":"output-power","output":"HEADLESS-1","mode":"off"}' \
	'resp["result"]["output"]["enabled"]' 'False'
check "回读确认关了" '{"cmd":"outputs"}' 'resp["result"][0]["enabled"]' 'False'
check "output-power on" '{"cmd":"output-power","output":"HEADLESS-1","mode":"on"}' \
	'resp["result"]["output"]["enabled"]' 'True'
check "output-scale 1.5" '{"cmd":"output-scale","output":"HEADLESS-1","scale":1.5}' \
	'resp["result"]["output"]["scale"]' '1.5'
check "缩放改了逻辑尺寸（1280/1.5 ≈ 853）" '{"cmd":"outputs"}' \
	'resp["result"][0]["logical_width"]' '853'
check "output-scale auto 回到配置值" \
	'{"cmd":"output-scale","output":"HEADLESS-1","scale":"auto"}' \
	'resp["result"]["output"]["scale"]' '1'
check "output-transform 90" \
	'{"cmd":"output-transform","output":"HEADLESS-1","transform":"90"}' \
	'resp["result"]["output"]["transform"]' '90'
check "转了 90 度之后布局宽高对调" '{"cmd":"outputs"}' \
	'(resp["result"][0]["layout"]["width"], resp["result"][0]["layout"]["height"])' \
	'(720, 1280)'
check "output-transform normal 转回来" \
	'{"cmd":"output-transform","output":"HEADLESS-1","transform":"normal"}' \
	'resp["result"]["output"]["transform"]' 'normal'
check "output-mode 1280x720" \
	'{"cmd":"output-mode","output":"HEADLESS-1","width":1280,"height":720}' \
	'resp["result"]["output"]["width"]' '1280'
check "output-position 50 25" \
	'{"cmd":"output-position","output":"HEADLESS-1","x":50,"y":25}' \
	'(resp["result"]["output"]["layout"]["x"], resp["result"]["output"]["layout"]["y"])' \
	'(50, 25)'
check "output-focus" '{"cmd":"output-focus","output":"HEADLESS-1"}' \
	'resp["result"]["output"]["focused"]' 'True'
check "不存在的输出要报错" '{"cmd":"output-scale","output":"DP-9","scale":2}' \
	'resp["ok"]' 'False'
check "缩放值离谱要报错" '{"cmd":"output-scale","output":"HEADLESS-1","scale":99}' \
	'resp["ok"]' 'False'
check "认不出的方向要报错" \
	'{"cmd":"output-transform","output":"HEADLESS-1","transform":"斜着"}' \
	'resp["ok"]' 'False'

# ------------------------------------------------------------ 9) 配置 / 合成器
mark "配置与合成器命令：运行时改配置 / 重载 / 光标 / overview / action"
check "set-config 改窗口动画时长" \
	'{"cmd":"set-config","key":"window_open_ms","value":"42"}' \
	'resp["result"]["value"]' '42'
check "get-config 读回来" '{"cmd":"get-config","key":"window_open_ms"}' \
	'resp["result"]["value"]' '42'
check "config 全量里也是 42" '{"cmd":"config"}' 'resp["result"]["window_open_ms"]' '42'
check "config 全量有几十个键" '{"cmd":"config"}' 'len(resp["result"]) > 40' 'True'
check "state 里也带 config" '{"cmd":"state"}' \
	'resp["result"]["config"]["window_open_ms"]' '42'
check "不存在的配置键要报错" '{"cmd":"set-config","key":"没有这个键","value":"1"}' \
	'resp["ok"]' 'False'
check "reload-config" '{"cmd":"reload-config"}' 'resp["result"]["reloaded"]' 'True'
grep -aq "config.lua: running" "$LOG" && pass "重载真的重跑了 config.lua" \
	|| fail "重载没有重跑脚本"
check "move-cursor" '{"cmd":"move-cursor","x":640,"y":360}' \
	'(resp["result"]["cursor"]["x"], resp["result"]["cursor"]["y"])' '(640, 360)'
check "action 跑键位动作（workspace 1）" '{"cmd":"action","action":"workspace 1"}' \
	'resp["result"]["done"]' 'True'
check "action 不认识的动作用 ok:false 回" '{"cmd":"action","action":"fly away"}' \
	'resp["ok"]' 'False'
check "toggle-overview 开" '{"cmd":"toggle-overview"}' \
	'resp["result"]["overview"]["target"]' 'True'
check "overview 状态查询" '{"cmd":"overview"}' 'resp["result"]["active"]' 'True'
check "toggle-overview 关" '{"cmd":"toggle-overview"}' \
	'resp["result"]["overview"]["target"]' 'False'
check "键位列表里有 config.lua 写的那条" '{"cmd":"keybinds"}' \
	'[b["action"] for b in resp["result"]]' "['workspace 1']"
check "规则列表里有那条窗口规则" '{"cmd":"rules"}' \
	'[r["app_id"] for r in resp["result"]]' "['pattern.py']"
check "输入状态里有键盘 / 光标" '{"cmd":"input"}' \
	'"cursor" in resp["result"] and "keyboards" in resp["result"]' 'True'
check "seat 报出能力列表" '{"cmd":"seats"}' \
	'"capabilities" in resp["result"][0]' 'True'
check "激活令牌列表（现在没有）" '{"cmd":"tokens"}' 'resp["result"]' '[]'
check "输出记忆里有 HEADLESS-1" '{"cmd":"memories"}' \
	'[m["key"] for m in resp["result"]]' "['HEADLESS-1']"
check "xwayland 状态（配置里关了）" '{"cmd":"xwayland"}' \
	'resp["result"]["enabled"]' 'False'
check "im 状态" '{"cmd":"im"}' 'resp["result"]["connected"]' 'False'
check "anims 结构" '{"cmd":"anims"}' '"running" in resp["result"]' 'True'
check "state 里带上了一切" '{"cmd":"state"}' \
	'sorted(resp["result"].keys())' \
	"['activation_tokens', 'anims', 'clipboard', 'config', 'config_lua', 'counts', 'focused', 'im', 'input', 'keybinds', 'layers', 'log_level', 'mru', 'output_memories', 'outputs', 'overview', 'pid', 'primary_selection', 'rules', 'seats', 'shaders', 'socket', 'time_ms', 'version', 'wayland_display', 'windows', 'workspaces', 'xwayland']"

# ------------------------------------------------------------ 10) 事件订阅
mark "事件订阅：打开 / 移动 / 浮动 / 关闭 / 工作区 / 输出 / 过滤器"
timeout 25 ./build/vidowmsg --socket "$SOCK" --raw watch > "$RUNDIR/watch.txt" 2>&1 &
WATCH_PID=$!
sleep 1.2
open_pattern 丙
NID=$(jf '{"cmd":"windows"}' '[w["id"] for w in resp["result"] if w["title"]=="丙"][0]')
jf "{\"cmd\":\"toggle-floating\",\"id\":$NID}" 'resp["ok"]' >/dev/null
sleep 0.4
# 再真的挪一下它：浮动窗口的位置是「相对自己那块屏」记的，平铺转浮动**不会**
# 把它挪到别处（早先会跳到屏幕左上角，那是 bug，已经修了）—— 不主动挪一下，
# 这一节就收不到 window-moved 事件了。
jf "{\"cmd\":\"set-window-position\",\"id\":$NID,\"x\":300,\"y\":200}" 'resp["ok"]' >/dev/null
sleep 0.4
jf '{"cmd":"focus-workspace","workspace":"2"}' 'resp["ok"]' >/dev/null
jf '{"cmd":"output-scale","output":"HEADLESS-1","scale":1.25}' 'resp["ok"]' >/dev/null
jf '{"cmd":"output-scale","output":"HEADLESS-1","scale":"auto"}' 'resp["ok"]' >/dev/null
sleep 0.8
jf "{\"cmd\":\"close-window\",\"id\":$NID}" 'resp["ok"]' >/dev/null
sleep 1.5
kill "$WATCH_PID" 2>/dev/null
wait "$WATCH_PID" 2>/dev/null

for ev in window-opened window-title-changed window-moved window-floating-changed \
		window-closed workspace-activated output-changed; do
	grep -aq "\"event\":\"$ev\"" "$RUNDIR/watch.txt" && pass "收到 $ev" \
		|| fail "没收到 $ev"
done
grep -aq '"title":"丙"' "$RUNDIR/watch.txt" && pass "事件带着标题" \
	|| fail "事件没带标题"
grep -aq '"app_id":"pattern.py"' "$RUNDIR/watch.txt" && pass "事件带着 app_id" \
	|| fail "事件没带 app_id"
WATCH_CHECK=$(python3 - "$RUNDIR/watch.txt" <<'PY'
import json, sys
events = [json.loads(l) for l in open(sys.argv[1], encoding="utf-8")
          if l.startswith('{') and '"event"' in l]
out = []
closed = [e for e in events if e["event"] == "window-closed"]
if closed and closed[-1]["data"]["title"] == "丙":
    out.append("OK window-closed 带的是标题快照")
else:
    out.append("BAD window-closed 的载荷是 %r" % (closed[-1] if closed else None,))
opened = [e for e in events if e["event"] == "window-opened"]
if opened and opened[-1]["data"]["geometry"]["width"] > 0:
    out.append("OK window-opened 带完整窗口对象（含几何）")
else:
    out.append("BAD window-opened 载荷不完整：%r" % (opened[-1] if opened else None,))
print("\n".join(out))
PY
)
while IFS= read -r line; do
	case "$line" in
	OK*) pass "${line#OK }" ;;
	BAD*) fail "${line#BAD }" ;;
	esac
done <<< "$WATCH_CHECK"

# 过滤器：只订 window-opened，不该收到 output-changed
timeout 14 ./build/vidowmsg --socket "$SOCK" --raw watch window-opened \
	> "$RUNDIR/watch2.txt" 2>&1 &
WATCH2=$!
sleep 1.2
jf '{"cmd":"output-scale","output":"HEADLESS-1","scale":1.5}' 'resp["ok"]' >/dev/null
sleep 0.5
open_pattern 丁
sleep 1.2
kill "$WATCH2" 2>/dev/null
wait "$WATCH2" 2>/dev/null
grep -aq '"event":"window-opened"' "$RUNDIR/watch2.txt" && pass "过滤器放行订了的事件" \
	|| fail "过滤器把订了的事件也挡了"
grep -aq '"event":"output-changed"' "$RUNDIR/watch2.txt" \
	&& fail "过滤器没挡住没订的事件" || pass "过滤器挡住没订的事件"
jf '{"cmd":"output-scale","output":"HEADLESS-1","scale":"auto"}' 'resp["ok"]' >/dev/null

# 订阅者计数
timeout 8 ./build/vidowmsg --socket "$SOCK" watch >/dev/null 2>&1 &
WATCH3=$!
sleep 1.2
check "state 报出订阅者数量" '{"cmd":"state"}' \
	'resp["result"]["counts"]["subscribers"] >= 1' 'True'
kill "$WATCH3" 2>/dev/null
wait "$WATCH3" 2>/dev/null

# 订阅 / 退订之后同一条连接还能照常问话（不是「订阅了就只能收事件」）
SUB_RES=$(python3 - "$SOCK" <<'PY'
import json, socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sys.argv[1])

def send(obj):
    s.sendall((json.dumps(obj) + "\n").encode("utf-8"))
    buf = b""
    while b"\n" not in buf:
        chunk = s.recv(65536)
        if not chunk:
            return ""
        buf += chunk
    return buf.split(b"\n")[0].decode("utf-8", "replace")

sub = send({"cmd": "subscribe", "events": ["window-opened"]})
unsub = send({"cmd": "unsubscribe"})
ping = send({"cmd": "ping"})
win = send({"cmd": "windows"})
print("sub", "OK" if '"subscribed":true' in sub else "BAD " + sub)
print("unsub", "OK" if '"subscribed":false' in unsub else "BAD " + unsub)
print("ping", "OK" if '"pong":true' in ping else "BAD " + ping)
print("reuse", "OK" if '"ok":true' in win and '"result":[' in win else "BAD " + win)
PY
)
while IFS= read -r line; do
	case "$line" in
	"sub OK") pass "subscribe 之后拿到订阅应答" ;;
	"unsub OK") pass "unsubscribe 退订成功" ;;
	"ping OK") pass "退订之后同一条连接还能问话" ;;
	"reuse OK") pass "退订之后还能查状态" ;;
	*) fail "订阅/退订这条链：$line" ;;
	esac
done <<< "$SUB_RES"

# ------------------------------------------------------------ 11) 错误路径
mark "错误路径：坏 JSON / 未知命令 / 未知 id / 超长一行 —— 都是拒绝，不是崩"
check "未知命令" '{"cmd":"没有这个命令"}' 'resp["ok"]' 'False'
checkp "未知命令给的是能读的原因" '{"cmd":"没有这个命令"}' 'resp["error"]' '没有'
check "没有 cmd 字段" '{"id":1}' 'resp["ok"]' 'False'
check "cmd 类型不对（数字）" '{"cmd":42}' 'resp["ok"]' 'False'
check "未知 id 的窗口" '{"cmd":"close-window","id":9999}' 'resp["ok"]' 'False'
checkp "未知 id 的原因里有那个号" '{"cmd":"close-window","id":9999}' 'resp["error"]' '没有 id = 9999'
check "平铺窗口不让改尺寸" '{"cmd":"set-window-size","title":"乙","width":10,"height":10}' \
	'resp["ok"]' 'False'
check "切到空工作区" '{"cmd":"focus-workspace","workspace":"9"}' 'resp["ok"]' 'True'
check "空工作区上 pick-window 是 null" '{"cmd":"pick-window","x":640,"y":360}' \
	'resp["result"]["window"]' 'None'
check "没有聚焦窗口时 focus-window 报错" '{"cmd":"focus-window"}' 'resp["ok"]' 'False'

# 坏 JSON：回一条带位置的 ok:false
BAD=$(python3 "$JF" "$SOCK" '{"cmd":bad}' raw)
case "$BAD" in
*'"ok":false'*"个字符"*) pass "坏 JSON 回了 ok:false 并指出位置" ;;
*) fail "坏 JSON 的应答不对：$(printf '%s' "$BAD" | head -c 200)" ;;
esac

# 同一连接上连着发三条请求：三条都要有应答（不是只答第一条）
MULTI=$(python3 - "$SOCK" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sys.argv[1])
s.sendall(b'{"cmd":"ping"}\n{"cmd":"version"}\n{"cmd":"windows"}\n')
buf = b""
while buf.count(b"\n") < 3:
    chunk = s.recv(65536)
    if not chunk:
        break
    buf += chunk
sys.stdout.write(buf.decode("utf-8", "replace"))
PY
)
COUNT=$(printf '%s' "$MULTI" | grep -ac '"ok":true')
[ "$COUNT" = "3" ] && pass "一条连接上多条请求各答一次" \
	|| fail "一条连接上三条请求只答了 $COUNT 条"

# 超长的一行（> 1 MiB）：报错并断开，而不是把内存吃光
LONG_RES=$(python3 - "$SOCK" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(10)
s.connect(sys.argv[1])
try:
    # 合成器发现这一行太长就直接断开：发送端可能吃到 EPIPE，那是预期的。
    s.sendall(b'{"cmd":"ping","pad":"' + b'x' * (2 * 1024 * 1024) + b'"}\n')
except (BrokenPipeError, ConnectionResetError):
    pass
out = b""
try:
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        out += chunk
except socket.timeout:
    pass
text = out.decode("utf-8", "replace")
if '"ok":false' in text and "太长" in text:
    print("OK 超长的一行被拒（回了 %d 字节）" % len(out))
else:
    print("BAD 超长一行的应答：%r" % text[:200])
PY
)
case "$LONG_RES" in
OK*) pass "${LONG_RES#OK }" ;;
*) fail "${LONG_RES#BAD }" ;;
esac
check "挨了超长行之后合成器照常应答" '{"cmd":"ping"}' 'resp["result"]["pong"]' 'True'

# ------------------------------------------------------------ 12) 多屏
mark "多屏：两块输出上搬窗口 / 搬工作区 / 开关其中一块"
stop_vidow
start_vidow 2
check "两块输出都在" '{"cmd":"outputs"}' 'len(resp["result"])' '2'
check "两块输出的名字不同" '{"cmd":"outputs"}' \
	'(resp["result"][0]["name"] != resp["result"][1]["name"])' 'True'
check "只有一块是聚焦的" '{"cmd":"outputs"}' \
	'sum(o["focused"] for o in resp["result"])' '1'
check "两块屏自动排开（x = 0 和 1280）" '{"cmd":"outputs"}' \
	'sorted(o["layout"]["x"] for o in resp["result"])' '[0, 1280]'
open_pattern 甲
OTHER=$(jf '{"cmd":"outputs"}' \
	'[o["name"] for o in resp["result"] if not o["focused"]][0]')
FOCUSED=$(jf '{"cmd":"outputs"}' '[o["name"] for o in resp["result"] if o["focused"]][0]')
check "新窗口开在聚焦的那块屏上" '{"cmd":"windows"}' \
	"[w['output'] for w in resp['result'] if w['title']=='甲'][0]" "$FOCUSED"
check "move-window-to-output 把窗口搬到另一块屏" \
	"{\"cmd\":\"move-window-to-output\",\"title\":\"甲\",\"output\":\"$OTHER\"}" \
	'resp["result"]["window"]["output"]' "$OTHER"
check "回读确认它在另一块屏上" '{"cmd":"windows"}' \
	"[w['output'] for w in resp['result'] if w['title']=='甲'][0]" "$OTHER"
check "move-workspace-to-output 搬一块工作区过去" \
	"{\"cmd\":\"move-workspace-to-output\",\"workspace\":\"2\",\"output\":\"$OTHER\"}" \
	'resp["result"]["workspace"]["output"]' "$OTHER"
check "那块屏上的活动工作区跟着变" '{"cmd":"outputs"}' \
	"[o['active_workspace'] for o in resp['result'] if o['name']=='$OTHER'][0]" '2'
check "output-power 关掉那块屏" \
	"{\"cmd\":\"output-power\",\"output\":\"$OTHER\",\"mode\":\"off\"}" \
	'resp["result"]["output"]["enabled"]' 'False'
check "另一块屏还开着" '{"cmd":"outputs"}' \
	"[o['enabled'] for o in resp['result'] if o['name']=='$FOCUSED'][0]" 'True'
check "关掉那块屏之后窗口还在（只是不画了）" '{"cmd":"windows"}' \
	"[w['output'] for w in resp['result'] if w['title']=='甲'][0]" "$OTHER"
check "output-power 再打开" \
	"{\"cmd\":\"output-power\",\"output\":\"$OTHER\",\"mode\":\"on\"}" \
	'resp["result"]["output"]["enabled"]' 'True'
check "两块屏各报各的窗口" '{"cmd":"outputs"}' \
	"[len(o['windows']) for o in resp['result'] if o['name']=='$OTHER'][0] >= 1" 'True'

# ------------------------------------------------------------ 13) 收尾
mark "收尾：quit 让合成器退出、socket 清掉"
check "quit" '{"cmd":"quit"}' 'resp["result"]["quitting"]' 'True'
sleep 1.5
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	fail "quit 之后进程还在"
else
	pass "quit 之后合成器退出了"
fi
[ -S "$SOCK" ] && fail "退出后 socket 文件还在" || pass "退出时 socket 文件清掉了"
stop_vidow

echo
if [ "$FAILED" = "0" ]; then
	echo "全部通过"
else
	echo "有失败"
fi
exit "$FAILED"
