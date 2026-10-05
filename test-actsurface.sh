#!/bin/bash
# 激活令牌还攥着一个表面，那个表面先没了 —— 合成器不能崩。
#
# 靶子：`src/activation.c` 的 token_surface_gone() / build_surface_gone() 把
# surface 清成 NULL、把「挂过监听」的标志清成 false，**但没有把自己从
# surface->events.destroy 上摘下来**。wlroots 在 wlr_surface 的资源销毁时是先
# `wl_signal_emit_mutable(&surface->events.destroy)` 再断言「监听器链表已经空了」
# （types/wlr_compositor.c 的 surface_handle_resource_destroy）：
#
#   vidow: types/wlr_compositor.c:736: surface_handle_resource_destroy:
#   Assertion `wl_list_empty(&surface->events.destroy.listener_list)' failed.
#
# 就是这条把用户的会话带走的（SIGABRT，coredump 1037481）：微信启动时请求了激活
# （令牌上 set_surface 指着自己的登录窗），登录窗随后销毁 —— 断言炸。
#
# 判据：
#   1) 客户端拿令牌请求激活之后，把自己的窗口表面销毁（客户端还连着）
#   2) 合成器不崩、不 abort（日志里没有那条断言、进程还在）
#   3) 之后合成器照常工作（还能再开一个窗口、还响应 IPC）
#   4) 客户端断开之后也不崩（令牌的清理路径同样要摘 link）
#
# 用法：./test-actsurface.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-actsurface-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/vidowmsg ] || { echo "test-actsurface.sh: 先 ninja -C build"; exit 1; }
[ -x ./build/activate ] || { echo "test-actsurface.sh: 先 ninja -C build"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1 }
window_close_ms = 0
xwayland = false
CFG

BIN=${VIDOW_BIN:-./build/vidow}
setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-actsurface.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

alive() { kill -0 "$VIDOW_PID" 2>/dev/null; }
wait_for() { # <grep 模式> <最多等几秒>
	local i
	for i in $(seq 1 $(( $2 * 4 ))); do
		grep -aq "$1" "$LOG" && return 0
		alive || return 1
		sleep 0.25
	done
	return 1
}

mark "1/3 令牌攥着 surface，surface 先没了"
# 0 秒就用令牌请求激活（令牌在 map 时就建好了、带 set_surface），2 秒时把窗口
# 表面销毁掉 —— 客户端不断开连接，令牌还活着（默认不过期）。
setsid nohup ./build/activate demo 演示 0 --serial=none --drop-surface=2 \
	> "$RUNDIR/act.txt" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do
	grep -aq "^activate committed" "$RUNDIR/act.txt" 2>/dev/null && break
	sleep 0.25
done
if grep -aq "^activate committed" "$RUNDIR/act.txt" 2>/dev/null; then
	pass "客户端拿令牌请求过激活（令牌上带着自己的 surface）"
else
	fail "客户端没能请求激活：$(tail -2 "$RUNDIR/act.txt" 2>/dev/null | tr '\n' ' ')"
fi
for _ in $(seq 1 48); do
	grep -aq "^activate surface-dropped" "$RUNDIR/act.txt" 2>/dev/null && break
	alive || break
	sleep 0.25
done
if grep -aq "^activate surface-dropped" "$RUNDIR/act.txt" 2>/dev/null; then
	pass "表面已经销毁（客户端还连着）"
else
	fail "客户端没走到「销毁表面」那一步：$(tail -2 "$RUNDIR/act.txt" 2>/dev/null | tr '\n' ' ')"
fi
mark "2/3 合成器必须还活着"
sleep 1
if alive; then
	pass "表面销毁之后 vidow 还活着"
else
	fail "vidow 没了：$(tail -2 "$LOG" | tr '\n' ' ')"
fi
if grep -aq "wl_list_empty(&surface->events.destroy.listener_list)" "$LOG"; then
	fail "日志里有 wlroots 那条断言（destroy 监听器没摘干净）"
else
	pass "没有 wlroots 的 destroy-监听器断言"
fi
if timeout 5 ./build/vidowmsg ping >/dev/null 2>&1; then
	pass "IPC 还通"
else
	fail "IPC 不通了"
fi

mark "3/3 之后照常工作 + 客户端断开也不崩"
setsid nohup python3 ./tools/pattern.py after > "$RUNDIR/after.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 2
if timeout 5 ./build/vidowmsg windows 2>/dev/null | grep -q after; then
	pass "表面销毁之后还能正常开窗口"
else
	fail "表面销毁之后开不出窗口了"
fi
# 断开那个客户端：令牌的清理路径（token_client_gone → token_free）也要摘 link
kill $CLIENT_PIDS 2>/dev/null
CLIENT_PIDS=""
sleep 1
alive && pass "客户端断开之后 vidow 还活着" || fail "客户端断开把 vidow 带崩了"

echo
if [ "$FAILED" -eq 0 ]; then
	echo "test-actsurface.sh: 全部通过"
else
	echo "test-actsurface.sh: 有失败项"
	echo
	echo "--- 日志尾部 ---"
	tail -5 "$LOG"
fi
exit $FAILED
