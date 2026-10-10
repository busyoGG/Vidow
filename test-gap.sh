#!/bin/bash
# 间距（gap）：一个统一值管全部 + 单边 / 窗口之间单独盖。
#
# 判据（全部读 IPC `state` 的 geometry，不截屏）：
#   1) 默认（gaps 6）= 离屏幕边 6px、窗口之间 6px，跟没有 gap 之前的样子一样
#   2) gaps_inner 只改窗口之间，不动离屏幕边
#   3) gaps 一个数管全部：屏幕边和窗口之间一起变
#   4) 单边（gaps_left / gaps_top…）只改那一边，-1 回到跟随统一值
#   5) 同列两个窗口的纵向间距 = gaps_inner（单列时列宽还是 50%）
#   6) 负值当 0（负 gap 会让工作区比屏幕还大）
#   7) 两条 50% 连着中间的 gap **正好铺满**可用区（第二条的右边缘贴到右边距）——
#      以前列宽 = 可用宽 × 比例，两条 50% 加起来比可用区多一个 gap，第二条得滚一下
#      才看得全（用户报的「两个 50 50 的窗口会因为 gap 导致一个屏装不下」）
#
# 模型（输出 1280x720、scale=1、两列各 50%）：
#   可用区 = 输出 - 四边 gap；**比例分的是连 gap 一起算的那块区域**，窗口 = 那块
#   区域 - 一个内间距（niri 的 resolve_column_width，见 server.h 的
#   vidow_column_width_px）：列宽 = floor((可用宽 + 间距) / 2) - 间距，列间距 = 间距。
#   所以两条 50% 连着中间的 gap 正好铺满可用区（这是这个测试的判据 7）。
# 注意：geometry 是**滚动之后**的位置，所以断言只看列间距、y、宽高这些跟滚动无关
# 的量，不去猜滚动量。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-gap-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	stop_clients
	if [ -n "$VIDOW_PID" ]; then
		kill -TERM -- "-$VIDOW_PID" 2>/dev/null
		sleep 0.3
		kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

python3 -c "import gi" 2>/dev/null || { echo "test-gap.sh: 需要 python3-gobject"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = false
window_open_ms = 60
window_close_ms = 60
window_move_ms = 60
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export WLR_RENDERER=${WLR_RENDERER:-}

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-gap.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

snap() { # snap <文件>
	./build/vidowmsg state > "$1" 2>/dev/null
}
setcfg() { ./build/vidowmsg set-config "$1" "$2" >/dev/null 2>&1; sleep 0.4; }

mark "开两个窗口（各自一列，各占 50%）"
for t in 1 2; do
	setsid nohup ./tools/pattern.py "w$t" >"$RUNDIR/p$t.log" 2>&1 </dev/null &
	for _ in $(seq 1 40); do grep -aq "place w$t" "$LOG" && break; sleep 0.25; done
	sleep 0.6
done
sleep 0.6
snap "$RUNDIR/s-default.json"
grep -a "config: gap" "$LOG" | head -1

mark "gaps_inner 20（只改窗口之间）"
setcfg gaps_inner 20
snap "$RUNDIR/s-inner20.json"

mark "gaps 16（一个数管：屏幕边和窗口之间一起变）"
setcfg gaps_inner -1          # 单独设过的项会盖住统一值，先放回 -1
setcfg gaps 16
snap "$RUNDIR/s-gaps16.json"

mark "gaps 40"
setcfg gaps 40
snap "$RUNDIR/s-gaps40.json"

mark "堆叠：move-window-direction -1 把第二个窗口并进第一列"
./build/vidowmsg move-window-direction -1 >/dev/null 2>&1
sleep 0.6
setcfg gaps 6
snap "$RUNDIR/s-stack6.json"

mark "同列间距 20"
setcfg gaps_inner 20
snap "$RUNDIR/s-stack20.json"

mark "负值当 0（-50）"
setcfg gaps_inner -50
snap "$RUNDIR/s-stackneg.json"

mark "单边：gaps_left 40（其余仍跟随统一值 6）"
setcfg gaps_inner -1
setcfg gaps 6
setcfg gaps_left 40
snap "$RUNDIR/s-side-left.json"

mark "单边：再加 gaps_top 30"
setcfg gaps_top 30
snap "$RUNDIR/s-side-top.json"

mark "单边：gaps_left -1 回到跟随"
setcfg gaps_left -1
snap "$RUNDIR/s-side-back.json"

mark "断言"
python3 - "$RUNDIR" <<'PY'
import json, sys, os

rundir = sys.argv[1]
failed = []

def load(name):
	d = json.load(open(os.path.join(rundir, name)))
	wins = {}
	for v in d["windows"]:
		wins[v["title"]] = v["geometry"]
	return wins

def check(cond, msg):
	if cond:
		print("  PASS  " + msg)
	else:
		print("  FAIL  " + msg)
		failed.append(msg)

# 统一值 6：可用宽 = 1280-12 = 1268 → 列宽 634；可用高 = 720-12 = 708
w = load("s-default.json")
check(set(w) == {"w1", "w2"}, "两个窗口都在（%s）" % sorted(w))
a, b = w["w1"], w["w2"]
# 两列时的 x 是滚动之后的位置，边距只能从宽高反推（640 = 1280/2 的列宽）
check(a["y"] == 6 and a["height"] == 708,
	"默认离屏幕边 6px（列高 708 = 720-12、y=6）")
check(a["width"] == 631, "默认列宽 631（= (1268+6)/2 - 6）")
check((b["x"] - a["x"]) - a["width"] == 6,
	"默认列间距 6px（相邻列 x 差 %d，列宽 %d）" % (b["x"] - a["x"], a["width"]))
check(b["x"] + b["width"] == 1280 - 6,
	"两条 50%% 正好装进一屏：第二条右边缘贴到右边距（%d + %d = %d，期望 %d）"
	% (b["x"], b["width"], b["x"] + b["width"], 1280 - 6))

# gaps_inner 20：只有列间距变，屏幕边不动
w = load("s-inner20.json")
a, b = w["w1"], w["w2"]
check((b["x"] - a["x"]) - a["width"] == 20,
	"gaps_inner 20 → 列间距 20px（%d）" % ((b["x"] - a["x"]) - a["width"]))
check(a["y"] == 6 and a["height"] == 708 and a["width"] == 624,
	"gaps_inner 不影响离屏幕边（y=%d h=%d w=%d）"
	% (a["y"], a["height"], a["width"]))

# gaps 16：一个数管全部 → 屏幕边 16、列间距 16；列宽 (1280-32)/2 = 624
w = load("s-gaps16.json")
a, b = w["w1"], w["w2"]
check(a["y"] == 16 and a["height"] == 688,
	"gaps 16 → 离屏幕边 16px、列高 720-32 = 688（y=%d h=%d）"
	% (a["y"], a["height"]))
check(a["width"] == 616 and (b["x"] - a["x"]) - a["width"] == 16,
	"gaps 16 → 列宽 616、列间距 16（w=%d 间距=%d）"
	% (a["width"], (b["x"] - a["x"]) - a["width"]))

# gaps 40：列宽 (1280-80)/2 = 600、列高 640
w = load("s-gaps40.json")
a, b = w["w1"], w["w2"]
check(a["y"] == 40 and a["height"] == 640,
	"gaps 40 → 离屏幕边 40、列高 640（y=%d h=%d）" % (a["y"], a["height"]))
check(a["width"] == 580, "gaps 40 → 列宽 580（= (1200+40)/2 - 40）")
check((b["x"] - a["x"]) - a["width"] == 40,
	"gaps 40 → 列间距 40px（%d）" % ((b["x"] - a["x"]) - a["width"]))

# 同列两个窗口：纵向间距 = gaps_inner
def stacked(name):
	w = load(name)
	check(len(w) == 2, "%s：两个窗口还在" % name)
	items = sorted(w.items(), key=lambda kv: kv[1]["y"])
	return items[0][1], items[1][1]

tg, bg = stacked("s-stack6.json")
check(tg["x"] == bg["x"], "并进同一列后 x 相同（%d / %d）" % (tg["x"], bg["x"]))
check(bg["y"] - (tg["y"] + tg["height"]) == 6,
	"同列纵向间距 6px（%d）" % (bg["y"] - (tg["y"] + tg["height"])))
check(tg["width"] == 631, "单列还是占一半（列宽 %d）" % tg["width"])
check(tg["y"] == 6 and tg["height"] == 351,
	"gaps 6 → 每格高 (708-6)/2 = 351、起点 y=6（h=%d y=%d）" % (tg["height"], tg["y"]))

tg, bg = stacked("s-stack20.json")
check(bg["y"] - (tg["y"] + tg["height"]) == 20,
	"同列纵向间距 20px（%d）" % (bg["y"] - (tg["y"] + tg["height"])))
check(tg["height"] == 344, "gaps_inner 20 → 每格高 (708-20)/2 = 344（%d）" % tg["height"])

tg, bg = stacked("s-stackneg.json")
check(bg["y"] - (tg["y"] + tg["height"]) == 0,
	"负值当 0：纵向间距 0px（%d）" % (bg["y"] - (tg["y"] + tg["height"])))
check(tg["height"] == 354, "负值当 0：每格高 708/2 = 354（%d）" % tg["height"])

# 单边设置（前置：gaps 6 + gaps_inner -1，可用高 708）
def one_col(name):
	w = load(name)
	check(len(w) == 2, "%s：两个窗口还在" % name)
	items = sorted(w.items(), key=lambda kv: kv[1]["y"])
	return items[0][1], items[1][1]

top, bot = one_col("s-side-left.json")
check(top["x"] == 40, "gaps_left 40 → 窗口左边距 40px（%d）" % top["x"])
check(top["width"] == 614,
	"左边距 40 后列宽 = (1234+6)/2 - 6 = 614（%d）" % top["width"])
check(top["y"] == 6 and top["height"] == 351,
	"上下没设，还是统一值 6（y=%d h=%d）" % (top["y"], top["height"]))

top, bot = one_col("s-side-top.json")
check(top["y"] == 30, "gaps_top 30 → 窗口上边距 30px（%d）" % top["y"])
check(top["height"] == 339, "上边距 30 后每格高 = (720-30-6-6)/2 = 339（%d）"
	% top["height"])
check(top["x"] == 40 and top["width"] == 614,
	"左边距 40、列宽 614 不受影响（x=%d w=%d）" % (top["x"], top["width"]))

top, bot = one_col("s-side-back.json")
check(top["x"] == 6 and top["width"] == 631,
	"gaps_left -1 回到跟随统一值 → 左边距 6、列宽 631（x=%d w=%d）"
	% (top["x"], top["width"]))
check(top["y"] == 30, "gaps_top 30 仍然生效（y=%d）" % top["y"])

print()
if failed:
	print("test-gap.sh: %d 项没通过" % len(failed))
	for f in failed:
		print("  - " + f)
	sys.exit(1)
print("全部通过 / test-gap.sh: OK")
PY
ASSERTED=$?
[ $ASSERTED -eq 0 ] || FAILED=1
exit $FAILED
