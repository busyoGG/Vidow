#!/bin/bash
# 「凭空多出一列」的第二个入口：**从来没 map 过**的窗口也会被塞进一条列。
#
# 用户报的现场（`vidowmsg windows` dump，DP-2）：窗口 50、标题 `Input`、`mapped=false`、
# `visible=false`、`float_mode=none`，可 `column` 还在、几何 `2883,6 951x1068` ——
# 正好夹在 lutris（1926–2877）和游戏（3834–5754）两条列中间。README 237 那次修的是
# 「映射过之后又 unmap」那条路（view_unmap → leave_layout），这个窗口走的是**另一条**：
# 它一辈子没上屏，是「浮动 → 平铺」那次重判把它塞进列的
# （`vidow_float_set_mode` → `vidow_view_join_layout` → `view_ensure_column`，都不看
# 窗口上没上屏）。X11 窗口的属性（WM_TRANSIENT_FOR / WM_NORMAL_HINTS /
# `_NET_WM_WINDOW_TYPE`）在它没 map 的时候照样会变，proton / Steam 的 `Input` 那类
# 辅助窗就撞在这条路上。
#
# 判据（headless 1280x720 一块屏，默认 gaps 6 → 工作区 6,6 1268x708；0.5 档列宽
# (1268+6)×0.5−6 = 631）：
#   1) 从没 map 的窗口（A）没有列、没有几何 —— 它不占条带上的位置（修之前：一条列、
#      几何 6,6 631x708）
#   2) 屏上只剩一条列：那条「空列」没有把上屏的窗口挤开（对照组 B 还在第一格 6）
#   3) 反向对照：同一个探针进程里**上屏**的窗口（B）走同一套属性重判，照旧拿到列 ——
#      修法不能把「上屏的平铺窗口」一起拦掉
#   4) 延迟到首次 map：A 后来自己 map 上来，列补回来了（不是「永远不给列」）
#   5) 日志里写了拦下它的那一行（还没上屏，先不给列），且没有 assert / abort
#
# 用法：./test-emptycol-nomap.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 xnomap.py。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-emptycol-nomap-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
PROBE_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$PROBE_PID" ] && kill "$PROBE_PID" 2>/dev/null
	if [ -n "${VIDOW_PID:-}" ]; then
		kill -TERM -- "-$VIDOW_PID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import Xlib" 2>/dev/null || {
	echo "test-emptycol-nomap.sh: 需要 python3-xlib（tools/xnomap.py 是 X11 探针）"
	exit 1
}

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = true
EOF

# 从 `vidowmsg windows` 的 dump 里把这次要断言的几件事算出来（一条一行 key=value）。
# 两个探针窗口按标题认：`emptycol-nomap`（一辈子不 map 的那个）与
# `emptycol-mapped`（对照组的那个）。
cat > "$RUNDIR/facts.py" <<'PY'
import json
import sys

ws = [w for w in json.load(open(sys.argv[1]))
      if w.get("kind") == "x11" and not w.get("popup")]

def col_id(w):
    c = w.get("column")
    return c.get("id") if isinstance(c, dict) else c

def find(title):
    for w in ws:
        if w.get("title") == title:
            return w
    return None

a = find("emptycol-nomap")   # 从没 map 过的那个（用户的窗口 50 那种）
b = find("emptycol-mapped")  # 上屏的对照组

print("count=%d" % len(ws))
if a is not None:
    print("a_mapped=%s" % ("true" if a["mapped"] else "false"))
    # Xwayland 没给它建 wl_surface（surface=null）= 它一次都没上屏。
    print("a_no_surface=%s" % ("true" if a.get("surface") is None else "false"))
    print("a_col=%s" % (col_id(a) if col_id(a) is not None else "none"))
    print("a_w=%d" % a["geometry"]["width"])
if b is not None:
    print("b_col=%s" % (col_id(b) if col_id(b) is not None else "none"))
    print("b_x=%d" % b["geometry"]["x"])
    print("b_w=%d" % b["geometry"]["width"])
cols = {col_id(w) for w in ws if col_id(w) is not None}
print("columns=%d" % len(cols))
PY

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 40); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG"; then
	fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)

facts() {
	./build/vidowmsg windows 2>/dev/null > "$RUNDIR/windows.json"
	python3 "$RUNDIR/facts.py" "$RUNDIR/windows.json" > "$RUNDIR/facts.txt"
	cat "$RUNDIR/facts.txt"
}
get() { sed -n "s/^$1=//p" "$RUNDIR/facts.txt"; }

python3 tools/xnomap.py > "$RUNDIR/probe.out" 2>&1 &
PROBE_PID=$!
sleep 1.0
grep -aq "建窗 B" "$RUNDIR/probe.out" || {
	fail "探针没起来"; cat "$RUNDIR/probe.out" | head -5; exit 1
}

# 探针在「删掉窗口类型」（重判成平铺）之后停 2.5 秒才 map A —— 趁这窗口量。
sleep 2.5

mark "1) 从没 map 过的窗口不占列"
facts > /dev/null
[ "$(get count)" = "2" ] &&
	pass "vidow 认得这两个 X11 探针窗口" ||
	fail "探针窗口数不对（count=$(get count)）"
[ "$(get a_no_surface)" = "true" ] &&
	pass "A 一次都没上屏（Xwayland 连 wl_surface 都没给它建）" ||
	fail "A 上过屏了？（a_no_surface=$(get a_no_surface)）"
[ "$(get a_mapped)" = "false" ] &&
	pass "A 的 mapped=false" ||
	fail "A 的 mapped=$(get a_mapped)"
[ "$(get a_col)" = "none" ] &&
	pass "A 没有列（column=null）" ||
	fail "A 占着一条列（column=$(get a_col)）—— 这就是那条空列"
[ "$(get a_w)" = "0" ] &&
	pass "A 没有几何（width=0，条带上一格都没占）" ||
	fail "A 被摆进布局了（width=$(get a_w)）"
[ "$(get columns)" = "1" ] &&
	pass "屏上只有一条列（没有多出来的空列）" ||
	fail "屏上有 $(get columns) 条列（该是 1 条）"

mark "2) 反向对照：上屏的平铺窗口照旧进列"
[ "$(get b_col)" != "none" ] &&
	pass "上屏的 B 拿到了列（column=$(get b_col)）" ||
	fail "上屏的平铺窗口没拿到列 —— 拦过头了"
[ "$(get b_x)" = "6" ] && [ "$(get b_w)" = "631" ] &&
	pass "它落在第一格（6,6 631 宽，没被空列挤开）" ||
	fail "它被挤开了（x=$(get b_x) width=$(get b_w)）"

mark "3) 日志：拦下它的那一行"
grep -aq "还没上屏，先不给列" "$LOG" &&
	pass "日志里有「join layout: … 还没上屏，先不给列（等 map）」" ||
	fail "日志里没有拦下它的那一行"
grep -aqE "assert|abort|Assertion" "$LOG" &&
	fail "日志里有 assert / abort" ||
	pass "日志里没有 assert / abort"

mark "4) 延迟到首次 map：A 上屏之后列补回来"
for _ in $(seq 1 40); do
	grep -aq "A 现在才 map" "$RUNDIR/probe.out" && break
	sleep 0.25
done
sleep 1.5
facts > /dev/null
[ "$(get a_no_surface)" = "false" ] && [ "$(get a_mapped)" = "true" ] &&
	pass "A 上屏了（surface 有了、mapped=true）" ||
	fail "A 没上屏（a_no_surface=$(get a_no_surface) a_mapped=$(get a_mapped)）"
[ "$(get a_col)" != "none" ] &&
	pass "A 上屏之后拿到了列（column=$(get a_col)）—— 只是延迟，不是不给" ||
	fail "A 上屏之后还是没列（column=$(get a_col)）"
[ "$(get columns)" = "2" ] &&
	pass "屏上现在是两条列（A + B）" ||
	fail "屏上列数不对（$(get columns) 条）"

mark "5) 收尾"
kill -0 "$VIDOW_PID" 2>/dev/null &&
	pass "合成器还活着" ||
	fail "合成器没了"

echo
if [ "$FAILED" = "0" ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
