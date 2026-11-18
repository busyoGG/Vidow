#!/bin/bash
# 抓「GPU 飙升」的现场：每 20ms 记一次 GPU / 显存 / vidow 的 CPU，
# 跑完列出最尖的 10 个时刻 —— 数字对了再谈优化。
#
# 用法：./tools/gpu-watch.sh [秒数]      （默认 30 秒）
#       ./tools/gpu-watch.sh 30 > /tmp/gpu.txt
set -u
cd "$(dirname "$0")/.."
SECS=${1:-30}
OUT=/tmp/vidow-gpu-watch.txt
: > "$OUT"
PID=""
for p in $(ls /proc | grep -E "^[0-9]+$"); do
	[ "$(cat /proc/$p/comm 2>/dev/null)" = vidow ] || continue
	PID=$p
done
CARD=""
BEST=0
for c in /sys/class/drm/card[0-9]*/device; do
	[ -r "$c/mem_info_vram_total" ] || continue
	t=$(cat "$c/mem_info_vram_total")
	if [ "$t" -gt "$BEST" ]; then BEST=$t; CARD=$c; fi
done
[ -n "$CARD" ] || { echo "找不到 DRM 卡"; exit 1; }
echo "# vidow pid=${PID:-无} card=$CARD 采样 ${SECS}s / 20ms"
end=$(( $(date +%s%N) + SECS * 1000000000 ))
prev_cpu=0; prev_t=0
while [ "$(date +%s%N)" -lt "$end" ]; do
	now=$(date +%s%N)
	g=$(cat "$CARD/gpu_busy_percent" 2>/dev/null || echo 0)
	m=$(cat "$CARD/mem_busy_percent" 2>/dev/null || echo 0)
	v=$(cat "$CARD/mem_info_vram_used" 2>/dev/null || echo 0)
	cpu=0
	if [ -n "$PID" ]; then
		read -r ut st < <(awk '{print $14, $15}' /proc/$PID/stat 2>/dev/null || echo "0 0")
		tot=$((ut + st))
		if [ "$prev_t" -gt 0 ]; then
			dt=$(( (now - prev_t) / 1000000 ))
			[ "$dt" -gt 0 ] && cpu=$(( (tot - prev_cpu) * 100 / dt ))
		fi
		prev_cpu=$tot; prev_t=$now
	fi
	printf "%s %s %s %s %s\n" "$now" "$g" "$m" "$v" "$cpu" >> "$OUT"
	sleep 0.02
done
echo "# 样本 $(grep -c "" "$OUT")  明细 $OUT"
echo "# 最尖的 10 个 GPU 时刻（时间 / GPU% / 显存带宽% / VRAM MB / vidow CPU%）"
sort -k2 -nr "$OUT" | head -10 | awk '{printf "  %s  gpu=%s%%  mem=%s%%  vram=%dMB  cpu=%s%%\n", strftime("%H:%M:%S", substr($1,1,10)), $2, $3, $4/1048576, $5}'
awk 'BEGIN{n=0} !/^#/ {n++; g=$2+0; if(g>mx)mx=g; s+=g} END{printf "# 平均 GPU %.2f%%，峰值 %d%%\n", s/n, mx}' "$OUT"
