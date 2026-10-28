#!/bin/bash
# 补帧不占主循环：窗口滚出视口 / 被藏起来 / 最小化时，补帧必须**受节流约束**，
# 绝不能把主循环烧穿。
#
# 为什么需要这个测试：2026-10-27 用户报「键盘 stutter」，KEYPROBE 抓到按键队列
# 延迟一路累积（lag 每 40ms 涨一次，涨到 2078ms）。根因是补帧循环在
# pump_interval_ms == 0 时退化成「不限速」，窗口滚出视口后每 6ms 补一帧，
# 主循环被占满，按键在队列里排队。
#
# 判据：
#   1) 窗口滚出视口时，补帧日志的**频率**不超过配置的 force_render_hz 的 2 倍
#      （节流真的生效，不是「不限速」）
#   2) 补帧日志里**不许出现「不限速」**三个字
#   3) 补帧期间 IPC 往返（主循环响应）中位数 ≤ 40ms
#   4) 补帧期间按键全部处理完，一个不丢
#   5) 没有 assert / abort
#
# 用法：./test-pump-throttle.sh        （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-pumpthrottle
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
SOCK=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	if [ -n "$VIDOW_PID" ] && kill -0 "$VIDOW_PID" 2>/dev/null; then
		kill "$VIDOW_PID" 2>/dev/null
		for _ in $(seq 1 40); do
			kill -0 "$VIDOW_PID" 2>/dev/null || break
			sleep 0.05
		done
		kill -9 "$VIDOW_PID" 2>/dev/null
	fi
	pkill -f "$RUNDIR" 2>/dev/null
	[ -z "${VIDOW_TEST_KEEP:-}" ] && rm -rf "$RUNDIR"
}
trap cleanup EXIT

rm -rf "$RUNDIR"
mkdir -p "$RUNDIR/run" "$CFGDIR/.config/vidow"
chmod 700 "$RUNDIR/run"

# force_render_hz = 30：补帧最多 30Hz（33ms 一张）。设得低一点，节流失效时
# 一眼就能看出来（不限速会是 4~6ms 一张，差一个数量级）。
cat > "$CFGDIR/.config/vidow/config.lua" <<'LUA'
scale = "1"
xwayland = false
input_injection = true
force_render_hz = 30
-- 和用户配置里那条「全屏游戏」规则同形：一旦窗口看不见就补帧。
-- 用通配 app_id 命中 build/frames（它的 app_id 是 NULL，规则里留空即可）。
vidow.rule { force_render = true }
LUA

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR"
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=1
export WLR_LIBINPUT_NO_DEVICES=1
export VIDOW_LOG=info
export VIDOW_LOG_FILE="$LOG"
unset DISPLAY WAYLAND_DISPLAY VIDOV_SOCKET

./build/vidow >/dev/null 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 80); do
	[ -S "$RUNDIR/run/vidow-wayland-0.sock" ] && break
	sleep 0.05
done
SOCK="$RUNDIR/run/vidow-wayland-0.sock"
if [ ! -S "$SOCK" ]; then
	echo "vidow 没起来"; tail -20 "$LOG" 2>/dev/null; exit 1
fi
sleep 1

# 起一个窗口，然后给它开 force_render（规则路径），再把它停到屏外。
# 停到屏外靠 IPC 把窗口挪走 —— 这里直接用 force_render + 最小化，
# 最小化那条路走的是 view_on_screen() 为假，等价于「滚出视口」。
if [ -x ./build/frames ]; then
	./build/frames 600 >/dev/null 2>&1 &
	FRAMES_PID=$!
	sleep 1
else
	echo "缺 build/frames"; exit 1
fi

mark "开启 force_render（窗口规则路径）"
./build/vidowmsg --socket "$SOCK" set-config force_render_hz 30 >/dev/null 2>&1
# 用 IPC 给所有窗口开 force_render，模拟用户配置里那条规则
WID=$(./build/vidowmsg --socket "$SOCK" windows 2>/dev/null | \
	python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
except Exception:
    print(''); raise SystemExit
ws=d if isinstance(d,list) else d.get('windows',[])
print(ws[0].get('id','') if ws else '')
" 2>/dev/null)
if [ -n "$WID" ]; then
	# force_render 由配置里的规则给（见上面 config.lua），不是 IPC ——
	# IPC 那条会被 apply_view() 重跑规则时冲掉，测不到真实路径。
	# 这里只负责让它「看不见」：最小化，等价于窗口滚出视口。
	./build/vidowmsg --socket "$SOCK" minimize-window >/dev/null 2>&1
	echo "  最小化了窗口 $WID（规则要求补帧）"
	# **踢一脚。** 补帧循环本身要靠「输出出帧」才被调用（vidow_output_render
	# 收尾那条），而最小化之后没有损伤、输出就停了 —— 循环一次都进不去。
	# 真机上这两件事是同一个时间窗里发生的（最小化那一下本身就有一次重绘），
	# 所以补帧能接上；测试里是两次独立 IPC，必须显式踢一脚。
	./build/vidowmsg --socket "$SOCK" unminimize-window >/dev/null 2>&1
	sleep 0.2
	./build/vidowmsg --socket "$SOCK" minimize-window >/dev/null 2>&1
	sleep 0.4
else
	echo "  没找到窗口"
fi
sleep 1
echo "  窗口状态：$(./build/vidowmsg --socket "$SOCK" windows 2>/dev/null | python3 -c "
import json,sys
try: d=json.load(sys.stdin)
except Exception: print('?'); raise SystemExit
ws=d if isinstance(d,list) else d.get('windows',[])
print(', '.join('%s minimized=%s force_render=%s output=%s' % (w.get('id'),w.get('minimized'),w.get('force_render'),w.get('output')) for w in ws))
" 2>/dev/null)"

mark "补帧真的在跑吗"
# **不能靠数日志。** 补帧日志只在 render_pumping 从 false 翻成 true 的那一下打
# 一条，之后一直补也不再打 —— 按条数判会得出「没在补」的错误结论（实测整个
# 测试只有 1 条）。真正的判据是主循环有没有被烧穿，见下面那段 IPC 往返。
if grep -aq '照样补帧回调' "$LOG" 2>/dev/null; then
	pass "补帧确实启动了（日志里有「照样补帧回调」）"
	grep -a '照样补帧回调' "$LOG" | tail -1 | sed 's/^/       /'
else
	fail "补帧根本没启动 —— 这条测试量不到节流"
fi
if grep -aq '不限速' "$LOG" 2>/dev/null; then
	fail "补帧日志里有「不限速」—— 节流退化了（主循环会被烧穿）"
else
	pass "补帧日志里没有「不限速」（节流在生效）"
fi

mark "补帧期间主循环响应（IPC 往返）"
python3 - "$SOCK" > "$RUNDIR/rtt.txt" <<'PYEOF'
import socket, sys, time
sock_path = sys.argv[1]
s = None
out = []
for i in range(40):
    t0 = time.monotonic()
    try:
        if s is None:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.settimeout(2.0)
            s.connect(sock_path)
        s.sendall(b'{"cmd":"send_key","keys":"a"}\n')
        s.recv(65536)
        ok = 1
    except Exception:
        s = None
        ok = 0
    out.append(((time.monotonic() - t0) * 1000.0, ok))
if s is not None:
    s.close()
for dt, ok in out:
    print('%.3f %d' % (dt, ok))
PYEOF

read -r MED OKN < <(python3 - "$RUNDIR/rtt.txt" <<'PYEOF'
import sys
vals, ok = [], 0
for line in open(sys.argv[1]):
    p = line.split()
    if len(p) == 2:
        vals.append(float(p[0])); ok += int(p[1])
if not vals:
    print('0 0'); raise SystemExit
vals.sort()
print('%.1f %d' % (vals[len(vals)//2], ok))
PYEOF
)
echo "  往返中位 ${MED}ms，成功 $OKN/40"

if [ "$OKN" -eq 40 ]; then
	pass "补帧期间 40 个 IPC 请求全部处理完"
else
	fail "补帧期间只处理了 $OKN/40 个"
fi
if python3 -c "import sys;sys.exit(0 if $MED<=40 else 1)"; then
	pass "补帧期间往返中位 ${MED}ms ≤ 40ms（主循环没被占满）"
else
	fail "补帧期间往返中位 ${MED}ms > 40ms（主循环被补帧占满了）"
fi

kill "$FRAMES_PID" 2>/dev/null

mark "崩溃检查"
if grep -aqE 'assert|abort|Assertion' "$LOG" 2>/dev/null; then
	fail "日志里有 assert / abort"
else
	pass "日志里没有 assert / abort"
fi

echo
if [ "$FAILED" -eq 0 ]; then
	echo "== 全部通过 =="
else
	echo "== 有失败项 =="
	[ -n "${VIDOW_TEST_KEEP:-}" ] && echo "运行目录保留在 $RUNDIR"
fi
exit "$FAILED"
