#!/usr/bin/env bash
# 基线三分类：把 tests.tsv 里没过的测试单独重跑一遍（不与其他测试并发），
# 区分「批量跑才坏（计时/资源竞争）」和「稳定红（真回归 / 环境缺依赖）」。
set -uo pipefail
cd "$(dirname "$0")/.."

OUT=docs/migration-baseline
IN="$OUT/tests.tsv"
RES="$OUT/retry.tsv"
LOGDIR="$OUT/retry-logs"
mkdir -p "$LOGDIR"

printf 'script\tfirst\tretry\tseconds\n' > "$RES"
while IFS=$'\t' read -r t res secs; do
    [ "$t" = "script" ] && continue
    case "$res" in PASS|MANUAL) continue ;; esac
    case "$t" in test-steam-fix.sh) continue ;; esac   # 要真 Steam，别在基线里跑
    t0=$(date +%s)
    timeout 180 bash "./$t" > "$LOGDIR/${t%.sh}.log" 2>&1
    rc=$?
    dt=$(( $(date +%s) - t0 ))
    if [ "$rc" = 0 ]; then r=PASS; elif [ "$rc" = 124 ]; then r=TIMEOUT; else r=FAIL; fi
    printf '%s\t%s\t%s\t%d\n' "$t" "$res" "$r" "$dt" >> "$RES"
    echo "$t: $res -> $r (${dt}s)"
done < "$IN"

echo
echo "== 汇总"
awk -F'\t' 'NR>1 {k=$2" -> "$3; n[k]++} END {for (k in n) printf "%-18s %d\n", k, n[k]}' "$RES"
