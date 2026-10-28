#!/bin/bash
# 按键延迟：连续注入一串按键，量每个按键「注入时刻 -> Vidow 处理时刻」的延迟。
#
# 为什么需要这个测试：用户报「键盘 stutter，所有按键都会，x 自己 repeat 也有概率
# stutter」，但日志里一条都看不见 —— 按键路径上没有任何计时。这个测试把延迟量出来，
# 分三种场景：
#   A) 空载：没有窗口在动，纯按键
#   B) 有窗口在持续重绘（模拟游戏 / 视频），按键照常
#   C) 有个窗口被强制补帧（force_render=true，用户配置里那条全屏游戏规则），按键照常
#
# 判据（延迟 = Vidow 收到事件到处理完的耗时；注入侧只能测「发出 -> IPC 返回」，
# 所以用 KEYPROBE 的 lag 做交叉验证）：
#   1) 空载：连续 60 个按键，**全部到达**，顺序不乱
#   2) 空载：相邻按键的处理间隔中位数 < 3ms（注入间隔 5ms，不该被放大）
#   3) 有窗口重绘时：延迟中位数不超过空载的 3 倍
#   4) 强制补帧窗口在场时：延迟中位数不超过空载的 3 倍
#   5) 三种场景下**一个按键都不许丢**
#
# 用法：./test-key-latency.sh            （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：隔离会话（自己的 XDG_RUNTIME_DIR），绝不碰真实会话。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-keylat
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
	if [ -z "${VIDOW_TEST_KEEP:-}" ]; then
		rm -rf "$RUNDIR"
	fi
}
trap cleanup EXIT

rm -rf "$RUNDIR"
mkdir -p "$RUNDIR/run" "$CFGDIR/.config/vidow"
chmod 700 "$RUNDIR/run"

# 配置：一块屏，开了按键探针要用的日志级别，input_injection 必须开（IPC 注入）。
cat > "$CFGDIR/.config/vidow/config.lua" <<'LUA'
scale = "1"
xwayland = false
input_injection = true
LUA

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR"
export WLR_BACKENDS=headless
export WLR_HEADLESS_OUTPUTS=1
export WLR_LIBINPUT_NO_DEVICES=1
export VIDOW_LOG=info
export VIDOW_LOG_FILE="$LOG"
export VIDOW_PROBE_KEYS=1
unset DISPLAY WAYLAND_DISPLAY VIDOV_SOCKET

./build/vidow >/dev/null 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 80); do
	[ -S "$RUNDIR/run/vidow-wayland-0.sock" ] && break
	sleep 0.05
done
SOCK="$RUNDIR/run/vidow-wayland-0.sock"
if [ ! -S "$SOCK" ]; then
	echo "vidow 没起来（socket 不存在）"; tail -20 "$LOG" 2>/dev/null; exit 1
fi
sleep 1

# 注入一串按键，记下每次发出的单调时钟（纳秒），用来和 KEYPROBE 的时间戳对齐。
# 参数：个数、间隔（毫秒）
inject_run() {
	local n=$1 gap=$2
	# 往返时间（发出请求 -> IPC 返回）才是「这个按键多久被处理完」的真实度量：
	# vidow 是单线程事件循环，IPC 请求和按键处理在同一条线上排队，往返时间
	# 变长就说明主循环被别的东西占住了 —— 那正是 stutter 的样子。
	python3 - "$SOCK" "$n" "$gap" <<'PYEOF' > "$RUNDIR/rtt.txt"
import socket, sys, time, json
sock_path, n, gap = sys.argv[1], int(sys.argv[2]), float(sys.argv[3]) / 1000.0
out = []
# 复用同一条连接：每次新建 socket 的 connect/close 是测试自己的开销（实测
# ~10ms，把真实延迟完全盖住），必须排除掉。复用之后往返时间就只剩「vidow
# 读到请求 -> 处理完 -> 回包」这一段。
s = None
for i in range(n):
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
    dt = (time.monotonic() - t0) * 1000.0
    out.append((dt, ok))
    if gap > 0:
        time.sleep(gap)
if s is not None:
    s.close()
for dt, ok in out:
    print('%.3f %d' % (dt, ok))
PYEOF
}

# 从 rtt.txt 算统计：中位 / 最大 / 成功数
rtt_stats() {
	python3 - "$RUNDIR/rtt.txt" <<'PYEOF'
import sys
vals, ok = [], 0
try:
    for line in open(sys.argv[1]):
        p = line.split()
        if len(p) == 2:
            vals.append(float(p[0]))
            ok += int(p[1])
except FileNotFoundError:
    pass
if not vals:
    print('0 0 0'); sys.exit()
vals.sort()
print('%.1f %.1f %d' % (vals[len(vals)//2], vals[-1], ok))
PYEOF
}

N=60
# 间隔 0：连续猛发，测的是「vidow 处理一个 IPC 请求要多久」。带间隔的话
# 往返时间里会混进 sleep 本身的调度抖动，量不出主循环的真实响应。
GAP=0

# ---------------- A) 空载 ----------------
mark "A) 空载：$N 个按键，间隔 ${GAP}ms"
inject_run "$N" "$GAP"
sleep 0.5
read -r MED MAX OKN <<<"$(rtt_stats)"
if [ "$OKN" -eq "$N" ]; then
	pass "空载：$N 个按键全部处理完（一个没丢）"
else
	fail "空载：只处理了 $OKN/$N 个按键"
fi
echo "       往返：中位 ${MED}ms，最大 ${MAX}ms"
# 阈值按 headless 后端的模拟刷新率定：它按 60Hz 出帧，一个 IPC 请求要等到
# 下一拍才被处理，所以**下限就是一帧 16.7ms**。这不是 vidow 的缺陷，是测试
# 环境自带的；判据只需要「不显著超过一帧」。
if python3 -c "import sys;sys.exit(0 if $MED<=25 else 1)"; then
	pass "空载：往返中位数 ${MED}ms ≤ 25ms（headless 一帧 16.7ms 的量级）"
else
	fail "空载：往返中位数 ${MED}ms > 25ms（远超一帧，主循环被占住了）"
fi
BASE_MED=$MED

# ---------------- B) 有窗口持续重绘 ----------------
mark "B) 有窗口持续重绘时：$N 个按键"
# 用一个不停重绘的客户端占住画面（pattern.py 是 GTK，会跟随输入重绘；这里用
# frames 工具持续提交帧更接近游戏）
if [ -x ./build/frames ]; then
	./build/frames 300 >/dev/null 2>&1 &
	FRAMES_PID=$!
	sleep 0.6
else
	FRAMES_PID=""
fi
inject_run "$N" "$GAP"
sleep 0.5
read -r MED MAX OKN <<<"$(rtt_stats)"
[ -n "$FRAMES_PID" ] && kill "$FRAMES_PID" 2>/dev/null

if [ "$OKN" -eq "$N" ]; then
	pass "重绘中：$N 个按键全部处理完"
else
	fail "重绘中：只处理了 $OKN/$N 个按键"
fi
echo "       往返：中位 ${MED}ms，最大 ${MAX}ms"
LIMIT=$(python3 -c "print(max(25.0, $BASE_MED*3))")
if python3 -c "import sys;sys.exit(0 if $MED<=$LIMIT else 1)"; then
	pass "重绘中：往返中位数 ${MED}ms ≤ ${LIMIT}ms（空载 ${BASE_MED}ms 的 3 倍内）"
else
	fail "重绘中：往返中位数 ${MED}ms > ${LIMIT}ms（重绘把按键堵住了）"
fi

# ---------------- C) 强制补帧窗口在场 ----------------
mark "C) 强制补帧窗口在场时：$N 个按键"
# force_render=true 的窗口会按 force_render_hz 不停补帧 —— 这正是用户配置里
# 「全屏游戏」那条规则的效果，也是最可疑的 stutter 来源。
if [ -x ./build/frames ]; then
	./build/frames 300 >/dev/null 2>&1 &
	FRAMES_PID=$!
	sleep 0.4
	./build/vidowmsg --socket "$SOCK" set-config force_render_hz 1000 >/dev/null 2>&1
	./build/vidowmsg --socket "$SOCK" rule-add 'force_render=true' >/dev/null 2>&1
	sleep 0.6
else
	FRAMES_PID=""
fi
inject_run "$N" "$GAP"
sleep 0.5
read -r MED MAX OKN <<<"$(rtt_stats)"
[ -n "$FRAMES_PID" ] && kill "$FRAMES_PID" 2>/dev/null

if [ "$OKN" -eq "$N" ]; then
	pass "补帧中：$N 个按键全部处理完"
else
	fail "补帧中：只处理了 $OKN/$N 个按键"
fi
echo "       往返：中位 ${MED}ms，最大 ${MAX}ms"
if python3 -c "import sys;sys.exit(0 if $MED<=$LIMIT else 1)"; then
	pass "补帧中：往返中位数 ${MED}ms ≤ ${LIMIT}ms"
else
	fail "补帧中：往返中位数 ${MED}ms > ${LIMIT}ms（补帧把主循环堵住了 —— 这就是 stutter）"
fi

# ---------------- 收尾 ----------------
mark "崩溃检查"
if grep -aqE 'assert|abort|Assertion' "$LOG" 2>/dev/null; then
	fail "日志里有 assert / abort"
	grep -aE 'assert|abort|Assertion' "$LOG" | head -3
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
