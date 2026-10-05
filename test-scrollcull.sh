#!/bin/bash
# 滚出视口的列不该画到相邻显示器上。
#
# 为什么会有这个问题：wlr_scene 是**一张全局坐标**的场景图，每块屏的场景输出画的
# 是「落在自己那块坐标里的所有节点」。列往右滚出屏宽之后，它的坐标正好落进右边那
# 块屏的框里 —— 于是就在隔壁显示器上出现了（niri 不会：它每块屏各渲染自己那棵，
# 屏外的部分被 framebuffer 自然裁掉）。
#
# 判据：
#   1) 三列里最左边那列滚出左边之后，它被「藏起」（不在场景里）
#   2) 焦点回到最左列时，第三列（x 落在右边那块屏的框里）被藏起 —— 修之前它正好
#      画在隔壁显示器上
#   3) 再滚回去，第三列重新显示、第一列又藏起（不是一次性关掉）
#   4) 藏起来的窗口照样在出帧（视口外补帧，不是被冻住）
#   5) 浮动窗口 / 拖拽不受这条规则管（允许跨屏）—— 见 test-drag.sh
#
# 用法：./test-scrollcull.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-scrollcull-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v python3 >/dev/null || { echo "test-scrollcull.sh: 需要 python3"; exit 1; }
python3 -c "import gi; gi.require_version('Gtk','3.0')" 2>/dev/null ||
	{ echo "test-scrollcull.sh: 需要 python3-gobject (GTK3)"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
-- 两块 1280 宽的屏，列宽 0.5：三列（634+6+634+6+634）装不下，必然有列越界
outputs = "HEADLESS-1:1 HEADLESS-2:1"
gaps = 6
scale = "1"
-- 补帧是「规则要求保活」才做的事（默认不补：滚出视口的窗口本来就该歇着）。
-- cull-1 用来说明：剔除只影响「画不画」，不影响这条规则要求的补帧。
vidow.rule{ title = "cull-1", force_render = true }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

mark "0) 两块屏并排"
./build/vidowmsg outputs | python3 -c '
import json, sys
outs = json.load(sys.stdin)
if isinstance(outs, dict):
    outs = outs.get("outputs", [])
boxes = {o["name"]: (o["layout"]["x"], o["layout"]["width"]) for o in outs}
print("      " + ", ".join("%s x=%d w=%d" % (n, x, w) for n, (x, w) in sorted(boxes.items())))
ok = len(boxes) == 2 and sorted(x for x, _ in boxes.values()) == [0, 1280]
sys.exit(0 if ok else 1)
' && pass "HEADLESS-1 在 0、HEADLESS-2 在 1280（相邻）" ||
	fail "两块屏不是并排的 0/1280"

# 三个窗口 = 三列
for i in 1 2 3; do
	setsid nohup python3 ./tools/heartbeat.py "cull-$i" > "$RUNDIR/beat-$i.log" 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 60); do grep -aq "place cull-$i -> " "$LOG" && break; sleep 0.2; done
	sleep 0.4
done
sleep 1

place() { grep -a "place cull-$1 -> " "$LOG" | tail -1; }
shown() { grep -a "workspace: cull-$1 显示" "$LOG" | wc -l; }
hidden() { grep -a "workspace: cull-$1 藏起" "$LOG" | wc -l; }
count() { grep -ac "$1" "$LOG" 2>/dev/null || true; }

mark "1) 三列装不下：最左那列滚出左边 → 藏起"
echo "      $(place 1)"
echo "      $(place 3)"
[ "$(hidden 1)" -ge 1 ] && pass "cull-1 被藏起（滚到屏左边外面）" ||
	fail "cull-1 还留在场景里（$(place 1)）"
[ "$(hidden 3)" -eq 0 ] && pass "cull-3 显示着（焦点在它身上，视口滚到了右边）" ||
	fail "cull-3 不该被藏起"

mark "2) 焦点回到最左列：第三列的 x 落进右边那块屏的框 → 藏起"
./build/vidowmsg focus-column -1 >/dev/null 2>&1
sleep 0.4
./build/vidowmsg focus-column -1 >/dev/null 2>&1
sleep 1.0
echo "      $(place 1)"
echo "      $(place 3)"
python3 - "$LOG" <<'PY'
import re, sys
log = open(sys.argv[1], 'rb').read().decode('utf-8', 'replace')
def last(title):
    m = None
    for m in re.finditer(r"place %s -> (-?\d+),(-?\d+) (\d+)x(\d+)" % title, log):
        pass
    return m
x3 = last("cull-3")
if x3 is None:
    print("  FAIL  日志里没有 cull-3 的 place")
    sys.exit(1)
x, w = int(x3.group(1)), int(x3.group(3))
inside_neighbor = x < 1280 + 1280 and x + w > 1280
print("  %s  cull-3 在 x=%d（宽 %d），%s隔壁屏的框（1280..2560）"
      % ("PASS" if inside_neighbor else "FAIL", x, w,
         "落在" if inside_neighbor else "没落在"))
sys.exit(0 if inside_neighbor else 1)
PY
[ $? -eq 0 ] || FAILED=1
[ "$(hidden 3)" -ge 1 ] && pass "cull-3 被藏起（不再画到隔壁显示器上）" ||
	fail "cull-3 还在场景里 —— 它的坐标落进隔壁屏，会串屏"
[ "$(shown 1)" -ge 1 ] && pass "cull-1 重新显示（焦点回到它身上）" ||
	fail "cull-1 没被放回来"

mark "3) 再滚回右边：第三列显示、第一列又藏起"
n1=$(count "workspace: cull-3 显示")
./build/vidowmsg focus-column 1 >/dev/null 2>&1
sleep 0.4
./build/vidowmsg focus-column 1 >/dev/null 2>&1
sleep 1.0
[ "$(count "workspace: cull-3 显示")" -gt "$n1" ] &&
	pass "cull-3 又显示了（剔除是可逆的，不是关掉一次就没了）" ||
	fail "cull-3 没被放回来"
[ "$(hidden 1)" -ge 2 ] && pass "cull-1 又被藏起" || fail "cull-1 还在画"

mark "4) 藏起来的窗口照样出帧（规则要求保活 → 视口外补帧）"
before=$(grep -c "^frame " "$RUNDIR/beat-1.log" 2>/dev/null || echo 0)
sleep 1.2
after=$(grep -c "^frame " "$RUNDIR/beat-1.log" 2>/dev/null || echo 0)
[ "$after" -gt "$before" ] &&
	pass "cull-1 藏起来之后还在出帧（$before → $after 帧）" ||
	fail "cull-1 被冻住了（帧数 $before → $after）"

mark "5) 没崩、没断言"
pgrep -x vidow >/dev/null && pass "合成器还活着" || fail "合成器挂了"
grep -aq "Assertion" "$LOG" && fail "日志里有 assert" || pass "日志里没有 assert"

echo
if [ $FAILED -eq 0 ]; then
	echo "test-scrollcull.sh: OK"
else
	echo "test-scrollcull.sh: 有失败项"
fi
exit $FAILED
