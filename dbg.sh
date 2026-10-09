#!/bin/bash
# ./dbg.sh —— tty 上的调试入口：起来 → 复现 → 退出，日志和崩溃栈它自己收好。
#
#   ./dbg.sh            日志写 /tmp/vidow-dbg.log，诊断块写 /tmp/vidow-dbg.diag
#   ./dbg.sh /tmp/x.log 写到别处（诊断块跟着叫 /tmp/x.diag）
#
# 做的事：用 debug 级别起 vidow，把日志里**每帧刷屏**的那几类（DRM 直扫失败、
# 光标纹理、GBM 分配、FBO 创建）过滤掉再写文件 —— 那些占 20MB/分钟的 6804 行
# 对排查没用，留着反而把终端刷花、把性能测成假的。剩下的照旧一行不少。
#
# 退出时（正常退 / 崩 / Ctrl+C 都算）它把三样打在屏幕上，同时整份收进 .diag：
#   ① vidow 的退出码（139 = 段错误，134 = abort，137 = 被 kill -9）
#   ② **这次新产生的 coredump 的调用栈**（崩了的话，要的就是它）
#   ③ 日志里「录屏 / 光标」那几行 + 日志最后 15 行
# tty 上翻屏 / 复制都麻烦：跑完照 .diag 那个文件抄（或者 `less` 它）就行。
#
# 注意：vidow 要独占一块 tty，所以先把正在跑的那个退掉（你平时的退出键位，或者
# 在它的终端里按 Ctrl+C），再跑这个脚本。
set -u
cd "$(dirname "$0")"

LOG=${1:-/tmp/vidow-dbg.log}
DIAG="${LOG%.log}.diag"
BIN=${VIDOW_BIN:-./build/vidow}
export VIDOW_LOG=${VIDOW_LOG:-debug}

START_TS=$(date +%s)
: > "$LOG"
echo "== vidow 起来了（$BIN，日志 $LOG）=="
echo "== 复现完按 Ctrl+C 退出（或用你平时的退出键位）=="
echo

# Ctrl+C 的时候终端把 SIGINT 发给**整个前台进程组**（vidow 也在里面）：grep / tee
# 先退，vidow 再往管道里写日志就会吃 SIGPIPE —— 那会跳过它自己的收尾（退出码 141，
# 而且看起来像「崩了」）。所以用一个小壳把 SIGPIPE 设成忽略再 exec（忽略是继承的），
# 日志写不出去只是白写，进程照常走自己的退出流程。
bash -c 'trap "" PIPE; exec "$@"' _ "$BIN" 2>&1 |
	grep --line-buffered -av \
		-e "drm/drm\.c:" \
		-e "drm/fb\.c:" \
		-e "output/cursor\.c:" \
		-e "allocator/gbm\.c:" \
		-e "gles2/renderer\.c:157" |
	tee -a "$LOG"
# 管道里 tee 的退出码是 tee 的：vidow 自己的那个在 PIPESTATUS[0]（壳是 exec 上去的，
# 所以就是 vidow 的退出码）。
RC=${PIPESTATUS[0]}

WHY=""
case "$RC" in
139) WHY=" = SIGSEGV 段错误" ;;
134) WHY=" = SIGABRT（断言 / abort）" ;;
137) WHY=" = SIGKILL（被 kill -9？）" ;;
esac
echo
echo "== vidow 退出了（退出码 $RC$WHY）=="

{
	echo "vidow 退出码 $RC$WHY（$(date -Is)）"
	echo
	echo "--- 录屏 / 光标（日志里最后 40 行）---"
	grep -aE "screencast|cursor_dst_pos|wincapture" "$LOG" | tail -40
	echo
	echo "--- 日志最后 15 行 ---"
	tail -15 "$LOG"
	echo
	echo "--- 这次新产生的 coredump ---"
	if ! command -v coredumpctl >/dev/null 2>&1; then
		echo "（这台机器没有 coredumpctl）"
	elif [ "$(coredumpctl list --since "@$START_TS" --no-legend 2>/dev/null |
			grep -c vidow)" = 0 ]; then
		echo "没有新 coredump（这次没崩）"
	else
		coredumpctl list --since "@$START_TS" --no-legend 2>/dev/null |
			grep vidow | sed 's/^/  /'
		echo
		# 头（PID / 信号 / si_code）+ 调用栈：崩溃排查要的就是这几行。
		coredumpctl info --since "@$START_TS" 2>/dev/null |
			grep -aE "^ *(PID|Signal|si_code|Stack trace of thread):|^ *#[0-9]+" |
			head -40
	fi
} | tee "$DIAG"

echo
echo "== 诊断块：$DIAG（$(wc -l < "$DIAG") 行）—— 这份抄给我 =="
echo "== 日志：$LOG（$(wc -l < "$LOG") 行）=="
