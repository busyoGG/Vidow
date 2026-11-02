#!/bin/bash
# Ctrl+Alt+F2 必须真的走到切 VT（2026-11-03 用户报「怎么还是不能切换 tty」）。
#
# 根因：默认 keymap 里 Ctrl+Alt+Fn 被 srvr_ctrl(fkey2vt) 换成了 XF86Switch_VT_n：
#     key <FK02> { type="CTRL+ALT"; symbols[1]=[ F2, F2, F2, F2, XF86Switch_VT_2 ] };
# 也就是「Ctrl+Alt 这一档」解出来的 keysym 是 XF86Switch_VT_2，不是 F2。
# vidow_keybind_key 里内置的逃生口只认 XKB_KEY_F1..F12，于是这条永远是死的
# （实测整个会话日志里一条 VT 记录都没有）。
#
# 怎么造出真键盘的这条路径：wtype 的 keymap 里没有 fkey2vt（解出来是 F2），
# vidowmsg send-key 又按设计不跑键位表。所以用 tools/vtkeysym.c：一块自己送
# 系统 keymap 的虚拟键盘，按 Ctrl+Alt+F2。
#
# 判据：
#   1) vtkeysym vt2 之后日志里出现「VT:」——headless 没有会话，走的是
#      「VT: 没有会话（嵌套 / headless），切 VT 交给宿主」那条；真机上会是
#      「VT: 切到 2 号 tty」。两条都算命中。
#   2) 一次按下+松开只切一次（松开那一下也被吃掉，不再触发一条）。
#   3) 反例 Ctrl+Alt+a 不切 VT（不能把无关的键也吃掉）。
#
# 用法：./test-vt-keysym.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-vt-keysym-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1" >&2; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/vidowmsg ] || { echo "先构建（ninja -C build）"; exit 1; }
[ -x ./build/vtkeysym ] || { echo "先构建（ninja -C build）：缺 build/vtkeysym"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
printf 'scale = "1"\nxwayland = false\n' > "$CFGDIR/.config/vidow/config.lua"

export XDG_RUNTIME_DIR="$RUNDIR/run" HOME="$CFGDIR" VIDOW_LOG=info
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow >"$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 100); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.2
done
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
[ -n "$WAYLAND_DISPLAY" ] || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }

# grep -c 没有匹配时会打印 0 然后以 1 退出，所以别再补一个 echo 0（会变成两行）。
vt_count() { grep -ac "VT:" "$LOG" 2>/dev/null; }

mark "1. Ctrl+Alt+F2（真 keymap）命中切 VT"
before=$(vt_count)
./build/vtkeysym vt2 >/dev/null 2>&1 || fail "vtkeysym vt2 自己失败了"
sleep 0.4
after=$(vt_count)
LINE=$(grep -a "VT:" "$LOG" | tail -1)
if [ "$after" -gt "$before" ]; then
	pass "命中了切 VT（$LINE）"
else
	fail "Ctrl+Alt+F2 没有走到切 VT（日志里没有 VT: 行）"
	grep -a "KEYPROBE\|action:" "$LOG" | tail -5 >&2
fi
if echo "$LINE" | grep -q "没有会话"; then
	pass "headless 下报错明确（真机会是「切到 2 号 tty」）"
fi

mark "2. 按下 + 松开只切一次"
if [ "$((after - before))" -eq 1 ]; then
	pass "切换次数 = 1（松开那一下没有再触发）"
else
	fail "切换次数不对：$((after - before)) 次（应为 1）"
	grep -a "VT:" "$LOG" | tail -4 >&2
fi

mark "3. 反例：Ctrl+Alt+a 不切 VT"
before=$(vt_count)
./build/vtkeysym plain >/dev/null 2>&1 || fail "vtkeysym plain 自己失败了"
sleep 0.4
after=$(vt_count)
if [ "$after" -eq "$before" ]; then
	pass "Ctrl+Alt+a 不触发切 VT"
else
	fail "Ctrl+Alt+a 也切了 VT"
fi

echo
if [ "$FAILED" -eq 0 ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
