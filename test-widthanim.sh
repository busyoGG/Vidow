#!/bin/bash
# 列宽过渡：cycle-column-width / 列最大化 / IPC 改宽时，列宽按 column_width_ms 补间
# 过去；拖动改宽（Mod+右键拖）不补间，必须跟手（那条由 ./test-tile-resize.sh 兜）。
#
# 判据：
#   1) 默认（column_width_ms = 200）：改宽时列宽**逐帧变化**（≥3 个中间值、单调不
#      回头），落在目标像素上；右边那条列跟着让位（offset = 左边宽 + gap，每一帧都
#      对得上），而且**不给它起位移补间**（过渡期间只有 column-width 一个动画）
#   2) 目标比例（width_fraction）当场就是新值，不跟着补间走
#   3) column_width_ms = 0：直接到位，一个中间值都没有
#   4) column_width_ms = 500：过渡真的按这个时长走（够长）
#   5) 列最大化 / 还原也走同一条路（同样的中间值）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-widthanim-test
CLIENT_PIDS=""
CFGDIR="$RUNDIR/home"
CFGDIRV="$CFGDIR/.config/vidow"
LOG="$RUNDIR/log"
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

trap 'exit 143' TERM INT
trap cleanup EXIT

our_vidow_pids() {
	local p
	for p in /proc/[0-9]*; do
		[ -r "$p/cmdline" ] || continue
		grep -aq "build/vidow" "$p/cmdline" 2>/dev/null || continue
		[ -r "$p/environ" ] || continue
		grep -aqz "$RUNDIR" "$p/environ" 2>/dev/null || continue
		echo "${p#/proc/}"
	done
}

cleanup() {
	local pid
	our_vidow_pids | while read -r pid; do kill -KILL "$pid" 2>/dev/null; done
	for pid in ${CLIENT_PIDS:-}; do kill -KILL "$pid" 2>/dev/null; done
}

rm -rf "$RUNDIR"
mkdir -p "$CFGDIRV" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=info
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=pixman
export DISPLAY= WAYLAND_DISPLAY=

write_config() { # <额外配置行>
	cat > "$CFGDIRV/config.lua" <<CFG
scale = "1"
gaps = 10
window_open_ms = 0
$1
CFG
}

start_vidow() {
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	for _ in $(seq 1 60); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		fail "vidow 没起来"
		grep -a "ERROR" "$LOG" | head -5
		exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	SOCK="$XDG_RUNTIME_DIR/vidow-$WAYLAND_DISPLAY.sock"
}

stop_vidow() {
	local pid
	our_vidow_pids | while read -r pid; do kill -TERM "$pid" 2>/dev/null; done
	for _ in $(seq 1 40); do
		[ -z "$(our_vidow_pids)" ] && return 0
		sleep 0.2
	done
	return 1
}

VM="./build/vidowmsg"
openwin() { # openwin <标题>
	python3 tools/pattern.py "$1" >/dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 1.5
}

# ---------------------------------------------------------------- 采样器
# 一条长连接：先采一次底、发动作、然后每 4ms 采一次 columns（顺带采 anims 记 tag）。
cat > "$RUNDIR/poll.py" <<'PY'
import json, socket, sys, time

sock_path, action, dur_ms, step_ms = (sys.argv[1], sys.argv[2],
                                     float(sys.argv[3]), float(sys.argv[4]))
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
buf = b""


def call(obj):
    global buf
    s.sendall((json.dumps(obj) + "\n").encode())
    while b"\n" not in buf:
        buf += s.recv(1 << 20)
    line, _, buf = buf.partition(b"\n")
    return json.loads(line)


def cols():
    return [(c["width"], c["offset"], round(c["width_fraction"], 6),
             c["maximized"]) for c in call({"cmd": "columns"})["result"]]


def anims():
    return [a["tag"] for a in call({"cmd": "anims"})["result"]["running"] if a]


before = cols()
t0 = time.monotonic()
reply = call(json.loads(action))
samples = []
tags = []
while (time.monotonic() - t0) * 1000 < dur_ms:
    ms = (time.monotonic() - t0) * 1000
    samples.append([round(ms, 1), cols()])
    for t in anims():
        tags.append([round(ms, 1), t])
    time.sleep(step_ms / 1000.0)
after = cols()
print(json.dumps({"before": before, "after": after, "samples": samples,
                  "tags": tags, "reply": reply}))
PY

# 采样 + 判据。check.py <trace.json> <标签> <animated|instant> <毫秒上界>
cat > "$RUNDIR/check.py" <<'PY'
import json, sys

trace = json.load(open(sys.argv[1]))
label, kind, budget = sys.argv[2], sys.argv[3], float(sys.argv[4])
failed = 0


def pass_(msg):
    print("  PASS  %s" % msg)


def fail_(msg):
    global failed
    failed = 1
    print("  FAIL  %s" % msg)


before, after = trace["before"], trace["after"]
samples = trace["samples"]
w0, w1 = before[0][0], after[0][0]
widths = [s[1][0][0] for s in samples]
fracs = [s[1][0][2] for s in samples]
lo, hi = min(w0, w1), max(w0, w1)
mid = sorted(set(w for w in widths if lo + 1 < w < hi - 1))

if kind == "animated":
    if len(mid) >= 3:
        pass_("%s：过渡里量到 %d 个中间宽度（%s）" % (
            label, len(mid), " → ".join(str(w) for w in mid[:8])))
    else:
        fail_("%s：只量到 %d 个中间宽度（%s）—— 没有过渡？" % (
            label, len(mid), mid))
    want = sorted(widths, reverse=w1 < w0)
    if widths == want:
        pass_("%s：宽度单调不回头" % label)
    else:
        fail_("%s：宽度回头了：%s" % (label, widths))
    # 到位时刻：第一次量到终点值的那一次采样
    t_done = None
    for ms, cs in samples:
        if cs[0][0] == w1:
            t_done = ms
            break
    if t_done is None:
        fail_("%s：整段都没量到终点宽度 %d" % (label, w1))
    elif t_done > budget:
        fail_("%s：用了 %.0fms 才到位（上限 %.0fms）" % (label, t_done, budget))
    else:
        pass_("%s：%.0fms 到位（上限 %.0fms）" % (label, t_done, budget))
else:
    if mid:
        fail_("%s：本该直接到位，却量到中间宽度 %s" % (label, mid))
    else:
        pass_("%s：直接到位，一个中间值都没有" % label)
    t_done = None
    for ms, cs in samples:
        if cs[0][0] == w1:
            t_done = ms
            break
    if t_done is not None and t_done <= budget:
        pass_("%s：%.0fms 内就到位（上限 %.0fms）" % (label, t_done, budget))
    else:
        fail_("%s：没在 %.0fms 内到位（量到 %s）" % (label, budget, t_done))

# 终点宽度
if w1 != w0 and widths and widths[-1] == w1:
    pass_("%s：最后停在目标像素 %d" % (label, w1))
else:
    fail_("%s：终点不对（起点 %d、目标 %d、末尾 %s）" % (
        label, w0, w1, widths[-1] if widths else None))
# 目标比例当场就是新值（不跟着补间走）
if fracs and fracs[0] == after[0][2]:
    pass_("%s：width_fraction 当场就是新值 %s" % (label, after[0][2]))
else:
    fail_("%s：width_fraction 没当场更新（%s vs %s）" % (
        label, fracs[0] if fracs else None, after[0][2]))
# 右边那条列：每一帧 offset 都要等于左边宽 + gap
bad = [s for s in samples if s[1][1][1] != s[1][0][0] + 10]
if len(before) > 1 and not bad:
    pass_("%s：右边那条列每一帧都贴住左边（offset = 宽 + gap 10）" % label)
elif len(before) > 1:
    fail_("%s：%d 帧里右边那条列没贴住（第一帧 %s）" % (
        label, len(bad), bad[0][1][1:]))
# 过渡**期间**不许给隔壁列起位移补间（只有「有过渡」这一档才要求：直接到位那一
# 档隔壁列该照常滑过去，那是布局重排本来的动画）。窗口取「第一次量到终点值」之前
# —— 收尾那一帧的 1px 取整会照常起一条补间，那是布局重排自己的事，不算「吊在
# 过渡后面」。
t_done = None
for ms, cs in samples:
    if cs[0][0] == w1:
        t_done = ms
        break
win = t_done if t_done is not None else samples[-1][0]
moves = [t for t in trace["tags"] if t[1] == "move" and t[0] < win]
tags = sorted(set(t[1] for t in trace["tags"]))
if kind == "animated":
    if moves:
        fail_("%s：过渡期间起了位移补间（move 出现于 %sms，过渡在 %sms 结束）"
              "—— 隔壁列会吊在后面" % (label, moves[0][0], win))
    else:
        pass_("%s：过渡期间没有位移补间（tags=%s）" % (label, tags))
else:
    pass_("%s：tags=%s（直接到位那一档隔壁列照常滑）" % (label, tags))

sys.exit(failed)
PY

run_check() { # run_check <trace> <标签> <animated|instant> <毫秒上界>
	local out
	out=$(python3 "$RUNDIR/check.py" "$@" 2>&1)
	printf '%s\n' "$out"
	case "$out" in *"  FAIL"*) FAILED=1 ;; esac
}

trace() { # trace <动作 JSON> <毫秒>
	python3 "$RUNDIR/poll.py" "$SOCK" "$1" "${2:-700}" 4 > "$RUNDIR/trace.json"
}

# ---------------------------------------------------- 1) 默认 200ms：有过渡
mark "1) 默认 column_width_ms = 200：改宽是过渡过去的"
write_config ""
start_vidow
openwin w-left
openwin w-right
NCOL=$($VM columns 2>/dev/null | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')
[ "$NCOL" = "2" ] && pass "两个窗口 = 两条列" || fail "列数不对：$NCOL"
# 焦点挪到左边那条列：改宽的就是它，右边那条列必须跟着让位（这才是「隔壁列」
# 那条路径）
$VM focus-column -1 >/dev/null 2>&1
sleep 0.3
trace '{"cmd":"cycle-column-width"}' 700
run_check "$RUNDIR/trace.json" "默认 200ms" animated 450
grep -aq "column_width=200ms" "$LOG" \
	&& pass "配置日志里报了 column_width=200ms" \
	|| fail "配置日志里没看到 column_width=200ms"

# ---------------------------------------------------- 2) 0ms：直接到位
mark "2) column_width_ms = 0：直接到位"
stop_vidow
write_config "column_width_ms = 0"
start_vidow
openwin w-left
openwin w-right
$VM focus-column -1 >/dev/null 2>&1
sleep 0.3
trace '{"cmd":"cycle-column-width"}' 400
run_check "$RUNDIR/trace.json" "0ms" instant 120

# ---------------------------------------------------- 3) 500ms：时长真的听配置
mark "3) column_width_ms = 500：过渡按这个时长走"
stop_vidow
write_config "column_width_ms = 500"
start_vidow
openwin w-left
openwin w-right
$VM focus-column -1 >/dev/null 2>&1
sleep 0.3
trace '{"cmd":"cycle-column-width"}' 900
run_check "$RUNDIR/trace.json" "500ms" animated 700
# 反证：够长 —— 到位时刻不该早于 400ms
T=$(python3 - "$RUNDIR/trace.json" <<'PY'
import json, sys
t = json.load(open(sys.argv[1]))
w1 = t["after"][0][0]
for ms, cs in t["samples"]:
    if cs[0][0] == w1:
        print(int(ms)); break
PY
)
if [ -n "$T" ] && [ "$T" -ge 400 ]; then
	pass "到位用了 ${T}ms（≥400ms，说明 500ms 这个配置真的生效了）"
else
	fail "500ms 配了但 ${T:-?}ms 就到了 —— 时长没听配置"
fi
# ---------------------------------------------------- 4) 列最大化也走这条路
mark "4) 列最大化 / 还原：同一条路（也是过渡）"
trace '{"cmd":"toggle-maximize-column"}' 900
run_check "$RUNDIR/trace.json" "列最大化" animated 700
MAX=$($VM columns 2>/dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["maximized"])')
[ "$MAX" = "True" ] && pass "最大化状态置上了" || fail "最大化状态不对：$MAX"
trace '{"cmd":"toggle-maximize-column"}' 900
run_check "$RUNDIR/trace.json" "列还原" animated 700

# ---------------------------------------------------- 5) 全程没崩
mark "5) 收尾"
stop_vidow
if grep -aq "vidow 崩了\|Segmentation fault\|assert" "$LOG"; then
	fail "日志里有崩溃痕迹"
else
	pass "全程没崩"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
