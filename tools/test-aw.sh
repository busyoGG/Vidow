#!/usr/bin/env bash
# 测试 A+W 同时按住时 X11 客户端收到的事件序列 —— 不用打开游戏。
#
# 用法：
#   tools/test-aw.sh            # 跑全部 4 个用例
#   tools/test-aw.sh baseline   # 只跑某一个
#
# 用例：
#   baseline   XTEST 直接注入（绕过 Vidow），看 X autorepeat 原生行为
#   single     单键长按（对照组，应该重复）
#   combo      A+W 同时按住（用户的症状）
#   switch     按住 A+W 期间制造焦点切换 / 键盘重配（验证 autorepeat 被重置）
#
# 判读：
#   每个用例末尾打印 "RELEASE 数" 和 "交替对数"。
#   combo 用例里若 A 和 W 都各有大量 RELEASE，说明两个键都在重复 —— 抖动来源。
#   正常 X11 只有**最后按下的那个键**重复。

set -u
cd "$(dirname "$0")/.." || exit 1

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
SOCK="$XDG_RUNTIME_DIR/vidow-wayland-0.sock"
REC=./build/xtest-keys
MSG=./build/vidowmsg
OUT=/tmp/aw-test

mkdir -p "$OUT"

if [ ! -x "$REC" ]; then
	echo "先编译记录器：cc -O2 -o build/xtest-keys tools/xtest-keys.c -lX11"
	exit 1
fi

inject() { DISPLAY=:0 "$MSG" --socket "$SOCK" send-key "$@" >/dev/null 2>&1; }

# 跑一个用例：$1=名字 $2=秒数 $3=测试体（bash 函数名）
run_case() {
	local name="$1" secs="$2" body="$3"
	local file="$OUT/$name.txt"
	echo "──────── $name ────────"
	DISPLAY=:0 "$REC" "$secs" > "$file" 2>&1 &
	local xpid=$!
	# 等记录器抢到焦点
	for _ in $(seq 1 40); do
		grep -q READY "$file" 2>/dev/null && break
		sleep 0.05
	done
	if ! grep -q READY "$file" 2>/dev/null; then
		echo "  记录器没起来："
		sed 's/^/    /' "$file"
		wait $xpid 2>/dev/null
		return
	fi
	$body
	wait $xpid 2>/dev/null
	summarize "$name" "$file"
}

summarize() {
	local name="$1" file="$2"
	local na nw pairs
	# grep -c 无匹配时退出码 1，`|| echo 0` 会和它自己打印的 0 叠成两行，
	# 下面 [ ] 就报「需要整数」。用变量兜底，别用 ||。
	na=$(grep -c 'RELEASE.*keycode=38' "$file" 2>/dev/null) || na=0
	nw=$(grep -c 'RELEASE.*keycode=25' "$file" 2>/dev/null) || nw=0
	na=${na:-0}; nw=${nw:-0}
	# 交替对：RELEASE 后紧邻同一 keycode 的 PRESS（同一毫秒）
	pairs=$(grep -E 'RELEASE|PRESS' "$file" | awk '
		/RELEASE/ { last_rel = $3 }
		/PRESS/   { if (last_rel == $3) n++; last_rel = "" }
		END { print n+0 }')
	printf '  A 松开 %-4s W 松开 %-4s  release+press 同刻对 %s\n' "$na" "$nw" "$pairs"
	if [ "$na" -gt 3 ] && [ "$nw" -gt 3 ]; then
		echo "  → A 和 W 都在重复（两个键同时 autorepeat）"
	elif [ "$nw" -gt 3 ]; then
		echo "  → 只有 W 在重复（X 标准行为：只重复最后按下的键）"
	elif [ "$na" -gt 3 ]; then
		echo "  → 只有 A 在重复"
	else
		echo "  → 没有重复"
	fi
}

case_baseline() {
	# 绕过 Vidow，直接走 XTEST —— 这是「裸 X server」的行为
	DISPLAY=:0 xdotool keydown a
	DISPLAY=:0 xdotool keydown w
	sleep 2
	DISPLAY=:0 xdotool keyup w
	DISPLAY=:0 xdotool keyup a
}

case_single() {
	inject press a
	sleep 2
	inject release a
}

case_combo() {
	inject press a
	inject press w
	sleep 2
	inject release w
	inject release a
}

case_switch() {
	# 按住期间制造焦点切换，看 autorepeat 会不会被重置 / 状态错乱
	inject press a
	inject press w
	sleep 0.8
	DISPLAY=:0 "$MSG" --socket "$SOCK" state >/dev/null 2>&1
	DISPLAY=:0 "$MSG" --socket "$SOCK" focus-window-direction 1 >/dev/null 2>&1
	sleep 0.3
	DISPLAY=:0 "$MSG" --socket "$SOCK" focus-window-direction -1 >/dev/null 2>&1
	sleep 1
	inject release w
	inject release a
}

want="${1:-all}"
echo "记录器：$REC   注入：$MSG"
echo "输出：$OUT/"
echo
for c in baseline single combo switch; do
	if [ "$want" = "all" ] || [ "$want" = "$c" ]; then
		case "$c" in
		baseline) run_case baseline 5 case_baseline ;;
		single)   run_case single   4 case_single ;;
		combo)    run_case combo    4 case_combo ;;
		switch)   run_case switch   5 case_switch ;;
		esac
		echo
	fi
done
echo "全部完成。原始记录在 $OUT/"
