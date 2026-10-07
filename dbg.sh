#!/bin/bash
# ./dbg.sh —— tty 上的调试入口：起来 → 复现 → Ctrl+C，日志我自己去读。
#
#   ./dbg.sh            日志写 /tmp/vidow-dbg.log
#   ./dbg.sh /tmp/x.log 写到别处
#
# 做的事：用 debug 级别起 vidow，把日志里**每帧刷屏**的那几类（DRM 直扫失败、
# 光标纹理、GBM 分配、FBO 创建）过滤掉再写文件 —— 那些占 20MB/分钟的 6804 行
# 对排查没用，留着反而把终端刷花、把性能测成假的。剩下的照旧一行不少。
#
# 注意：vidow 要独占一块 tty，所以先把正在跑的那个退掉（你平时的退出键位，或者
# 在它的终端里按 Ctrl+C），再跑这个脚本。
set -u
cd "$(dirname "$0")"

LOG=${1:-/tmp/vidow-dbg.log}
BIN=${VIDOW_BIN:-./build/vidow}
export VIDOW_LOG=${VIDOW_LOG:-debug}

: > "$LOG"
echo "== vidow 起来了（$BIN，日志 $LOG）=="
echo "== 复现完按 Ctrl+C 退出（或用你平时的退出键位）=="
echo

"$BIN" 2>&1 |
	grep --line-buffered -av \
		-e "drm/drm\.c:" \
		-e "drm/fb\.c:" \
		-e "output/cursor\.c:" \
		-e "allocator/gbm\.c:" \
		-e "gles2/renderer\.c:157" |
	tee -a "$LOG"

echo
echo "== 日志在 $LOG（$(wc -l < "$LOG") 行）=="
