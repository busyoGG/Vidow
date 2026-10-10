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
#   8) 日志里没有 assert / abort，窗口还在
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
	'vidow.rule{ app_id = "!mpv", xwayland = false, capture = "hide" }' \
	'vidow.rule{ literal = true, title = "a.b", capture = "hide" }' \
	'vidow.rule{ pid = 4242, exe = "^mpv$", cmdline = "mpv --no-config",' \
	'             layer = "overlay", on_output = "DP-1", floating = true,' \
	'             fixed_size = false, initial_title = "^新", role = "pop-up",' \
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
	eq(4, "xwayland", False)
	eq(5, "literal", True)
	eq(5, "title", "a.b")
	eq(6, "pid", 4242)
	eq(6, "exe", "^mpv$")
	eq(6, "cmdline", "mpv --no-config")
	eq(6, "layer", "overlay")
	eq(6, "on_output", "DP-1")
	eq(6, "floating", True)
	eq(6, "fixed_size", False)
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
case_state "xwayland = false 命中"        hide 'vidow.rule{ xwayland = false, capture = "hide" }'
case_state "xwayland = true 不命中"       off  'vidow.rule{ xwayland = true, capture = "hide" }'
case_state "floating = false 命中"        hide 'vidow.rule{ floating = false, capture = "hide" }'
case_state "floating = true 不命中"       off  'vidow.rule{ floating = true, capture = "hide" }'
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

# ------------------------------------------------------------ 5) 收尾
mark "5) 收尾"
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
