#!/bin/bash
# 「后端不给帧事件的时候，拖窗口照样跟手」。
#
# 场景：headless vidow + 一个浮窗，用测试钩子 `VIDOW_TEST_STALL_OUTPUT=HEADLESS-1`
# 把那块屏的**帧事件**按住（模拟嵌套在别的合成器里、窗口被盖住、或者那块屏的
# page-flip 不回来）。然后拖这个窗口。
#
# 为什么会有这条测试：浮动拖拽的落位是**推迟到帧里**做的（见 src/float.c 的
# arrange_dirty），帧一停它就一点都不动。而保帧看门狗（src/output.c 的
# anim_watchdog）以前只在「动画 / 果冻在跑」时武装，拖动不在其中 —— 于是帧事件
# 一断，拖动就冻在那儿（实测嵌套在 Hyprland 里拖一次：2.9 秒里只出了 1 帧，
# 帧追踪报「vidow 主动要过 597 帧」）。
#
# 判据：
#   1) vidow 起来了、拖拽真的起来了（grab move 日志）
#   2) 掐着帧事件拖：窗口**跟着动**（位移 ≈ 拖动量）
#   3) 拖动期间真的出过帧（统计行里有帧数）—— 帧是看门狗推出来的
#   4) vidow 还活着
#
# 用法：./test-drag-stall.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-dragstall-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENTS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENTS; do kill "$p" 2>/dev/null; done
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in *pattern.py*|*wall.py*|*spin.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
blur_enabled = false
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
jelly = true
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=info VIDOW_RENDER_STATS=1
export VIDOW_TEST_STALL_OUTPUT=HEADLESS-1
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

mark "掐断帧事件之后，拖浮动窗口还跟不跟手"

BIN=${VIDOW_BIN:-./build/vidow}
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
sleep 0.5
exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.25}"; }

setsid nohup python3 ./tools/pattern.py w1 > "$RUNDIR/w1.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 1.6

# 看的是 **geometry**（真正摆在屏幕上的那个框），不是 float_geometry（模型）：
# 拖动只改模型，落位是帧里做的 —— 帧停了，geometry 就一点都不动。
field() { ./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    if w["title"] == "w1":
        g = w["geometry"]
        print(g["x"], g["y"]); break' 2>/dev/null; }

for _ in $(seq 1 12); do
	./build/vidowmsg toggle-floating --title w1 >/dev/null 2>&1
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
sys.exit(0 if any(w["title"] == "w1" and w["floating"] for w in json.load(sys.stdin)) else 1)' && break
	sleep 0.3
done
./build/vidowmsg set-window-size --title w1 600 400 >/dev/null 2>&1
sleep 0.3
./build/vidowmsg set-window-position --title w1 100 100 >/dev/null 2>&1
sleep 0.8

if grep -aq "vidow ready" "$LOG"; then
	pass "vidow 起来了（帧事件已被掐断：VIDOW_TEST_STALL_OUTPUT=HEADLESS-1）"
else
	fail "vidow 没起来"
fi

P0=$(field); set -- $P0
if [ $# -eq 2 ]; then
	pass "浮窗 w1 的起点 $P0"
else
	fail "拿不到浮窗位置（$P0）"
fi
X0=${1:-0}; Y0=${2:-0}

# 从窗口中间起拖，往右下拖 240x120
OFF=$(wc -c < "$LOG")
act "pointer-motion $((X0 + 300)) $((Y0 + 200))"
act "pointer-button left press mod" 0.3
for i in 20 60 120 180 240; do
	act "pointer-motion $((X0 + 300 + i)) $((Y0 + 200 + i / 2))" 0.15
done
# 还没松手：这时候的位置才是「跟不跟手」—— 松手时 float.c 会把欠着的那次
# 重排补上（所以只看松手后的位置，帧一帧都没出也照样是对的）。
PMID=$(field); set -- $PMID
XMID=${1:-0}; YMID=${2:-0}
DMIDX=$((XMID - X0)); DMIDY=$((YMID - Y0))
act "pointer-button left release mod" 0.4

if grep -aq "grab move: w1" "$LOG"; then
	pass "拖拽真的起来了（grab move: w1）"
else
	fail "没起来拖拽（没有 grab move 日志）"
fi

if [ "$DMIDX" -ge 200 ] && [ "$DMIDY" -ge 100 ]; then
	pass "还按着的时候就跟着走了：$P0 → $PMID（+$DMIDX,+$DMIDY）"
else
	fail "按着的时候窗口没动：$P0 → $PMID（+$DMIDX,+$DMIDY，应该 ≈ +240,+120）—— 帧停了，落位就欠着不动"
fi

P1=$(field); set -- $P1
X1=${1:-0}; Y1=${2:-0}
DX=$((X1 - X0)); DY=$((Y1 - Y0))
if [ "$DX" -ge 200 ] && [ "$DY" -ge 100 ]; then
	pass "帧事件断着也跟手：往右下拖 240x120 → 窗口从 $P0 到 $P1（+$DX,+$DY）"
else
	fail "窗口没跟着走：$P0 → $P1（+$DX,+$DY，应该 ≈ +240,+120）—— 帧停了就一点都不动"
fi

# 拖动期间真的出过帧吗（看门狗推的）
tail -c +$((OFF + 1)) "$LOG" | grep -a "渲染统计" > "$RUNDIR/stats.txt"
FRAMES=$(python3 - "$RUNDIR/stats.txt" <<'PY'
import re, sys
n = 0
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'渲染统计: (\d+) 帧', line)
    if m:
        n += int(m.group(1))
print(n)
PY
)
if [ "$FRAMES" -ge 5 ] 2>/dev/null; then
	pass "拖动期间出了 $FRAMES 帧（帧事件被掐断，这些是保帧看门狗推的）"
else
	fail "拖动期间只出了 $FRAMES 帧 —— 帧事件一断就没人推帧了"
fi

# ------------------------------------------------------- 2) 平铺拖拽（Mod+左键）

mark "掐断帧事件之后，拖**平铺**窗口（Mod+左键）也得出帧"

./build/vidowmsg toggle-floating --title w1 >/dev/null 2>&1
sleep 0.8
T0=$(field); set -- $T0
TX=${1:-0}; TY=${2:-0}
OFF2=$(wc -c < "$LOG")
act "pointer-motion $((TX + 200)) $((TY + 150))"
act "pointer-button left press mod" 0.3
for i in 20 60 120 180; do
	act "pointer-motion $((TX + 200 + i)) $((TY + 150))" 0.15
done
act "pointer-button left release mod" 0.4

if grep -aq "grab tile: w1" "$LOG"; then
	pass "平铺拖拽起来了（grab tile: w1）"
else
	fail "没起来平铺拖拽（没有 grab tile 日志）"
fi
tail -c +$((OFF2 + 1)) "$LOG" | grep -a "渲染统计" > "$RUNDIR/stats2.txt"
FRAMES2=$(python3 - "$RUNDIR/stats2.txt" <<'PY'
import re, sys
n = 0
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'渲染统计: (\d+) 帧', line)
    if m:
        n += int(m.group(1))
print(n)
PY
)
if [ "$FRAMES2" -ge 5 ] 2>/dev/null; then
	pass "平铺拖拽期间出了 $FRAMES2 帧（帧事件被掐断，靠保帧看门狗推）"
else
	fail "平铺拖拽期间只出了 $FRAMES2 帧 —— 帧事件一断就没人推帧了"
fi

if pgrep -x vidow >/dev/null 2>&1; then
	pass "vidow 还活着"
else
	fail "vidow 死了（$(tail -3 "$LOG" | tr '\n' '|')）"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-drag-stall.sh: OK"
else
	echo "test-drag-stall.sh: 有失败"
fi
exit "$FAILED"
