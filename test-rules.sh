#!/bin/bash
# 窗口规则 / 图层规则的匹配：正则、取反、literal、字段清单、报错、读回。
#
# 判据：
#   1) 字段读回来对得上：vidowmsg rules（JSON）和 vidow.rules()（Lua）两边都全，
#      没写的字段是 null / nil（「没管它」和「管了、给的是默认值」分得开）
#   2) 报错都是人话，而且**当场**报：字段名拼错（还列出能写的）、正则不合法、
#      一个作用都没写、capture 写了别的、capture_mosaic < 2、类型不对
#   3) 匹配真的是正则：^...$ 锚定、. 是任意字符、大小写不敏感、literal = true
#      退回子串
#   4) 取反（字段前面加 !）
#   5) 字段真的有值：pid / exe / cmdline / xwayland / floating / on_output /
#      title / initial_title / class / role
#   6) 多条规则：AND、后面的覆盖前面的、没写的字段回默认（不是留着上一条的值）
#   7) 图层规则：namespace / layer 匹配，窗口字段写进图层规则 = 这条不匹配
#   8) 图层规则的作用：开合（anim / open_ms / close_ms）和不透明度（抓帧量像素）
#   9) 日志里没有 assert / abort，窗口还在
#
# 用法：./test-rules.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 bluewin / dot 这两个客户端。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-rules-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
BLUE_PID=""
DOT_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		local pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		local cmd
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*bluewin.py*|*tools/dot.py*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.3
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	stop_clients
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

CFG="$CFGDIR/.config/vidow/config.lua"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

# ------------------------------------------------------------ 起合成器
cat > "$CFG" <<'EOF'
scale = "1"
xwayland = false
EOF

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-rules.sh: vidow 没起来"
	grep -a "ERROR" "$LOG" | head -5
	exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

reload_count() { grep -ac "config.lua: running" "$LOG"; }

wait_reload() { # 等配置真的被重跑一遍（自动重载有 150ms 去抖）
	local before="$1" i
	for i in $(seq 1 60); do
		[ "$(reload_count)" -gt "$before" ] && return 0
		sleep 0.1
	done
	return 1
}

write_base() { printf 'scale = "1"\nxwayland = false\n'; }

apply_cfg() { # apply_cfg <行…>：写配置（基础行 + 给的行）并等重载
	local before
	before=$(reload_count)
	{ write_base; printf '%s\n' "$@"; } > "$CFG"
	wait_reload "$before"
}

# 窗口 / 图层现在的抓帧遮挡状态（规则有没有套上，看这里）
state() { # state <window|layer> [名字]
	local what="$1" name="${2:-bluewin}"
	./build/vidowmsg "$([ "$what" = layer ] && echo layers || echo windows)" \
		2>/dev/null | python3 -c '
import json, sys
what, name = sys.argv[1], sys.argv[2]
data = json.load(sys.stdin)
items = data.get("layers" if what == "layer" else "windows", data) \
	if isinstance(data, dict) else data
for it in items:
	if what == "layer":
		key = it.get("namespace")
		cap = it.get("capture")
	else:
		key = it.get("title")
		cap = it.get("rules", {}).get("capture")
	if key == name:
		print(cap if cap is not None else "?")
		break
else:
	print("missing")
' "$what" "$name"
}

wait_state() { # wait_state <期望> [window|layer] [名字] —— 最多等 5 秒
	local want="$1" what="${2:-window}" name="${3:-bluewin}" i got
	for i in $(seq 1 50); do
		got=$(state "$what" "$name")
		[ "$got" = "$want" ] && return 0
		sleep 0.1
	done
	echo "      （等 $what/$name 的 capture = $want 超时，现在是 $got）"
	return 1
}

case_state() { # case_state <标签> <期望> <行…>
	local label="$1" want="$2"; shift 2
	apply_cfg "$@" || { fail "$label：配置没重载"; return; }
	if wait_state "$want"; then
		pass "$label → capture = $want"
	else
		fail "$label → 想要 $want"
	fi
}

# ------------------------------------------------------------ 1) 解析与读回
mark "1) 字段读回来（vidowmsg rules / vidow.rules）"
apply_cfg \
	'vidow.rule{ app_id = "mpv", force_render = true, hz = 15 }' \
	'vidow.rule{ title = "^预览", capture = "mosaic", capture_mosaic = 24 }' \
	'vidow.rule{ namespace = "notifications", capture = "hide" }' \
	'vidow.rule{ class = "Steam", focus_on_open = false }' \
	'vidow.rule{ app_id = "!mpv", is_xwayland = false, capture = "hide" }' \
	'vidow.rule{ literal = true, title = "a.b", capture = "hide" }' \
	'vidow.rule{ pid = 4242, exe = "^mpv$", cmdline = "mpv --no-config",' \
	'             layer = "overlay", on_output = "DP-1", is_floating = true,' \
	'             is_fixed_size = false, initial_title = "^新", role = "pop-up",' \
	'             capture = "off" }' \
	'local n = 0' \
	'for i, r in ipairs(vidow.rules()) do' \
	'  n = n + 1' \
	'  vidow.log(string.format("R%d app_id=%s title=%s hz=%s literal=%s",' \
	'    i, tostring(r.app_id), tostring(r.title), tostring(r.hz),' \
	'    tostring(r.literal)))' \
	'end' \
	'vidow.log("RULES " .. n)'

./build/vidowmsg rules > "$RUNDIR/rules.json" 2>/dev/null
python3 - "$RUNDIR/rules.json" <<'PY'
import json, sys
data = json.load(open(sys.argv[1], encoding='utf-8'))
rules = data.get("rules", data) if isinstance(data, dict) else data
failed = 0

def check(cond, msg):
	global failed
	if cond:
		print("  PASS  " + msg)
	else:
		print("  FAIL  " + msg)
		failed += 1

def eq(i, key, want):
	got = rules[i].get(key, "<缺字段>")
	check(got == want, "rules[%d].%s = %r（想要 %r）" % (i, key, got, want))

check(len(rules) == 7, "7 条规则都读回来了（%d 条）" % len(rules))
if len(rules) == 7:
	eq(0, "app_id", "mpv")
	eq(0, "force_render", True)
	eq(0, "hz", 15)
	eq(0, "capture", None)          # 没写 = null
	eq(0, "title", None)
	eq(1, "title", "^预览")
	eq(1, "capture", "mosaic")
	eq(1, "capture_mosaic", 24)
	eq(2, "namespace", "notifications")
	eq(2, "capture", "hide")
	eq(2, "app_id", None)
	eq(3, "class", "Steam")
	eq(3, "focus_on_open", False)
	eq(4, "app_id", "!mpv")         # 取反原样读回来
	eq(4, "is_xwayland", False)
	eq(5, "literal", True)
	eq(5, "title", "a.b")
	eq(6, "pid", 4242)
	eq(6, "exe", "^mpv$")
	eq(6, "cmdline", "mpv --no-config")
	eq(6, "layer", "overlay")
	eq(6, "on_output", "DP-1")
	eq(6, "is_floating", True)
	eq(6, "is_fixed_size", False)
	eq(6, "initial_title", "^新")
	eq(6, "role", "pop-up")
	eq(6, "capture", "off")
sys.exit(1 if failed else 0)
PY
[ $? -eq 0 ] || FAILED=1

grep -aq "RULES 7" "$LOG" && pass "vidow.rules() 也数得出 7 条" ||
	fail "vidow.rules() 数不出 7 条"
grep -aq "R1 app_id=mpv title=nil hz=15 literal=nil" "$LOG" &&
	pass "vidow.rules() 里没写的字段是 nil、写了的是值" ||
	fail "vidow.rules() 读回来的值不对：$(grep -a "^R1 " "$LOG" | tail -1)"

# ------------------------------------------------------------ 2) 报错
mark "2) 写错了当场报错（不是静默失效）"
apply_cfg \
	'vidow.log("E1 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ appid = "mpv", force_render = true } end)))))' \
	'vidow.log("E2 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ title = "[unclosed", capture = "hide" } end)))))' \
	'vidow.log("E3 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = "mpv" } end)))))' \
	'vidow.log("E4 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = "mpv", capture = "blur" } end)))))' \
	'vidow.log("E5 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = "mpv", capture = "mosaic", capture_mosaic = 1 } end)))))' \
	'vidow.log("E6 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = 42, capture = "hide" } end)))))' \
	'vidow.log("E7 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = "mpv", hz = "fast" } end)))))' \
	'vidow.log("E8 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = "mpv", column_width = 5 } end)))))' \
	'vidow.log("E9 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = "mpv", width = 0 } end)))))' \
	'vidow.log("E10 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = "mpv", workspace = "" } end)))))' \
	'vidow.log("E11 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = "mpv", opacity = 2 } end)))))' \
	'vidow.log("E12 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ app_id = "mpv", corner_radius = -3 } end)))))' \
	'vidow.log("E13 " .. tostring((select(2, pcall(function()' \
	'  vidow.rule{ namespace = "dot", open_ms = -1 } end)))))' \
	'vidow.rule{ app_id = "keep", capture = "hide" }'

check_log() { # check_log <标签> <必须出现的串> [<也得出现的串>]
	local label="$1"; shift
	local line
	line=$(grep -a "config.lua: $label " "$LOG" | tail -1)
	local ok=1
	for want in "$@"; do
		case "$line" in *"$want"*) ;; *) ok=0 ;; esac
	done
	if [ "$ok" = 1 ]; then
		pass "$label 的报错对得上：$(echo "$line" | head -c 120)…"
	else
		fail "$label 的报错不对（要含 $*）：$line"
	fi
}

check_log E1 "appid" "app_id"      # 拼错的字段名 + 列出能写的
check_log E2 "title" "正则"
check_log E3 "至少要写一个作用"
check_log E4 "hide"
check_log E5 "至少 2"
check_log E6 "字符串"
check_log E7 "整数"
check_log E8 "0.1 ~ 1.0"
check_log E9 "正数"
check_log E10 "不能是空的"
check_log E11 "0 ~ 1"
check_log E12 "0 ~ 1e+09"
check_log E13 "open_ms" "负数"

RULES_N=$(./build/vidowmsg rules 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
print(len(d.get("rules", d) if isinstance(d, dict) else d))')
[ "$RULES_N" = "1" ] && pass "报错的规则一条都没进去（只剩后面那条好的）" ||
	fail "报错的规则混进去了：$RULES_N 条"

# ------------------------------------------------------------ 3) 窗口匹配
mark "3) 真窗口上的匹配（bluewin：Wayland 客户端，标题 bluewin）"
HOME="$CFGDIR" python3 tools/bluewin.py > "$RUNDIR/blue.log" 2>&1 &
BLUE_PID=$!
for _ in $(seq 1 60); do
	grep -aq "place bluewin" "$LOG" && break
	sleep 0.2
done
grep -aq "place bluewin" "$LOG" && pass "客户端上屏了" || fail "bluewin 没起来"

OUT=$(./build/vidowmsg outputs 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
o = d.get("outputs", d) if isinstance(d, dict) else d
print(o[0]["name"] if o else "")')
[ -n "$OUT" ] && pass "这块屏叫 $OUT" || fail "拿不到输出名"

case_state "正则：^bluewin 前缀"          hide 'vidow.rule{ app_id = "^bluewin", capture = "hide" }'
case_state "正则：^nomatch 不命中"        off  'vidow.rule{ app_id = "^nomatch", capture = "hide" }'
case_state "正则：. 是任意字符"           hide 'vidow.rule{ title = "^blue.in$", capture = "hide" }'
case_state "正则：^blue\\.in$ 转义后不命中" off 'vidow.rule{ title = "^blue\\.in$", capture = "hide" }'
case_state "大小写不敏感"                 hide 'vidow.rule{ title = "^BLUEWIN$", capture = "hide" }'
case_state "literal = true 退回子串"      off  'vidow.rule{ literal = true, title = "^bluewin$", capture = "hide" }'
case_state "literal 的子串照样命中"       hide 'vidow.rule{ literal = true, title = "luewi", capture = "hide" }'
case_state "取反：!^bluewin$ 不命中"      off  'vidow.rule{ title = "!^bluewin$", capture = "hide" }'
case_state "取反：!^nomatch$ 命中"        hide 'vidow.rule{ title = "!^nomatch$", capture = "hide" }'
case_state "pid 匹配"                     hide "vidow.rule{ pid = $BLUE_PID, capture = \"hide\" }"
case_state "pid 不匹配"                   off  'vidow.rule{ pid = 1, capture = "hide" }'
case_state "exe 匹配（python3）"          hide 'vidow.rule{ exe = "^python3", capture = "hide" }'
case_state "cmdline 匹配"                 hide 'vidow.rule{ cmdline = "bluewin\\.py", capture = "hide" }'
case_state "cmdline 不匹配"               off  'vidow.rule{ cmdline = "^nope", capture = "hide" }'
case_state "is_xwayland = false 命中"        hide 'vidow.rule{ is_xwayland = false, capture = "hide" }'
case_state "is_xwayland = true 不命中"       off  'vidow.rule{ is_xwayland = true, capture = "hide" }'
case_state "is_floating = false 命中"        hide 'vidow.rule{ is_floating = false, capture = "hide" }'
case_state "is_floating = true 不命中"       off  'vidow.rule{ is_floating = true, capture = "hide" }'
case_state "on_output 命中"               hide "vidow.rule{ on_output = \"^$OUT$\", capture = \"hide\" }"
case_state "on_output 不命中"             off  'vidow.rule{ on_output = "^NOPE$", capture = "hide" }'
case_state "initial_title 命中"           hide 'vidow.rule{ initial_title = "^bluewin$", capture = "hide" }'
case_state "两个字段 AND：一真一假"       off  'vidow.rule{ app_id = "^bluewin", title = "^nomatch$", capture = "hide" }'
case_state "两个字段 AND：都真"           hide 'vidow.rule{ app_id = "^bluewin", title = "^bluewin$", capture = "hide" }'
case_state "后面的覆盖前面的"             off \
	'vidow.rule{ app_id = "^bluewin", capture = "hide" }' \
	'vidow.rule{ app_id = "^bluewin", capture = "off" }'
case_state "后面的规则只覆盖它写了的字段" hide \
	'vidow.rule{ app_id = "^bluewin", capture = "hide" }' \
	'vidow.rule{ app_id = "^bluewin", force_render = true }'
case_state "只命中后面那条也算"           mosaic \
	'vidow.rule{ app_id = "^nomatch", capture = "hide" }' \
	'vidow.rule{ app_id = "^bluewin", capture = "mosaic", capture_mosaic = 32 }'

# ------------------------------------------------------------ 4) 图层匹配
mark "4) 真图层上的匹配（dot：namespace = dot，background 层）"
HOME="$CFGDIR" python3 tools/dot.py > "$RUNDIR/dot.log" 2>&1 &
DOT_PID=$!
for _ in $(seq 1 60); do
	./build/vidowmsg layers 2>/dev/null | grep -aq '"namespace": *"dot"' && break
	sleep 0.2
done
./build/vidowmsg layers 2>/dev/null | grep -aq '"namespace": *"dot"' &&
	pass "layer surface 上屏了" || fail "dot 没起来"

case_layer() { # case_layer <标签> <期望> <行…>
	local label="$1" want="$2"; shift 2
	apply_cfg "$@" || { fail "$label：配置没重载"; return; }
	if wait_state "$want" layer dot; then
		pass "$label → capture = $want"
	else
		fail "$label → 想要 $want"
	fi
}

case_layer "namespace 正则命中"            hide 'vidow.rule{ namespace = "^dot$", capture = "hide" }'
case_layer "namespace 里的 . 是任意字符"   hide 'vidow.rule{ namespace = "d.t", capture = "hide" }'
case_layer "namespace 转义后不命中"        off  'vidow.rule{ namespace = "^d\\.t$", capture = "hide" }'
case_layer "layer = background 命中"       hide 'vidow.rule{ layer = "^background$", capture = "hide" }'
case_layer "layer = overlay 不命中"        off  'vidow.rule{ layer = "overlay", capture = "hide" }'
case_layer "图层规则写窗口字段 = 不匹配"   off  'vidow.rule{ namespace = "^dot$", app_id = "^dot$", capture = "hide" }'
case_layer "窗口规则不管图层"              off  'vidow.rule{ app_id = "^dot$", capture = "hide" }'
case_layer "capture_mosaic 在图层上也认"   mosaic 'vidow.rule{ namespace = "^dot$", capture = "mosaic", capture_mosaic = 48 }'

# ------------------------------------------------------------ 5) 开窗时兑现的作用
mark "5) 开窗时一次的作用（浮动 / 尺寸 / 居中 / 全屏 / 最大化 / 最小化 / 列宽 / 动画 / 工作区 / 屏）"

newest_id() {
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
w = d.get("windows", d) if isinstance(d, dict) else d
print(max([x["id"] for x in w], default=0))'
}

LAST_PID=""
open_win() { # 起一个客户端、等它上屏，把窗口 id 打在 stdout
	local before id i
	before=$(newest_id)
	HOME="$CFGDIR" python3 tools/bluewin.py > "$RUNDIR/blue-$RANDOM.log" 2>&1 &
	LAST_PID=$!
	for i in $(seq 1 80); do
		id=$(newest_id)
		if [ -n "$id" ] && [ "$id" -gt "$before" ]; then
			echo "$id"
			return 0
		fi
		sleep 0.2
	done
	return 1
}

kill_win() {
	[ -n "$LAST_PID" ] && kill "$LAST_PID" 2>/dev/null
	LAST_PID=""
	sleep 0.5
}

win_field() { # win_field <id> <python 表达式，x 是那个窗口的 dict>
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
wid, expr = int(sys.argv[1]), sys.argv[2]
d = json.load(sys.stdin)
w = d.get("windows", d) if isinstance(d, dict) else d
for x in w:
	if x["id"] == wid:
		print(eval(expr, {"x": x}))
		break
else:
	print("missing")
' "$1" "$2"
}

wait_field() { # wait_field <id> <表达式> <期望> —— 最多等 5 秒
	local id="$1" expr="$2" want="$3" i got
	for i in $(seq 1 50); do
		got=$(win_field "$id" "$expr")
		[ "$got" = "$want" ] && return 0
		sleep 0.1
	done
	echo "      （等 $expr = $want 超时，现在是 $got）"
	return 1
}

# case_open <标签> <表达式> <期望> <规则行…>：写配置 → 开一个新窗口 → 看它开成什么样
case_open() {
	local label="$1" expr="$2" want="$3"; shift 3
	apply_cfg "$@" || { fail "$label：配置没重载"; return; }
	local id
	if ! id=$(open_win); then
		fail "$label：窗口没起来"
		return
	fi
	if wait_field "$id" "$expr" "$want"; then
		pass "$label → $expr = $want"
	else
		fail "$label → 想要 $want"
	fi
	kill_win
}

case_open "float = true 强制浮动" 'x["floating"]' True \
	'vidow.rule{ title = "^bluewin$", float = true }'
case_open "width / height 定浮动尺寸" 'x["float_geometry"]["width"]' 400 \
	'vidow.rule{ title = "^bluewin$", float = true, width = 400, height = 300 }'
case_open "height 也认" 'x["float_geometry"]["height"]' 300 \
	'vidow.rule{ title = "^bluewin$", float = true, width = 400, height = 300 }'
case_open "fullscreen = true 开出来就全屏" 'x["fullscreen"]' True \
	'vidow.rule{ title = "^bluewin$", fullscreen = true }'
case_open "maximize = true 开出来就整列最大化" 'x["column"]["maximized"]' True \
	'vidow.rule{ title = "^bluewin$", maximize = true }'
case_open "minimize = true 开出来就收起来" 'x["minimized"]' True \
	'vidow.rule{ title = "^bluewin$", minimize = true }'
case_open "收起来的窗口不抢焦点" 'x["focused"]' False \
	'vidow.rule{ title = "^bluewin$", minimize = true }'
case_open "column_width 定新列的宽度" 'x["column"]["width_fraction"]' 0.25 \
	'vidow.rule{ title = "^bluewin$", column_width = 0.25 }'
case_open "workspace 开在别的工作区" 'x["workspace"]' "chat" \
	'vidow.rule{ title = "^bluewin$", workspace = "chat" }'
case_open "output 开在指定的屏上" 'x["output"]' "$OUT" \
	"vidow.rule{ title = \"^bluewin$\", output = \"$OUT\" }"

# 居中：拿浮动窗口的中心和屏幕中心比（工作区有边距，容 30px）
apply_cfg 'vidow.rule{ title = "^bluewin$", float = true, width = 400, height = 300, center = true }'
if id=$(open_win); then
	OUT_W=$(./build/vidowmsg outputs 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
o = d.get("outputs", d) if isinstance(d, dict) else d
print(o[0]["layout"]["width"])')
	got=$(win_field "$id" 'x["float_geometry"]["x"] + x["float_geometry"]["width"] // 2')
	want=$((OUT_W / 2))
	if [ "$got" != missing ] && [ $((got - want)) -le 30 ] && [ $((want - got)) -le 30 ]; then
		pass "center = true 居中（窗口中心 $got，屏幕中心 $want）"
	else
		fail "center = true 没居中（窗口中心 $got，屏幕中心 $want）"
	fi
	kill_win
else
	fail "center = true：窗口没起来"
fi

# anim = false：日志里不该有这一条的入场动画（别的窗口有，所以只数「这一条之后」）
apply_cfg 'vidow.rule{ title = "^bluewin$", anim = false }'
ANIM_BEFORE=$(grep -ac "anim open start" "$LOG")
if id=$(open_win); then
	sleep 0.8
	ANIM_AFTER=$(grep -ac "anim open start" "$LOG")
	if [ "$ANIM_AFTER" = "$ANIM_BEFORE" ]; then
		pass "anim = false：开窗没有入场动画（anim open start 还是 $ANIM_BEFORE 条）"
	else
		fail "anim = false 还是跑了入场动画（多了 $((ANIM_AFTER - ANIM_BEFORE)) 条）"
	fi
	kill_win
else
	fail "anim = false：窗口没起来"
fi
apply_cfg   # 回到「没有规则」的配置
ANIM_BEFORE=$(grep -ac "anim open start" "$LOG")
if id=$(open_win); then
	sleep 0.8
	ANIM_AFTER=$(grep -ac "anim open start" "$LOG")
	[ "$ANIM_AFTER" -gt "$ANIM_BEFORE" ] &&
		pass "对照组（没写 anim）：照样有入场动画" ||
		fail "对照组（没写 anim）反而没有入场动画"
	kill_win
else
	fail "对照组：窗口没起来"
fi

# float 是「一直在」的那一类：重载改规则，已经开着的窗口跟着变
apply_cfg 'vidow.rule{ title = "^bluewin$", float = true }'
if id=$(open_win); then
	wait_field "$id" 'x["floating"]' True >/dev/null &&
		apply_cfg 'vidow.rule{ title = "^bluewin$", float = false }' &&
		if wait_field "$id" 'x["floating"]' False; then
			pass "float 跟着重载走（浮动的窗口被规则按回平铺）"
		else
			fail "重载之后 float = false 没生效"
		fi
	kill_win
else
	fail "float 重载用例：窗口没起来"
fi

# ------------------------------------------------------------ 6) 外观作用
mark "6) 外观作用（装饰 / 不透明度 / 圆角 / 边框 / 阴影 / 噪点 / 模糊）"

case_look() { # case_look <标签> <表达式> <期望> <规则行…>
	local label="$1" expr="$2" want="$3"; shift 3
	apply_cfg "$@" || { fail "$label：配置没重载"; return; }
	# 用第 3 节那个一直开着的窗口（title = bluewin 的第一个）
	local id
	id=$(./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
w = d.get("windows", d) if isinstance(d, dict) else d
print(next((x["id"] for x in w if x.get("title") == "bluewin"), 0))')
	if [ "$id" = 0 ]; then
		fail "$label：没有窗口"
		return
	fi
	if wait_field "$id" "$expr" "$want"; then
		pass "$label → $expr = $want"
	else
		fail "$label → 想要 $want"
	fi
}

case_look "opacity 改不透明度" 'x["look"]["opacity"]' 0.5 \
	'vidow.rule{ title = "^bluewin$", opacity = 0.5 }'
case_look "corner_radius 改圆角" 'x["look"]["corner_radius"]' 24 \
	'vidow.rule{ title = "^bluewin$", corner_radius = 24 }'
case_look "border_width 改边框" 'x["look"]["border_width"]' 0 \
	'vidow.rule{ title = "^bluewin$", border_width = 0 }'
case_look "border_color 改边框颜色" 'x["look"]["border_color"]' "#ff0000ff" \
	'vidow.rule{ title = "^bluewin$", border_color = "ff0000" }'
case_look "border_color_focus 也认" 'x["look"]["border_color_focus"]' "#00ff00ff" \
	'vidow.rule{ title = "^bluewin$", border_color_focus = "00ff00" }'
case_look "shadow_radius 改阴影" 'x["look"]["shadow_radius"]' 30 \
	'vidow.rule{ title = "^bluewin$", shadow_radius = 30 }'
case_look "shadow_offset 也认" 'x["look"]["shadow_offset_y"]' 8 \
	'vidow.rule{ title = "^bluewin$", shadow_offset_y = 8 }'
case_look "noise 也认" 'x["look"]["noise"]' 0.2 \
	'vidow.rule{ title = "^bluewin$", noise = 0.2 }'
case_look "decor = false 不画装饰" 'x["look"]["decor"]' False \
	'vidow.rule{ title = "^bluewin$", decor = false }'
case_look "blur = false 关掉这个窗口的模糊" 'x["look"]["blur"]' False \
	'vidow.rule{ title = "^bluewin$", blur = false }'
case_look "blur = true 让平铺窗口也模糊" 'x["look"]["blur"]' True \
	'vidow.rule{ title = "^bluewin$", blur = true }'
case_look "blur_xray 也认" 'x["look"]["blur_xray"]' True \
	'vidow.rule{ title = "^bluewin$", blur_xray = true }'
case_look "全局开关压得住规则（decor_enabled = false）" 'x["look"]["decor"]' False \
	'decor_enabled = false' \
	'vidow.rule{ title = "^bluewin$", decor = true }'
case_look "规则压得住全局值（全局圆角 4，规则 24）" 'x["look"]["corner_radius"]' 24 \
	'corner_radius = 4' \
	'vidow.rule{ title = "^bluewin$", corner_radius = 24 }'

# 真的画上去了：不透明度 0.3 的窗口抓下来要暗一大截（量窗口中心的蓝通道）
mark "6b) 不透明度真的作用在画面上（抓帧量像素）"
if command -v grim >/dev/null && python3 -c 'import PIL' 2>/dev/null; then
	center_pixel() { # center_pixel <png> <x> <y> —— 打印蓝通道
		python3 - "$1" "$2" "$3" <<'PYEOF'
import sys
from PIL import Image
img = Image.open(sys.argv[1]).convert("RGB")
x, y = int(sys.argv[2]), int(sys.argv[3])
x = max(0, min(img.width - 1, x))
y = max(0, min(img.height - 1, y))
print(img.getpixel((x, y))[2])
PYEOF
	}
	win_center() { # 那个窗口的中心（布局坐标）
		./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
w = d.get("windows", d) if isinstance(d, dict) else d
for x in w:
	if x.get("title") == "bluewin":
		g = x["geometry"]
		print(g["x"] + g["width"] // 2, g["y"] + g["height"] // 2)
		break
else:
	print(0, 0)'
	}

	apply_cfg
	sleep 0.5
	read -r WX WY <<< "$(win_center)"
	grim -o "$OUT" "$RUNDIR/full.png" 2>/dev/null
	B_FULL=$(center_pixel "$RUNDIR/full.png" "$WX" "$WY")
	apply_cfg 'vidow.rule{ title = "^bluewin$", opacity = 0.3 }'
	sleep 0.5
	grim -o "$OUT" "$RUNDIR/dim.png" 2>/dev/null
	B_DIM=$(center_pixel "$RUNDIR/dim.png" "$WX" "$WY")
	if [ "$B_FULL" -gt 150 ] && [ "$B_DIM" -lt $((B_FULL * 6 / 10)) ]; then
		pass "opacity = 0.3：窗口中心的蓝从 $B_FULL 掉到 $B_DIM"
	else
		fail "opacity = 0.3 没作用在画面上（原样 $B_FULL → 现在 $B_DIM）"
	fi
	apply_cfg
else
	echo "  （没有 grim 或 PIL，跳过像素判据）"
fi

# ------------------------------------------------------------ 6c) anim = false 不等于「不要 fx」
mark "6c) anim = false / open_ms = 0 的窗口照样有装饰（不是裸表面）"
# 判据：给窗口一条红边框的规则 + anim = false，抓帧量**窗口左边缘**那个像素 ——
# 装饰（边框）是画在 fx 节点上的，fx 不建的话那里就是客户端自己的蓝，或者壁纸。
if command -v grim >/dev/null && python3 -c 'import PIL' 2>/dev/null; then
	border_pixel() { # border_pixel <png> <窗口 id> —— 打印左边缘往里 2px 那个像素的 R,G
		./build/vidowmsg windows > "$RUNDIR/windows.json" 2>/dev/null
		python3 - "$1" "$2" "$RUNDIR/windows.json" <<'PYEOF'
import json, sys
from PIL import Image
w = json.load(open(sys.argv[3], encoding='utf-8'))
w = w.get("windows", w) if isinstance(w, dict) else w
g = next((x["geometry"] for x in w if x["id"] == int(sys.argv[2])), None)
img = Image.open(sys.argv[1]).convert("RGB")
if g is None:
    print(-1, -1)
else:
    x = max(0, min(img.width - 1, g["x"] + 2))
    y = max(0, min(img.height - 1, g["y"] + g["height"] // 2))
    r, gg, _ = img.getpixel((x, y))
    print(r, gg)
PYEOF
	}

	apply_cfg 'vidow.rule{ title = "^bluewin$", anim = false, border_width = 6,' \
		'             border_color = "ff0000", border_color_focus = "ff0000" }'
	if WID=$(open_win); then
		sleep 0.5
		grim -o "$OUT" "$RUNDIR/anim-nofx.png" 2>/dev/null
		read -r BR BG <<< "$(border_pixel "$RUNDIR/anim-nofx.png" "$WID")"
		if [ "$BR" -gt 150 ] && [ "$BG" -lt 100 ]; then
			pass "anim = false 的窗口左边缘是规则给的红边框（R=$BR G=$BG）"
		else
			fail "anim = false 把 fx 一起关了：左边缘不是边框色（R=$BR G=$BG）"
		fi
		kill_win
	else
		fail "anim = false 那个窗口没起来"
	fi
	# open_ms = 0 是同一条路（这条一直是对的，一起钉住）
	apply_cfg 'vidow.rule{ title = "^bluewin$", open_ms = 0, border_width = 6,' \
		'             border_color = "ff0000", border_color_focus = "ff0000" }'
	if WID=$(open_win); then
		sleep 0.5
		grim -o "$OUT" "$RUNDIR/openms0.png" 2>/dev/null
		read -r BR BG <<< "$(border_pixel "$RUNDIR/openms0.png" "$WID")"
		if [ "$BR" -gt 150 ] && [ "$BG" -lt 100 ]; then
			pass "open_ms = 0 的窗口也有边框（R=$BR G=$BG）"
		else
			fail "open_ms = 0 把 fx 一起关了（R=$BR G=$BG）"
		fi
		kill_win
	else
		fail "open_ms = 0 那个窗口没起来"
	fi
	apply_cfg
else
	echo "  （没有 grim 或 PIL，跳过 6c 的像素判据）"
fi

# ------------------------------------------------------------ 8) 图层：开合 / 时长 / 不透明度
mark "8) 图层规则的开合与不透明度（anim / open_ms / close_ms / opacity）"

layer_field() { # layer_field <namespace> <python 表达式，x 是那个 layer 的 dict>
	./build/vidowmsg layers 2>/dev/null | python3 -c '
import json, sys
ns, expr = sys.argv[1], sys.argv[2]
d = json.load(sys.stdin)
l = d.get("layers", d) if isinstance(d, dict) else d
for x in l:
	if x.get("namespace") == ns:
		print(eval(expr, {"x": x}))
		break
else:
	print("missing")
' "$1" "$2"
}

stop_dot() {
	[ -n "$DOT_PID" ] && kill "$DOT_PID" 2>/dev/null
	DOT_PID=""
	sleep 0.4
}

# 起一个 dot 图层客户端（**先写配置再起**：规则要在它 map 那一刻就生效）。
# 第三个参数起：坐标（默认 dot.py 自己的 640,20）。
start_dot() {
	stop_dot
	HOME="$CFGDIR" python3 tools/dot.py "$@" > "$RUNDIR/dot-$RANDOM.log" 2>&1 &
	DOT_PID=$!
	local i
	for i in $(seq 1 60); do
		# 等 mapped（不是等 namespace）：入场动画是在 map 的那一刻起的
		[ "$(layer_field dot 'x["mapped"]')" = True ] && return 0
		sleep 0.1
	done
	return 1
}

# 正例：open_ms = 3000 —— 采样这一刻动画还该在跑（duration_ms 是规则给的那个数）
apply_cfg 'vidow.rule{ namespace = "^dot$", open_ms = 3000 }'
if start_dot; then
	LAY_DUR=$(layer_field dot 'x["anim"]["duration_ms"] if x["anim"] else "none"')
	[ "$LAY_DUR" = 3000 ] &&
		pass "open_ms = 3000：图层入场动画真的跑 3000ms（$LAY_DUR）" ||
		fail "open_ms = 3000 没生效（anim = $LAY_DUR）"
	[ "$(layer_field dot 'x["rule"]["open_ms"]')" = 3000 ] &&
		pass "rule.open_ms 读回来是 3000" || fail "rule.open_ms 不对"
	[ "$(layer_field dot 'x["alpha"] == 1')" = True ] &&
		fail "open_ms = 3000 时不该已经淡到位（alpha = $(layer_field dot 'x["alpha"]')）" ||
		pass "open_ms = 3000：采样这一刻还在淡入（alpha = $(layer_field dot 'x["alpha"]')）"
else
	fail "dot 没起来"
fi

# 反例：anim = false —— 同一套采样方式，这次压根没有动画，alpha 一上来就是 1
apply_cfg 'vidow.rule{ namespace = "^dot$", anim = false }'
if start_dot; then
	[ "$(layer_field dot 'x["anim"]')" = "None" ] &&
		pass "anim = false：没有 layer-open 动画" ||
		fail "anim = false 还是有动画（$(layer_field dot 'x["anim"]')）"
	[ "$(layer_field dot 'x["alpha"] == 1')" = True ] &&
		pass "anim = false：alpha 一上来就是 1（不淡入）" ||
		fail "anim = false 还在淡入（alpha = $(layer_field dot 'x["alpha"]')）"
	[ "$(layer_field dot 'x["rule"]["open_ms"]')" = 0 ] &&
		pass "anim = false 把两个时长都归零（rule.open_ms = 0）" ||
		fail "anim = false 没归零"
else
	fail "dot 没起来（anim = false）"
fi
grep -aq "rule: layer dot 动画 = 关（开 0ms / 关 0ms）" "$LOG" &&
	pass "日志里说了「layer dot 动画 = 关」" || fail "日志里没有 layer 动画那一行"

# close_ms = 0：关掉面板时不出现关闭快照（日志里说明是哪条路跳过的）
apply_cfg 'vidow.rule{ namespace = "^dot$", close_ms = 0 }'
stop_dot
sleep 0.3
grep -aq "关闭动画跳过 layer dot：close_ms = 0" "$LOG" &&
	pass "close_ms = 0：关面板时跳过关闭动画（日志说明了原因）" ||
	fail "close_ms = 0 没生效（日志里没有跳过那一行）"

# anim = false 连关也跳过，报的是另一条原因
apply_cfg 'vidow.rule{ namespace = "^dot$", anim = false }'
start_dot || fail "dot 没起来（关的那条）"
stop_dot
sleep 0.3
grep -aq "关闭动画跳过 layer dot：规则写了 anim = false" "$LOG" &&
	pass "anim = false：关的那条也跳过（日志说的是 anim = false）" ||
	fail "anim = false 的关闭没跳过"

# close_ms = 400：正例（读回来的值，以及默认配置下不该有「跳过」）
apply_cfg 'vidow.rule{ namespace = "^dot$", close_ms = 400 }'
start_dot || fail "dot 没起来（close_ms = 400）"
[ "$(layer_field dot 'x["rule"]["close_ms"]')" = 400 ] &&
	pass "close_ms = 400 读回来对得上" || fail "rule.close_ms 不对"
SKIP_BEFORE=$(grep -ac "关闭动画跳过 layer dot" "$LOG")
stop_dot
sleep 0.3
[ "$(grep -ac "关闭动画跳过 layer dot" "$LOG")" = "$SKIP_BEFORE" ] &&
	pass "close_ms = 400：真的走了关闭动画（没有再报跳过）" ||
	fail "close_ms = 400 也把关闭动画跳过了"

# 不透明度：抓帧量那个白点中心的蓝通道（底衬是黑的，白点 = 255）
if command -v grim >/dev/null && python3 -c 'import PIL' 2>/dev/null; then
	apply_cfg
	stop_clients
	sleep 0.5
	start_dot 900 40 || fail "dot 没起来（像素判据）"
	sleep 0.5
	layer_pixel() { # layer_pixel <png> —— dot 的白点中心（图层局部 900,40）的蓝通道
		./build/vidowmsg layers > "$RUNDIR/layers.json" 2>/dev/null
		python3 - "$1" "$RUNDIR/layers.json" <<'PYEOF'
import json, sys
from PIL import Image
d = json.load(open(sys.argv[2], encoding='utf-8'))
l = d.get("layers", d) if isinstance(d, dict) else d
g = next((x["geometry"] for x in l if x.get("namespace") == "dot"), None)
img = Image.open(sys.argv[1]).convert("RGB")
if g is None:
    print(-1)
else:
    x = max(0, min(img.width - 1, g["x"] + 900))
    y = max(0, min(img.height - 1, g["y"] + 40))
    print(img.getpixel((x, y))[2])
PYEOF
	}
	grim -o "$OUT" "$RUNDIR/layer-full.png" 2>/dev/null
	LB_FULL=$(layer_pixel "$RUNDIR/layer-full.png")
	apply_cfg 'vidow.rule{ namespace = "^dot$", opacity = 0.3 }'
	sleep 0.5
	grim -o "$OUT" "$RUNDIR/layer-dim.png" 2>/dev/null
	LB_DIM=$(layer_pixel "$RUNDIR/layer-dim.png")
	if [ "$LB_FULL" -gt 150 ] && [ "$LB_DIM" -lt $((LB_FULL * 6 / 10)) ]; then
		pass "图层 opacity = 0.3：白点的蓝从 $LB_FULL 掉到 $LB_DIM"
	else
		fail "图层 opacity = 0.3 没作用在画面上（原样 $LB_FULL → 现在 $LB_DIM）"
	fi
else
	echo "  （没有 grim 或 PIL，跳过图层像素判据）"
fi

# 收尾那节要看「窗口还活着」：这一节把客户端清过一轮，补一个回来
apply_cfg
open_win > /dev/null || fail "补一个窗口失败（收尾那节要它）"

# ------------------------------------------------------------ 7) 收尾
mark "7) 收尾"
grep -aq "Assertion" "$LOG" || grep -aq "abort" "$LOG" &&
	fail "日志里有 assert / abort" || pass "日志里没有 assert / abort"
./build/vidowmsg windows 2>/dev/null | grep -aq '"title": *"bluewin"' &&
	pass "窗口还活着" || fail "窗口不见了"

echo
if [ "$FAILED" = 0 ]; then
	echo "test-rules.sh: OK"
else
	echo "test-rules.sh: 有失败项"
fi
exit $FAILED
