#!/bin/bash
# 间距（gap）：niri 那套四个值 + `gaps <n>` 简写。
#
# 判据（全部读 IPC `state` 的 geometry，不截屏）：
#   1) 默认和加 gap 之前一模一样：离屏幕边 6px、窗口之间 6px
#   2) window_gaps_inner 只改窗口之间，不动离屏幕边
#   3) gaps <n> 一次把四个都改掉
#   4) workspace_gaps_outer + window_gaps_outer 相加才是「离屏幕边」的距离
#   5) 同列两个窗口的纵向间距 = window_gaps_inner
#   6) 负值当 0（负 gap 会让工作区比屏幕还大）
#   7) 四边单独设置：gaps_left/top 只改那一边，-1 回到跟随统一值
#
# 注意：geometry 是**滚动之后**的位置，所以断言只看「列间距」「y」「宽高」这些
# 与滚动无关的量，不去猜滚动量。
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

mark "window_gaps_inner 20（只改窗口之间）"
setcfg window_gaps_inner 20
snap "$RUNDIR/s-inner20.json"

mark "gaps 16（简写：一次设满四个）"
setcfg gaps 16
snap "$RUNDIR/s-gaps16.json"

mark "window_gaps_outer 0 + workspace_gaps_outer 40（离屏幕边 40）"
setcfg window_gaps_outer 0
setcfg workspace_gaps_outer 40
snap "$RUNDIR/s-outer40.json"

mark "堆叠：move-window-direction -1 把第二个窗口并进第一列"
./build/vidowmsg move-window-direction -1 >/dev/null 2>&1
sleep 0.6
setcfg gaps 6
snap "$RUNDIR/s-stack6.json"

mark "同列间距 20"
setcfg window_gaps_inner 20
snap "$RUNDIR/s-stack20.json"

mark "负值当 0（-50）"
setcfg window_gaps_inner -50
snap "$RUNDIR/s-stackneg.json"

mark "四边单独：gaps_left 40（其余仍跟随统一值 6）"
setcfg gaps 6
setcfg gaps_left 40
snap "$RUNDIR/s-side-left.json"

mark "四边单独：再加 gaps_top 30"
setcfg gaps_top 30
snap "$RUNDIR/s-side-top.json"

mark "四边单独：gaps_left -1 回到跟随"
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

def outer_of(g):
	# 列宽 = (输出宽 - 2*离屏幕边) * 0.5，输出宽 1280
	return 640 - g["width"]

# --- 1) 默认：离屏幕边 6、窗口之间 6、列宽 634、列高 708
w = load("s-default.json")
check(set(w) == {"w1", "w2"}, "两个窗口都在（%s）" % sorted(w))
a, b = w["w1"], w["w2"]
check(outer_of(a) == 6, "默认离屏幕边 6px（列宽 %d）" % a["width"])
check(a["y"] == 6 and a["height"] == 708,
	"默认 y=6、列高 708（y=%d h=%d）" % (a["y"], a["height"]))
check((b["x"] - a["x"]) - a["width"] == 6,
	"默认列间距 6px（相邻列 x 差 %d，列宽 %d）" % (b["x"] - a["x"], a["width"]))

# --- 2) window_gaps_inner 20：只有列间距变
w = load("s-inner20.json")
a, b = w["w1"], w["w2"]
check((b["x"] - a["x"]) - a["width"] == 20,
	"window_gaps_inner 20 → 列间距 20px（%d）" % ((b["x"] - a["x"]) - a["width"]))
check(outer_of(a) == 6 and a["y"] == 6,
	"inner 不影响离屏幕边的距离（仍是 %dpx）" % outer_of(a))

# --- 3) gaps 16：四个一起变
w = load("s-gaps16.json")
a, b = w["w1"], w["w2"]
check(outer_of(a) == 32, "gaps 16 → 离屏幕边 32px（= 16 + 16，列宽 %d）" % a["width"])
check(a["y"] == 32 and a["height"] == 656,
	"gaps 16 → y=32、列高 656（y=%d h=%d）" % (a["y"], a["height"]))
check((b["x"] - a["x"]) - a["width"] == 16,
	"gaps 16 → 列间距 16px（%d）" % ((b["x"] - a["x"]) - a["width"]))

# --- 4) outer 40 + window_outer 0：相加才是离屏幕边
w = load("s-outer40.json")
a, b = w["w1"], w["w2"]
check(outer_of(a) == 40, "workspace_outer 40 + window_outer 0 → 离屏幕边 40px（%d）"
	% outer_of(a))
check(a["y"] == 40 and a["height"] == 640,
	"同上 y=40、列高 640（y=%d h=%d）" % (a["y"], a["height"]))
check((b["x"] - a["x"]) - a["width"] == 16,
	"只改了 outer，列间距还是 16px（%d）" % ((b["x"] - a["x"]) - a["width"]))

# --- 5) 同列两个窗口：纵向间距 = window_gaps_inner
def stacked(name):
	w = load(name)
	check(len(w) == 2, "%s：两个窗口还在" % name)
	# 同一列：x 相同；上下两格
	items = sorted(w.items(), key=lambda kv: kv[1]["y"])
	(top, tg), (bot, bg) = items
	return tg, bg

tg, bg = stacked("s-stack6.json")
check(tg["x"] == bg["x"], "并进同一列后 x 相同（%d / %d）" % (tg["x"], bg["x"]))
check(bg["y"] - (tg["y"] + tg["height"]) == 6,
	"同列纵向间距 6px（%d）" % (bg["y"] - (tg["y"] + tg["height"])))
check(tg["height"] == 345 and tg["y"] == 12,
	"gaps 6 → 每格高 345、起点 y=12（h=%d y=%d）" % (tg["height"], tg["y"]))

tg, bg = stacked("s-stack20.json")
check(bg["y"] - (tg["y"] + tg["height"]) == 20,
	"同列纵向间距 20px（%d）" % (bg["y"] - (tg["y"] + tg["height"])))
check(tg["height"] == 338, "inner 20 → 每格高 338（%d）" % tg["height"])

tg, bg = stacked("s-stackneg.json")
check(bg["y"] - (tg["y"] + tg["height"]) == 0,
	"负值当 0：纵向间距 0px（%d）" % (bg["y"] - (tg["y"] + tg["height"])))
check(tg["height"] == 348, "负值当 0：每格高 348（%d）" % tg["height"])

# --- 6) 四边单独设置（此时是一个列里两个窗口：视口不滚动，x 就是左边距）
def one_col(name):
	w = load(name)
	check(len(w) == 2, "%s：两个窗口还在" % name)
	items = sorted(w.items(), key=lambda kv: kv[1]["y"])
	return items[0][1], items[1][1]

# 这几步的前置是 `gaps 6`：它把 workspace/window 的 outer 都设成 6，所以
# 「离屏幕边」= 6 + 6 = 12（niri 的语义），列宽 = (1280 - 左 - 右) / 2。
top, bot = one_col("s-side-left.json")
check(top["x"] == 40, "gaps_left 40 → 窗口左边距 40px（%d）" % top["x"])
check(top["width"] == 614,
	"左边距 40 后列宽 = (1280-40-12)/2 = 614（%d）" % top["width"])
check(top["y"] == 12 and top["height"] == 345,
	"上/下没设，还是统一值 12（y=%d h=%d）" % (top["y"], top["height"]))

top, bot = one_col("s-side-top.json")
check(top["y"] == 30, "gaps_top 30 → 窗口上边距 30px（%d）" % top["y"])
check(top["height"] == 336, "上边距 30 后每格高 = (720-30-12-6)/2 = 336（%d）"
	% top["height"])
check(top["x"] == 40, "左边距 40 不受影响（%d）" % top["x"])

top, bot = one_col("s-side-back.json")
check(top["x"] == 12 and top["width"] == 628,
	"gaps_left -1 回到跟随统一值 → 左边距 12、列宽 628（x=%d w=%d）"
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
