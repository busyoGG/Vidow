#!/bin/bash
#
# 结论（2026-10-25）：根因是 SIGPIPE —— 见 README 第 32 条，本脚本留作观测工具。
# 复现「kitty 里跑 vshell 按 Ctrl+C → vidow 退出」并抓出退出原因。
#
# 做法：不重启桌面。用 VIDOW_LOG_FILE 让正在跑的 vidow 之外的信息不可得，
# 所以这里改成「在桌面里起一条独立的 vidow 探测」是不行的（会和真实例抢设备）。
#
# 因此本脚本只做两件事（只读 / 只观测，不动用户的 vidow）：
#   1) 每 200ms 记录一次 vidow 的 /proc/<pid>/status 与信号队列；
#   2) vidow 消失时，打印消失前最后一帧的 SigBlk/SigPnd/ShdPnd + logind 事件。
#
# 用法：tools/repro-ctrlc-exit.sh [秒数，默认 120]
set -u

DURATION="${1:-120}"
OUT="/tmp/vidow-ctrlc-repro-$(date +%H%M%S).log"

pid=$(pgrep -xo vidow || true)
if [ -z "$pid" ]; then
	echo "vidow 没在跑" >&2
	exit 1
fi

echo "监视 pid=$pid 共 ${DURATION}s，输出 $OUT"
exec > >(tee -a "$OUT") 2>&1

deadline=$(( $(date +%s) + DURATION ))
while [ "$(date +%s)" -lt "$deadline" ]; do
	if ! kill -0 "$pid" 2>/dev/null; then
		echo "=== vidow($pid) 消失于 $(date +%T) ==="
		echo "--- 消失前进程状态（若已消失则为空）---"
		grep -E '^(Name|State|SigBlk|SigIgn|SigCgt|SigPnd|ShdPnd|Threads)' \
			"/proc/$pid/status" 2>/dev/null
		echo "--- 内核日志（信号导致的退出会留痕）---"
		journalctl --since '-3min' --no-pager 2>/dev/null |
			grep -aiE 'vidow|session (closed|logged out)|sddm-helper exited' |
			tail -25
		exit 0
	fi
	sleep 0.2
done

echo "=== ${DURATION}s 内 vidow 一直活着 ==="
grep -E '^(SigBlk|SigIgn|SigPnd|ShdPnd)' "/proc/$pid/status" 2>/dev/null
