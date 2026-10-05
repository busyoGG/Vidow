#!/bin/bash
# 完整默认配置（vidow --default-config）+ 配置「族」的表写法。
#
# 判据：
#   1) `vidow --default-config` 打出来的 Lua 能直接当配置读（不报错、不半途死）
#   2) 拿它当配置跑，每个键的值都和「什么都不配」时一模一样
#   3) 表写法 = 平铺键：gaps = { top=30, inner=20 } → gaps_top/gaps_inner，
#      而 gaps 自己不变（表只盖写了的项）；border/blur/anim/ipc 同理
#   4) 族名当一个值用：blur = false → blur_enabled = false
#   5) 表里拼错的项名、给只认表的族（border/shadow/anim/portal）写一个值，
#      都当场报错，而且不把后面整段配置带走
#   6) --help / 不认识的参数
set -u
cd "$(dirname "$0")"

VIDOW=${VIDOW_BIN:-./build/vidow}
RUNDIR=/tmp/vidow-defaultconfig-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	stop_vidow
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

stop_vidow() {
	[ -n "$VIDOW_PID" ] || return
	kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	VIDOW_PID=""
}

# run_config <配置文件> <标签>：跑一次合成器，把 IPC 里的配置 dump 出来
run_config() {
	rm -f "$LOG"
	VIDOW_CONFIG="$1" setsid "$VIDOW" > "$LOG" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	if ! grep -aq "vidow ready" "$LOG"; then
		fail "$2：vidow 没起来"
		grep -a "ERROR" "$LOG" | head -5
		return 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	./build/vidowmsg config > "$RUNDIR/cfg-$2.json" 2>/dev/null
	stop_vidow
}

mark "1) 打印完整默认配置"
"$VIDOW" --default-config > "$RUNDIR/default.lua" 2> "$RUNDIR/gen.err"
if [ -s "$RUNDIR/default.lua" ]; then
	pass "打出来了（$(wc -l < "$RUNDIR/default.lua") 行）"
else
	fail "什么都没打出来"
fi
# 每个键都得在：entries[] 里的键全在 IPC 的 config 里，两边的键名对一遍
python3 - "$RUNDIR/default.lua" "$RUNDIR" <<'PY'
import re, sys
lua, rundir = sys.argv[1], sys.argv[2]
keys = set(re.findall(r'^([a-z_][a-z0-9_]*) = ', open(lua, encoding='utf-8').read(), re.M))
open(rundir + "/keys.txt", "w").write("\n".join(sorted(keys)))
print("  默认配置里 %d 个键" % len(keys))
PY

mark "2) 仓库里的 config.lua.example 就是它打出来的那份"
if diff -u config.lua.example "$RUNDIR/default.lua" > "$RUNDIR/gen.diff" 2>&1; then
	pass "config.lua.example 和 --default-config 一模一样"
else
	fail "config.lua.example 跟 --default-config 走散了（改完记得重新生成）"
	head -20 "$RUNDIR/gen.diff"
fi

mark "3) --help 和不认识的参数"
"$VIDOW" --help 2>&1 | grep -q "default-config" && pass "--help 提到了 --default-config" ||
	fail "--help 没提 --default-config"
"$VIDOW" --nope >/dev/null 2>&1 && fail "不认识的参数居然返回 0" ||
	pass "不认识的参数非 0 退出"

mark "4) 什么都不配 vs 完整默认配置"
mkdir -p "$RUNDIR/empty"
run_config "$RUNDIR/empty/none.lua" none
run_config "$RUNDIR/default.lua" default

mark "5) 表写法"
cat > "$RUNDIR/table.lua" <<'EOF'
scale = "1"
xwayland = false
gaps = { top = 30, inner = 20 }        -- 只盖上边距和窗口之间，其余跟随统一值
border = { width = 0, color = "#ff0000" }
blur = false                            -- 族名 = 一个值（blur_enabled）
anim = { open_ms = 77 }
ipc = { enabled = true }
-- 错误用例：项名拼错、给只认表的族写一个值。都要当场报错，用 pcall 兜住 ——
-- 一句写错不该把后面整段配置带走。
-- （用还没出现过的键名：配置键第一次赋值之后就是普通全局变量了，见
-- config.lua.example 里那句「同一个键写两遍只有第一遍生效」。）
vidow.log("E1 " .. tostring((select(2, pcall(function() shadow = { nope = 1 } end)))))
vidow.log("E2 " .. tostring((select(2, pcall(function() portal = 1 end)))))
corner_radius = 7
-- 老键名（gaps 那一家的旧名字）现在没了：不能静默失效，得喊一声
window_gaps_outer = 40
EOF
run_config "$RUNDIR/table.lua" table

mark "6) 断言"
python3 - "$RUNDIR" <<'PY'
import json, os, re, sys

rundir = sys.argv[1]
failed = []

def check(cond, msg):
	if cond:
		print("  PASS  " + msg)
	else:
		print("  FAIL  " + msg)
		failed.append(msg)

def load(name):
	with open(os.path.join(rundir, "cfg-%s.json" % name), encoding='utf-8') as f:
		return json.load(f)

none, dflt, table = load("none"), load("default"), load("table")

# 2) 完整默认配置 == 什么都没配
diffs = {k: (none.get(k), dflt.get(k)) for k in set(none) | set(dflt)
		if none.get(k) != dflt.get(k)}
check(not diffs, "完整默认配置读进来后每个键都和内置默认值一样（%d 个键，%d 处不同）"
	% (len(none), len(diffs)))
for k, (a, b) in list(diffs.items())[:5]:
	print("      %s: 无配置 %r → 默认配置 %r" % (k, a, b))

# 3) 表写法：写了的项盖住，没写的跟着统一值/默认值
check(table["gaps"] == none["gaps"] == "6",
	"gaps = { top=30, inner=20 } 不动统一值（gaps=%s）" % table["gaps"])
check(table["gaps_top"] == "30", "→ gaps_top = 30（%s）" % table["gaps_top"])
check(table["gaps_inner"] == "20", "→ gaps_inner = 20（%s）" % table["gaps_inner"])
check(table["gaps_bottom"] == "-1" and table["gaps_left"] == "-1"
	and table["gaps_right"] == "-1", "没写的那几边还是 -1（跟随）")
check(table["border_width"] == "0" and table["border_color"] == "#ff0000ff",
	"border = { width=0, color=\"#ff0000\" } → width=0、color=#ff0000ff（%s / %s）"
	% (table["border_width"], table["border_color"]))
check(table["border_color_focus"] == none["border_color_focus"],
	"border 没写 color_focus → 还是默认值（%s）" % table["border_color_focus"])
check(table["blur_enabled"] == "false" and table["blur_levels"] == "4",
	"blur = false → blur_enabled=false、levels 没动（%s / %s）"
	% (table["blur_enabled"], table["blur_levels"]))
check(table["window_open_ms"] == "77" and table["window_close_ms"] == "140",
	"anim = { open_ms=77 } → 开 77、关还是 140（%s / %s）"
	% (table["window_open_ms"], table["window_close_ms"]))
check(table["corner_radius"] == "7", "错误用例之后的配置照样生效（corner_radius=%s）"
	% table["corner_radius"])

# 除了上面这些，其余全和默认一致
changed = {"gaps_top", "gaps_inner", "border_width", "border_color",
	"blur_enabled", "window_open_ms", "corner_radius",
	"scale", "xwayland"}   # 这两行是测试自己为 headless 写的
extra = {k for k in set(none) | set(table)
		if none.get(k) != table.get(k)} - changed
check(not extra, "表写法只改了预期的那几个键（多改的：%s）" % sorted(extra))

# 4) 错误信息
log = open(os.path.join(rundir, "log"), encoding='utf-8', errors='replace').read()
check("没有 'nope' 这一项" in log, "项名拼错会报错（并列出能写的项名）")
check("得写成一张表" in log, "给只认表的族写值会报错")
check("corner_radius = 7" in log or table["corner_radius"] == "7",
	"报错不会把后面的配置带走")
check("这个键没有了" in log and "window_gaps_outer" in log,
	"老键名（window_gaps_outer）会喊一声，不静默失效")
check("window_gaps_outer" not in table, "老键名也确实不存在于配置里")

print()
if failed:
	print("test-defaultconfig.sh: %d 项没通过" % len(failed))
	for f in failed:
		print("  - " + f)
	sys.exit(1)
print("全部通过 / test-defaultconfig.sh: OK")
PY
ASSERTED=$?
[ $ASSERTED -eq 0 ] || FAILED=1
exit $FAILED
