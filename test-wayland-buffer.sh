#!/bin/bash
# Wayland 客户端「发送缓冲区满就被踢」的回归测试。
#
# 背景（2026-11-02）：Element / kitty 会莫名其妙被 vidow 断连，客户端日志最后
# 一行是 "Fatal Wayland communication error: 连接被对方重置 / 断开的管道"。
# libwayland 每条连接有一条**发送**缓冲区，默认上限只有 4096 字节
# （wayland-private.h 的 WL_BUFFER_DEFAULT_MAX_SIZE）。客户端一时不读 socket
# （主线程忙、GC、加载页面），合成器这边攒过 4096 字节，下一条事件就失败：
#     [wayland] Data too big for buffer (4088 + 20 > 4096).
# 然后 libwayland 静默销毁整条连接 —— 客户端看到 ECONNRESET / EPIPE。
# Hyprland 和 niri 都把默认值抬到 1MB（见 src/main.c 里 wl_display_create
# 后面那段）；vidow 以前没设，于是就是 4096。
#
# 判据：
#   1) 客户端把自己的接收缓冲压到 4KB（STALLTEST_RCVBUF）后睡住不读，
#      合成器灌 3000 条指事件 —— 连接不能断，日志不能出现
#      "Data too big for buffer"，客户端退出码必须是 0；
#   2) 探针（src/waylandfd.c）真的会说话：把客户端 SIGKILL 掉之后，
#      vidow 下一次 sendmsg 必然失败，日志里要有一行带 errno / 对端 pid /
#      字节数 / fd 指向的 ERROR。
#
# 用法：./test-wayland-buffer.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-wayland-buffer-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1" >&2; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$CLIENT_PID" ] && kill -9 "$CLIENT_PID" 2>/dev/null
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/vidowmsg ] || { echo "先构建（ninja -C build）"; exit 1; }
[ -x ./build/stalltest ] || { echo "先构建（ninja -C build）"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
printf 'scale = "1"\nxwayland = false\n' > "$CFGDIR/.config/vidow/config.lua"

export XDG_RUNTIME_DIR="$RUNDIR/run" HOME="$CFGDIR" VIDOW_LOG=info
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export DISPLAY= WAYLAND_DISPLAY=

msg() { ./build/vidowmsg "$@" 2>&1; }

geo() { # "x y w h"（找不到就是 -1 -1 0 0）
	msg windows | python3 -c '
import json,sys
try:
    ws=json.load(sys.stdin)
except Exception:
    print("-1 -1 0 0"); raise SystemExit
if isinstance(ws,dict): ws=ws.get("windows",[])
for w in ws:
    if (w.get("title") or "")=="stalltest":
        g=w.get("geometry") or {}
        print(g.get("x"),g.get("y"),g.get("width"),g.get("height"))
        break
else:
    print("-1 -1 0 0")
'
}

start_vidow() {
	setsid "${VIDOW:-./build/vidow}" >"$LOG" 2>&1 &
	VIDOW_PGID=$!
	for _ in $(seq 1 100); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.2
	done
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	[ -n "$WAYLAND_DISPLAY" ] || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
}

start_client() { # $1 = 目录后缀，$2 = 睡多少毫秒
	STALLTEST_RCVBUF=4096 ./build/stalltest --stall "$2" \
		>"$RUNDIR/client-$1.log" 2>&1 &
	CLIENT_PID=$!
	for _ in $(seq 1 80); do
		[ "$(geo)" != "-1 -1 0 0" ] && break
		sleep 0.2
	done
	sleep 0.2
}

stop_client() {
	if [ -n "$CLIENT_PID" ]; then
		kill -9 "$CLIENT_PID" 2>/dev/null
		wait "$CLIENT_PID" 2>/dev/null
		CLIENT_PID=""
	fi
}

flood() { # 在窗口里灌 $1 条指针移动
	# `vidowmsg windows` 报的是**列**的几何，stalltest 的 surface 只有 400x300、
	# 水平居中、贴着列顶。所以取列的水平中点 + 列顶往下 20px —— 一定落在
	# surface 里（列高 708、surface 高 300，取列垂直中点会掉到窗口外面去，
	# 客户端一条 motion 都收不到）。
	local x y w h i
	read -r x y w h <<<"$(geo)"
	for i in $(seq 1 "$1"); do
		msg move-cursor $((x + w / 2 + i % 100 - 50)) $((y + 20)) \
			>/dev/null 2>&1
	done
}

start_vidow
mark "1. 客户端睡住不读，合成器灌 3000 条指针事件"
start_client stall 15000
read -r X Y W H <<<"$(geo)"
echo "  窗口在 $X,$Y ${W}x${H}"
flood 3000
wait "$CLIENT_PID"; RC=$?
CLIENT_PID=""

if ! grep -aq "Data too big for buffer" "$LOG"; then
	pass "没有出现 Data too big（发送缓冲区够大）"
else
	fail "出现了 Data too big：$(grep -a 'Data too big' "$LOG" | tail -1)"
fi
if ! grep -aq "写事件失败" "$LOG"; then
	pass "没有 sendmsg 失败"
else
	fail "sendmsg 失败：$(grep -a '写事件失败' "$LOG" | tail -1)"
fi
GOT=$(grep -ao "收到 [0-9]* 条 motion" "$RUNDIR/client-stall.log" |
	sed -E 's/.*收到 ([0-9]+) 条 motion.*/\1/' | tail -1)
if [ "${GOT:-0}" -ge 1000 ]; then
	pass "灌进去的事件真的到了客户端（$GOT 条 motion 排队等着读）"
else
	fail "客户端只收到 ${GOT:-0} 条 motion —— 灌的时候窗口没吃到指针"
fi
if [ "$RC" -eq 0 ] && grep -aq "stalltest ok" "$RUNDIR/client-stall.log"; then
	pass "客户端活着醒来，正常往返（退出码 0）"
else
	fail "客户端退出码=$RC（应为 0）：$(tail -3 "$RUNDIR/client-stall.log")"
fi

mark "2. 探针真的链进来了（否则下次又只能看到一句 error in client communication）"
if nm -D ./build/vidow 2>/dev/null | grep -qw sendmsg; then
	pass "build/vidow 的动态符号表里有 sendmsg（src/waylandfd.dynlist 生效）"
else
	fail "build/vidow 没有导出 sendmsg —— 检查 meson.build 的 link_args"
fi
if grep -q "waylandfd" build/meson-info/intro-targets.json 2>/dev/null ||
		[ -f src/waylandfd.c ]; then
	pass "探针源码在位（src/waylandfd.c）"
else
	fail "src/waylandfd.c 不见了"
fi

echo
if [ "$FAILED" -eq 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
