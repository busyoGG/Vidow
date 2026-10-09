#!/bin/bash
# 窗口抓帧源的**尺寸中途变了**：窗口挪到缩放不同的屏上，这一路流的像素尺寸跟着变
# （窗口逻辑尺寸 × 那块屏的 scale，见 src/wincapture.c 的 win_source_size），合成器
# 重发 buffer_size —— 客户端手上那块缓冲立刻就对不上了。
#
# 这时候合成器**只能回一次 failed**：wlroots 的
# wlr_ext_image_copy_capture_frame_v1_copy_buffer() 在尺寸对不上时自己就
# `fail(frame)` + return false，而 fail 会把 frame 销毁掉 —— 再补一句 fail 就是拿
# 一块 free 掉的内存去 wl_resource_post_event，当场 SEGV。用户的崩溃栈就是这个：
#
#   wl_resource_post_event ← wlr_ext_image_copy_capture_frame_v1_fail
#   ← source_copy_frame ← wl_source_emit ← source_request_frame
#
# 触发条件正好是用户那台机器上的日常：选择器开着（每个窗口一个缩略图抓帧会话），
# 窗口在 4K 缩放 2 那块屏和 1 倍那块屏之间挪了一下。
#
# 判据：
#   1) 两块屏：HEADLESS-1 缩放 1、HEADLESS-2 缩放 2（跟用户那台机器的几何一样）
#   2) 窗口在 1 倍屏上：抓帧尺寸 = 逻辑尺寸
#   3) 挪到 2 倍屏：客户端收到新的 buffer_size（= 逻辑 × 2），而且**只有一次** failed
#   4) 合成器没崩（进程还在、没新 coredump）—— 修之前这一条必挂
#   5) 客户端换掉缓冲之后接着出帧（不是每帧都 failed 的死循环）
#   6) 挪回 1 倍屏：再来一轮，同样只有一次 failed
#   7) 日志里没有 assert / abort
#
# 用法：./test-wincapture-scale.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-wincapture-scale-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
CAP="$RUNDIR/cap.txt"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""
START_TS=$(date +%s)

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*build/capture*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

cleanup() {
	stop_clients
	for pid in $CLIENT_PIDS; do
		[ -n "$pid" ] || continue
		kill "$pid" 2>/dev/null
	done
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/vidow ] || { echo "test-wincapture-scale.sh: 先构建（ninja -C build）"; exit 1; }
[ -x ./build/capture ] || { echo "test-wincapture-scale.sh: 先构建（ninja -C build）"; exit 1; }
python3 -c 'import gi' 2>/dev/null || { echo "test-wincapture-scale.sh: 需要 python3-gobject"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 两块屏位置钉死、缩放不同：这就是用户那台机器的几何（一块 4K 缩放 2 摆在 x=1280）。
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", x = 0, y = 0, scale = 1 }
vidow.output{ name = "HEADLESS-2", x = 1280, y = 0, scale = 2, mode = "2560x1440" }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"
unset VIDOW_TEST_OUTPUTS VIDOW_TEST_ACTION

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-wincapture-scale.sh: vidow 起不来"
	grep -a "ERROR" "$LOG" | head -5
	exit 1
fi
SOCK=$(grep -a "vidow ready" "$LOG" | grep -ao "wayland-[0-9]*" | head -1)
export WAYLAND_DISPLAY="$SOCK"
IPC="$XDG_RUNTIME_DIR/vidow-$SOCK.sock"
vm() { ./build/vidowmsg --socket "$IPC" --timeout 5 "$@"; }
where() { vm windows 2>/dev/null | grep -a '"output"' | head -1 | tr -d ' ' | cut -d: -f2 | tr -d '",'; }
alive() { [ -n "$VIDOW_PGID" ] && kill -0 "$VIDOW_PGID" 2>/dev/null; }

# 客户端报过的 buffer_size（去重、按出现顺序）。
sizes() { grep -a '^capture: buffer_size' "$CAP" | awk '{print $3}' | uniq; }
fails() { grep -ac '这一帧失败' "$CAP"; }
# 第 n 次 failed 之后又抓到多少帧（换缓冲之后有没有接着出帧）。
frames_after_last_fail() {
	awk '/这一帧失败/{n=NR} {lines[NR]=$0} END{for (i=n+1;i<=NR;i++) if (lines[i] ~ /^capture [0-9]/) c++; print c+0}' "$CAP"
}

python3 tools/pattern.py w1 >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
for _ in $(seq 1 40); do
	vm windows 2>/dev/null | grep -q '"title": "w1"' && break
	sleep 0.25
done
if ! vm windows 2>/dev/null | grep -q '"title": "w1"'; then
	fail "窗口没起来"
	exit 1
fi

# ---------------------------------------------------------------- 1) 1 倍屏上
mark "1) 窗口在 1 倍屏（HEADLESS-1）上抓帧"
vm move-window-to-output HEADLESS-1 --title w1 >/dev/null 2>&1
for _ in $(seq 1 20); do
	[ "$(where)" = "HEADLESS-1" ] && break
	sleep 0.25
done
if [ "$(where)" = "HEADLESS-1" ]; then
	pass "窗口在 HEADLESS-1（缩放 1）上"
else
	fail "窗口没挪到 HEADLESS-1（在 $(where)）"
fi
sleep 1.0

setsid nohup ./build/capture w1 14 10 > "$CAP" 2>&1 &
CAP_PID=$!
CLIENT_PIDS="$CLIENT_PIDS $CAP_PID"
sleep 3.0
S1=$(sizes | head -1)
echo "      1 倍屏上的流尺寸：${S1:-读不出来}"
if [ -n "$S1" ]; then
	pass "1 倍屏上抓帧尺寸 $S1"
else
	fail "1 倍屏上没拿到 buffer_size"
fi

# ---------------------------------------------------------------- 2) 挪到 2 倍屏
mark "2) 挪到 2 倍屏（HEADLESS-2）—— 尺寸翻倍，这一帧只能回一次 failed"
vm move-window-to-output HEADLESS-2 --title w1 >/dev/null 2>&1
for _ in $(seq 1 20); do
	[ "$(where)" = "HEADLESS-2" ] && break
	sleep 0.25
done
sleep 3.5
S2=$(sizes | tail -1)
echo "      2 倍屏上的流尺寸：${S2:-读不出来}（之前 $S1）"
if [ "$(where)" = "HEADLESS-2" ]; then
	pass "窗口在 HEADLESS-2（缩放 2）上"
else
	fail "窗口没挪到 HEADLESS-2（在 $(where)）"
fi
if [ -n "$S1" ] && [ -n "$S2" ] && [ "$S2" != "$S1" ]; then
	pass "换屏之后合成器重发了 buffer_size（$S1 -> $S2）"
else
	fail "换屏之后 buffer_size 没变（$S1 -> $S2）—— 这一节就测不到尺寸变化了"
fi
# 尺寸口径：2 倍屏上是 1 倍屏上的两倍（同一套「逻辑 × scale」）。
W1=${S1%x*}; W2=${S2%x*}
if [ -n "$W1" ] && [ -n "$W2" ] && [ "$W2" = "$((W1 * 2))" ]; then
	pass "2 倍屏上的流尺寸正好是 1 倍屏的两倍（$S2 = $S1 × 2）"
else
	fail "2 倍屏上的流尺寸不是 1 倍屏的两倍（$S1 -> $S2）"
fi
if alive; then
	pass "换屏那一帧合成器没崩（进程还在）—— 二次 fail 是 use-after-free"
else
	fail "合成器死了（换屏时二次 fail：拿 free 掉的内存去 wl_resource_post_event）"
fi
N_FAIL=$(fails)
if [ "$N_FAIL" -ge 1 ]; then
	pass "客户端收到 failed（${N_FAIL} 次）—— 缓冲对不上就得回 failed，不能闷着"
else
	fail "客户端一次 failed 都没收到（缓冲对不上还硬拷？）"
fi
if [ "$N_FAIL" -le 1 ]; then
	pass "整个换屏过程只回了一次 failed（没在死循环里刷）"
else
	fail "换屏过程回了 ${N_FAIL} 次 failed —— 客户端换了缓冲还在失败"
fi
AFTER=$(frames_after_last_fail)
if [ "${AFTER:-0}" -ge 10 ]; then
	pass "换缓冲之后接着出帧（failed 之后又抓到 ${AFTER} 帧）"
else
	fail "failed 之后只抓到 ${AFTER:-0} 帧 —— 客户端换缓冲没自愈"
fi
# 日志里那条「尺寸变了」：一条对应一次 failed（多一条就是多回了一次）。
MISMATCH=$(grep -ac "wincapture: w1 这一帧" "$LOG")
if [ "$MISMATCH" -eq "$N_FAIL" ]; then
	pass "日志里的「尺寸变了」条数跟 failed 次数对得上（$MISMATCH）"
else
	fail "日志里 ${MISMATCH} 条「尺寸变了」，客户端 ${N_FAIL} 次 failed（对不上）"
fi

# ---------------------------------------------------------------- 3) 挪回去
mark "3) 挪回 1 倍屏 —— 再来一轮"
vm move-window-to-output HEADLESS-1 --title w1 >/dev/null 2>&1
for _ in $(seq 1 20); do
	[ "$(where)" = "HEADLESS-1" ] && break
	sleep 0.25
done
sleep 3.0
S3=$(sizes | tail -1)
echo "      挪回去之后的流尺寸：${S3:-读不出来}"
if [ "$S3" = "$S1" ]; then
	pass "挪回 1 倍屏：流尺寸回到 $S3"
else
	fail "挪回 1 倍屏：流尺寸是 ${S3:-读不出来}，期望 $S1"
fi
if [ "$(fails)" -le 2 ]; then
	pass "两趟换屏一共只回了 $(fails) 次 failed"
else
	fail "两趟换屏回了 $(fails) 次 failed（期望 ≤2）"
fi
kill "$CAP_PID" 2>/dev/null
TOTAL=$(grep -ac '^capture [0-9]' "$CAP")
if [ "${TOTAL:-0}" -ge 40 ]; then
	pass "全程抓到 ${TOTAL} 帧（换屏没把这一路抓帧打断）"
else
	fail "全程只抓到 ${TOTAL:-0} 帧"
fi

# ---------------------------------------------------------------- 4) 收尾
mark "4) 没崩、没断言"
if alive; then
	pass "合成器还活着"
else
	fail "合成器挂了"
fi
if grep -aq "Assertion" "$LOG"; then
	fail "日志里有 assert"
else
	pass "日志里没有 assert"
fi
if command -v coredumpctl >/dev/null 2>&1 &&
	[ "$(coredumpctl list --since "@$START_TS" --no-legend 2>/dev/null | grep -c vidow)" != 0 ]; then
	fail "这次跑出了新的 coredump"
	coredumpctl list --since "@$START_TS" --no-legend 2>/dev/null | grep vidow | sed 's/^/        /'
else
	pass "没有新 coredump"
fi

echo
if [ $FAILED -eq 0 ]; then
	echo "test-wincapture-scale.sh: OK"
else
	echo "test-wincapture-scale.sh: 有失败项"
fi
exit $FAILED
