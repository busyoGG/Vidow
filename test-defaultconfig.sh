#!/bin/bash
# 默认配置（按类别拆成 configs/ 的一套）+ 配置「族」的表写法。
#
# 判据：
#   1) `vidow --default-config` 打的是入口（只有 vidow.include 行），
#      `--default-config all` 打的是全部分类文件拼在一起的那一份
#   2) 拿它们当配置跑，每个键的值都和「什么都不配」时一模一样
#      （入口 + configs/ 那一套、以及 all 拼出来的单文件，两条路都验）
#   3) 仓库里的 config.example/ 和 --default-config 打出来的一套逐字一致
#      （入口 + 每一份分类文件，文件清单也要一致 —— 少一份就是漂移）
#   3b) 这份打出来的配置就是配置项的手册，它得全：源码里能配的东西（config.c 的
#      entries[] 平铺键、*_fields[] 族项、script.c 里注册的 vidow.* 函数、
#      vidow.rule / vidow.output 读的表字段）逐个都得在 config.example/ 里出现过
#   4) --write-config 把整套写进配置目录；已存在的不覆盖（除非 --force）
#   5) 表写法 = 平铺键：gaps = { top=30, inner=20 } → gaps_top/gaps_inner，
#      而 gaps 自己不变（表只盖写了的项）；border/blur/anim/ipc 同理
#   6) 族名当一个值用：blur = false → blur_enabled = false
#   7) 表里拼错的项名、给只认表的族（border/shadow/anim/portal）写一个值，
#      都当场报错，而且不把后面整段配置带走
#   8) --help / 不认识的参数 / --default-config 写了个不存在的名字
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

mark "1) 打印入口 / 全部分类文件"
"$VIDOW" --default-config > "$RUNDIR/entry.lua" 2> "$RUNDIR/gen.err"
if [ -s "$RUNDIR/entry.lua" ]; then
	pass "入口打出来了（$(wc -l < "$RUNDIR/entry.lua") 行）"
else
	fail "什么都没打出来"
fi
grep -q '^vidow.include("configs/01-gaps.lua")' "$RUNDIR/entry.lua" &&
	pass "入口里是 include 行" || fail "入口里没有 include 行"
grep -qE '^[a-z_]+ = ' "$RUNDIR/entry.lua" &&
	fail "入口里不该有配置键（应该只有 include）" ||
	pass "入口里没有配置键（$(grep -c '^vidow.include' "$RUNDIR/entry.lua") 行 include）"
"$VIDOW" --default-config all > "$RUNDIR/default.lua" 2>> "$RUNDIR/gen.err"
if [ -s "$RUNDIR/default.lua" ]; then
	pass "全部拼一起也打出来了（$(wc -l < "$RUNDIR/default.lua") 行）"
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

mark "2) 仓库里的 config.example/ 就是它打出来的那一套"
if diff -u config.example/config.lua "$RUNDIR/entry.lua" > "$RUNDIR/gen.diff" 2>&1; then
	pass "config.example/config.lua 和 --default-config 一模一样"
else
	fail "config.example/config.lua 跟 --default-config 走散了（改完记得重新生成）"
	head -20 "$RUNDIR/gen.diff"
fi
# 文件清单：磁盘上的分类文件 == 入口里 include 的那些
ls config.example/configs | sort > "$RUNDIR/files.txt"
sed -n 's|^vidow.include("configs/\(.*\)").*|\1|p' "$RUNDIR/entry.lua" |
	sort > "$RUNDIR/included.txt"
if diff -u "$RUNDIR/files.txt" "$RUNDIR/included.txt" > "$RUNDIR/files.diff" 2>&1; then
	pass "configs/ 下的 $(wc -l < "$RUNDIR/files.txt") 份和入口 include 的一一对上"
else
	fail "configs/ 里的文件和入口 include 的对不上"
	head -10 "$RUNDIR/files.diff"
fi
# 每一份：仓库里那份 == --default-config <名字>（.lua 后缀、数字前缀都能省）
ONES=0
for f in config.example/configs/*.lua; do
	base=$(basename "$f")
	"$VIDOW" --default-config "$base" > "$RUNDIR/one.lua" 2>> "$RUNDIR/gen.err" ||
		{ fail "$base：--default-config $base 失败了"; continue; }
	diff -q "$f" "$RUNDIR/one.lua" >/dev/null 2>&1 ||
		{ fail "$base 和 --default-config $base 不一致"; continue; }
	short=${base#*-}; short=${short%.lua}
	"$VIDOW" --default-config "$short" > "$RUNDIR/one2.lua" 2>/dev/null &&
		diff -q "$f" "$RUNDIR/one2.lua" >/dev/null 2>&1 ||
		fail "省掉数字前缀的写法（--default-config $short）没对上 $base"
	ONES=$((ONES + 1))
done
[ "$ONES" -gt 0 ] && pass "每一份分类文件都和仓库里那份一致（$ONES 份）" ||
	fail "一份分类文件都没比对到"

mark "2b) 配置说明的完整性：源码里能配的东西都写进了这份默认配置"
# 打出来的那份就是「配置项的手册」—— 它得**全**。这里直接从源码里把能配的东西
# 抠出来（config.c 的 entries[] 平铺键、*_fields[] 族项、script.c 里注册的 vidow.*
# 函数、以及 vidow.rule / vidow.output 读的那些表字段），逐个去 config.example/ 里
# 找：找不到就是「这个配置项没写进说明」。
python3 - "$RUNDIR" <<'PYCFG'
import os, re, sys
rundir = sys.argv[1]
cfg = open("src/config.c", encoding="utf-8").read()
script = open("src/script.c", encoding="utf-8").read()
rule = open("src/rule.c", encoding="utf-8").read()

flat = re.findall(r'^\t\{ "([a-zA-Z0-9_]+)",\s*K_', cfg, re.M)
fields = []
for m in re.finditer(r'static const struct group_field \w+_fields\[\] = \{', cfg):
    i = cfg.index("{", m.end() - 1)
    j = cfg.index("};", i)
    fields += [f for f, _ in re.findall(r'\{ "([^"]+)",\s*"([^"]+)" \}', cfg[i:j])]
api = re.findall(r'^\t\t\{ "([a-z_]+)", l_[a-z_]+ \},', script, re.M) + ["include"]
# vidow.rule 的字段表在 rule.c（匹配和作用同一张表，见 vidow_rule_fields[]）
tbl = re.findall(r'^\t\{ "([a-z0-9_]+)",\s*RK_', rule, re.M)
tbl += re.findall(r'(?:lua_getfield\(L, 1, |table_string\(L, 1, )"([a-z0-9_]+)"',
                  script)
tbl += re.findall(r'lua_getfield\(L, 2, "([a-z0-9_]+)"', script)

doc = ""
for f in sorted(os.listdir("config.example/configs")):
    doc += open(os.path.join("config.example/configs", f), encoding="utf-8").read()
doc += open("config.example/config.lua", encoding="utf-8").read()

groups = [
    ("平铺键（entries[]）", flat),
    ("族里的项（*_fields[]）", fields),
    ("vidow.* 的函数", api),
    ("表参数里的字段（vidow.rule 在 rule.c / vidow.output 在 script.c）", tbl),
]
bad = 0
for name, names in groups:
    miss = sorted({n for n in names
                   if not re.search(r'\b' + re.escape(n) + r'\b', doc)})
    if miss:
        print("  FAIL  %s 里有 %d 项没写进配置说明：%s"
              % (name, len(miss), " ".join(miss)))
        bad += len(miss)
    else:
        print("  PASS  %s 的 %d 项都写进了配置说明" % (name, len(set(names))))
sys.exit(1 if bad else 0)
PYCFG
[ $? -eq 0 ] || FAILED=1

mark "3) --help 和不认识的参数"
"$VIDOW" --help 2>&1 | grep -q "default-config" && pass "--help 提到了 --default-config" ||
	fail "--help 没提 --default-config"
"$VIDOW" --help 2>&1 | grep -q "write-config" && pass "--help 提到了 --write-config" ||
	fail "--help 没提 --write-config"
"$VIDOW" --nope >/dev/null 2>&1 && fail "不认识的参数居然返回 0" ||
	pass "不认识的参数非 0 退出"
"$VIDOW" --default-config 99-nope >/dev/null 2> "$RUNDIR/nope.err" &&
	fail "--default-config 写了个不存在的名字居然返回 0" ||
	pass "--default-config 写了个不存在的名字非 0 退出"
grep -q "01-gaps.lua" "$RUNDIR/nope.err" &&
	pass "报错里列出了能写的名字：$(head -c 80 "$RUNDIR/nope.err")…" ||
	fail "报错里没列出能写的名字"

mark "3b) --write-config：把整套（入口 + configs/）写进配置目录"
# 全新的 HOME：连 ~/.config 都没有，父目录得自己建出来
WCHOME="$RUNDIR/wchome"
mkdir -p "$WCHOME"
HOME="$WCHOME" "$VIDOW" --write-config > "$RUNDIR/wc.out" 2> "$RUNDIR/wc.err"
WCPATH=$(head -1 "$RUNDIR/wc.out")
WCCFG="$WCHOME/.config/vidow"
[ "$WCPATH" = "$WCCFG/config.lua" ] &&
	pass "写到了配置目录：$WCPATH" || fail "路径不对：'$WCPATH'"
[ -s "$WCPATH" ] && pass "入口有了（$(wc -l < "$WCPATH") 行）" || fail "入口是空的"
WN=$(ls "$WCCFG/configs" | wc -l)
[ "$WN" -eq "$(wc -l < "$RUNDIR/files.txt")" ] &&
	pass "分类文件也写了（$WN 份）" || fail "分类文件只写了 $WN 份"
diff -q "$RUNDIR/entry.lua" "$WCPATH" >/dev/null 2>&1 &&
	pass "入口内容 == --default-config 打出来的那份" ||
	fail "入口内容和 --default-config 不一致"
DIFFS=0
for f in config.example/configs/*.lua; do
	diff -q "$f" "$WCCFG/configs/$(basename "$f")" >/dev/null 2>&1 || DIFFS=$((DIFFS + 1))
done
[ "$DIFFS" -eq 0 ] && pass "每一份分类文件的内容都对" ||
	fail "有 $DIFFS 份分类文件的内容不对"

# 已经有一份了：不动它（配置目录里那份是用户改过的）
echo "-- 我自己改的一行" >> "$WCPATH"
echo "-- 我改过的间距" >> "$WCCFG/configs/01-gaps.lua"
HOME="$WCHOME" "$VIDOW" --write-config >/dev/null 2> "$RUNDIR/wc2.err"
[ $? -ne 0 ] && pass "第二遍拒绝覆盖（非 0 退出）" || fail "第二遍居然覆盖了"
grep -q "已经有一份了" "$RUNDIR/wc2.err" &&
	pass "并且说清楚了原因：$(head -1 "$RUNDIR/wc2.err")" || fail "没给原因"
grep -q "我自己改的一行" "$WCPATH" && pass "入口一个字没动" ||
	fail "入口被改掉了"
grep -q "我改过的间距" "$WCCFG/configs/01-gaps.lua" &&
	pass "分类文件也没动" || fail "分类文件被改掉了"
HOME="$WCHOME" "$VIDOW" --write-config --force >/dev/null 2>&1 &&
	pass "--force 才覆盖" || fail "--force 没能覆盖"
grep -q "我自己改的一行" "$WCPATH" && fail "--force 之后还是老内容" ||
	pass "--force 之后入口是新的"
grep -q "我改过的间距" "$WCCFG/configs/01-gaps.lua" &&
	fail "--force 之后分类文件还是老内容" ||
	pass "--force 之后分类文件也是新的"

mark "4) 什么都不配 vs 默认配置（入口 + configs/）vs 全部拼成一份"
mkdir -p "$RUNDIR/empty"
run_config "$RUNDIR/empty/none.lua" none
# 入口 + configs/：就用 --write-config 写出来、又 --force 复原过的那一套
run_config "$WCCFG/config.lua" default
# all 拼出来的单文件：include 就是就地展开，所以这条也得一模一样
run_config "$RUNDIR/default.lua" flat

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
-- （用还没出现过的键名：配置键第一次赋值之后就是普通全局变量了，同一个键
-- 写两遍只有第一遍生效。）
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

none, dflt, flat, table = (load("none"), load("default"), load("flat"),
	load("table"))

# 2) 完整默认配置 == 什么都没配（入口 + configs/ 那一套）
diffs = {k: (none.get(k), dflt.get(k)) for k in set(none) | set(dflt)
		if none.get(k) != dflt.get(k)}
check(not diffs, "入口 + configs/ 读进来后每个键都和内置默认值一样（%d 个键，%d 处不同）"
	% (len(none), len(diffs)))
for k, (a, b) in list(diffs.items())[:5]:
	print("      %s: 无配置 %r → 默认配置 %r" % (k, a, b))

# 2b) --default-config all 拼出来的单文件 == 同一套
fdiffs = {k: (none.get(k), flat.get(k)) for k in set(none) | set(flat)
		if none.get(k) != flat.get(k)}
check(not fdiffs, "全部拼成一份（--default-config all）也一样（%d 处不同）"
	% len(fdiffs))
for k, (a, b) in list(fdiffs.items())[:5]:
	print("      %s: 无配置 %r → all %r" % (k, a, b))

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
check(table["blur_enabled"] == "false"
	and table["blur_levels"] == none["blur_levels"],
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
