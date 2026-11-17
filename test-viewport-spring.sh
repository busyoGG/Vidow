#!/bin/bash
# 快速连按换焦点时，条带的视口滚动要**速度连续**：改道只换目标，不许把速度清零。
#
# 为什么（用户 2026-11-17）：「窗口焦点快速切换的时候有时候像是被夹住一样过渡
# 时长太长（像是一个一个窗口过渡一样）」。
#
# 根因：视口滚动用的是弹簧曲线（viewport_easing = "spring 0.6 370" ≈ 798ms），
# 而以前每次改道都**重起一条完整时长的曲线**，曲线初速度恒为 0 —— 连按时每一段
# 都从「静止 → 缓慢加速」起步，带子每按一下先顿一下再走，最后那一下还要走满
# 一整条曲线。现在弹簧有速度状态（output->viewport_vel）：改道只换目标，速度
# 接着用（view.c 的 viewport_spring_tick / output_arrange 的改道分支）。
#
# 判据：
#   1) 连按三条 focus-column 时日志里出现「viewport spring 改道」（不是重新起跑）
#   2) 那条改道日志里的速度不为 0（≥200px/s）—— 说明速度被继承了，不是清零
#   3) 最后一条命令之后 ≤650ms 视口静止（修之前实测 720ms，且每按一下顿一次）
#   4) 滚动期间不起位移补间（move tween = 0）：整条带子刚性平移，窗口不各走各的
#   5) 日志无 assert / abort
#
# 用法：./test-viewport-spring.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-vp-spring-test
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
	[ -n "$(printenv VIDOW_TEST_KEEP)" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v python3 >/dev/null || { echo "test-viewport-spring.sh: 需要 python3"; exit 1; }
[ -x ./build/frames ] || { echo "test-viewport-spring.sh: 先 ninja -C build frames"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-viewport-spring.sh: 先 ninja -C build vidowmsg"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
-- 和用户那台一样的曲线：弹簧，798ms 一条（复现「太长」那组参数）
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
window = { ms = 480, easing = "out_back 0.18" }
viewport_easing = "spring 0.6 370"
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 80); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.2
done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "vidow 没起来"; tail -5 "$LOG"; exit 1
fi
for _ in $(seq 1 80); do
	WAYLAND_DISPLAY=$(ls "$RUNDIR/run" 2>/dev/null | grep -m1 '^wayland-[0-9]*$')
	[ -n "$WAYLAND_DISPLAY" ] && break
	sleep 0.1
done
export WAYLAND_DISPLAY
sleep 0.5

mark "0. 开 5 个窗口（各占一列）"
for i in 1 2 3 4 5; do
	./build/frames "w$i" 20 > "$RUNDIR/w$i.log" 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 0.35
done
sleep 1.5

mark "1. 回到最左列，等它停下来"
for _ in 1 2 3 4 5; do
	./build/vidowmsg focus-column -1 >/dev/null 2>&1
	sleep 0.06
done
sleep 2
: > "$RUNDIR/mark.txt"
MARK=$(wc -l < "$LOG")

mark "2. 连按三次 focus-column 1（间隔 90ms），同时采样视口位置"
( while :; do
	printf '%s %s\n' "$(date +%s%N)" "$(./build/vidowmsg state 2>/dev/null |
		python3 -c '
import json,sys
try:
    st=json.load(sys.stdin)
except Exception:
    print(-1); raise SystemExit
for o in st.get("outputs") or []:
    if str(o.get("name","")).startswith("HEADLESS"):
        print(o.get("viewport_x", -1)); raise SystemExit
print(-1)')"
	sleep 0.025
done ) > "$RUNDIR/vp.txt" 2>/dev/null &
SPID=$!
sleep 0.35
for _ in 1 2 3; do
	echo "CMD $(date +%s%N)" >> "$RUNDIR/cmds.txt"
	./build/vidowmsg focus-column 1 >/dev/null 2>&1
	sleep 0.09
done
sleep 1.5
kill $SPID 2>/dev/null

python3 - "$LOG" "$MARK" "$RUNDIR/vp.txt" "$RUNDIR/cmds.txt" <<'PY'
import re, sys

log, mark, vp_path, cmd_path = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
text = open(log, encoding="utf-8", errors="replace").read().splitlines()
tail = text[mark:]

starts = [l for l in tail if "viewport spring 起跑" in l]
redir = [l for l in tail if "viewport spring 改道" in l]
fail = 0
if redir:
    print("  PASS  连按时走的是「改道」（%d 次），没有重新起跑" % len(redir))
else:
    print("  FAIL  没看到「viewport spring 改道」：连按把弹簧重起了"
          "（起跑 %d 次）" % len(starts))
    fail = 1
# 第一条命令当然要「起跑」（此前没有动画在跑），判据是**之后不许再起**。
if len(starts) > 1:
    print("  FAIL  连按期间重起了 %d 次弹簧（只该有第一条命令那 1 次，"
          "后面的都要走改道）" % len(starts))
    fail = 1
else:
    print("  PASS  只有第一条命令起了弹簧（%d 次），后面的都走改道"
          % len(starts))

vels = []
for l in redir:
    m = re.search(r"速度\s*(-?[\d.]+)px/s", l)
    if m:
        vels.append(abs(float(m.group(1))))
if vels and max(vels) >= 200.0:
    print("  PASS  改道时速度被继承（最大 %.0fpx/s，不是清零）" % max(vels))
else:
    print("  FAIL  改道时速度还是 0（%s）—— 速度没接上" % (vels or "没读到"))
    fail = 1

rows = []
for line in open(vp_path):
    p = line.split()
    if len(p) != 2:
        continue
    try:
        rows.append((int(p[0]), float(p[1])))
    except ValueError:
        pass
cmds = []
try:
    for line in open(cmd_path):
        p = line.split()
        if len(p) >= 2:
            cmds.append(int(p[1]))
except OSError:
    pass
if cmds and rows:
    rows = [(t, v) for (t, v) in rows if t >= cmds[0]]
    settle = None
    for i, (t, v) in enumerate(rows):
        tail_v = [r[1] for r in rows[i:]]
        if max(tail_v) - min(tail_v) <= 0.5:
            settle = t
            break
    if settle is None:
        print("  FAIL  1.5s 内视口没静止（还在动）")
        fail = 1
    else:
        ms = (settle - cmds[-1]) / 1e6
        if ms <= 650.0:
            print("  PASS  最后一条命令之后 %.0fms 静止（判据 ≤650ms）" % ms)
        else:
            print("  FAIL  最后一条命令之后 %.0fms 才静止（判据 ≤650ms）" % ms)
            fail = 1
else:
    print("  FAIL  没采到视口位置 / 命令时刻")
    fail = 1

moves = sum(1 for l in tail if "move tween" in l)
if moves == 0:
    print("  PASS  滚动期间没起位移补间（整条带子刚性平移）")
else:
    print("  FAIL  滚动期间起了 %d 条位移补间（窗口各走各的）" % moves)
    fail = 1

sys.exit(fail)
PY
[ $? -eq 0 ] || FAILED=1

mark "3. 日志干净"
grep -aqE 'assert|abort|Assertion' "$LOG" && fail "日志里有 assert/abort" \
	|| pass "日志里没有 assert / abort"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
