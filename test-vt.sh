#!/bin/bash
# tty 上的 VT 切换（Ctrl+Alt+F1..F12 / `vidowmsg action switch-vt n`）。
#
# 背景：内核一般自己就把 Ctrl+Alt+Fn 处理掉了（合成器不 EVIOCGRAB，控制台键盘那一侧
# 也收得到），所以真机上这条通常是空转。但「在 tty 上开 vidow 就切不走 tty」是收到过
# 的报告，而 X 服务器就是自己实现的（它抓了设备，内核收不到这个键）—— 所以 vidow 也
# 自己接一遍：两种情况下都能用。
#
# headless / 嵌套里**没有会话**，切不了，这条测试只能验「判据认得出来、不接管、不崩」：
#   1) 启动日志说清楚有没有会话
#   2) 注入 Ctrl+Alt+F2：认得出来（日志说「没有会话，交给宿主」），而且**不吃**这个键
#      （嵌套里那是宿主 Hyprland 的 Ctrl+Alt+F2，吃掉会让宿主切不了）
#   3) `vidowmsg action switch-vt 3` 优雅失败（不是崩溃 / 不是静默成功）
#   4) 越界的 tty 号（0 / 99）被拒
#   5) config.lua 里绑的 `switch-vt` 动作也走同一条路（真机上就是它生效）
#   6) 全程没有崩溃 / 断言
#
# 用法：./test-vt.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-vt-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	if [ -n "$VIDOW_PID" ]; then
		kill -TERM -- "-$VIDOW_PID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v wtype >/dev/null || { echo "test-vt.sh: 需要 wtype"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1 }
-- 真机上生效的就是这条（headless 没有会话，只会记一行「没有会话」）
vidow.bind("Ctrl+Alt+P", "switch-vt 3")
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

mark "1) 启动时说清楚有没有会话"
grep -a "会话:" "$LOG" | tail -1
grep -aq "会话: 没有（嵌套 / headless 后端）" "$LOG" &&
	pass "headless 下明说「没有会话」" || fail "没说明会话情况"

mark "2) Ctrl+Alt+F2：认得出来、不接管"
wtype -M ctrl -M alt -k F2 2>/dev/null
sleep 0.4
grep -a "VT:" "$LOG" | tail -1
grep -aq "VT: 没有会话（嵌套 / headless），Ctrl+Alt+F2 交给宿主" "$LOG" &&
	pass "认得出 Ctrl+Alt+F2（F1..F12 + Ctrl+Alt 就是 VT 键）" ||
	fail "没认出 Ctrl+Alt+F2"
pgrep -x vidow >/dev/null && pass "注入之后合成器还活着" || fail "合成器挂了"

mark "3) IPC 动作 switch-vt：优雅失败"
out=$(./build/vidowmsg action switch-vt 3 2>&1); rc=$?
echo "      $out（rc=$rc）"
[ $rc -ne 0 ] && pass "没有会话时明确失败（rc=$rc）" || fail "居然报了成功"
grep -aq "VT: 没有会话" "$LOG" && pass "日志里说了原因" || fail "没说原因"

mark "4) 越界的 tty 号被拒"
./build/vidowmsg action switch-vt 99 >/dev/null 2>&1 &&
	fail "99 号 tty 居然成功" || pass "99 号被拒"
./build/vidowmsg action switch-vt 0 >/dev/null 2>&1 &&
	fail "0 号 tty 居然成功" || pass "0 号被拒"
grep -aq "switch-vt 要 1..12 号 tty" "$LOG" && pass "日志里说了范围" ||
	fail "没说范围"

mark "5) config.lua 里绑的 switch-vt 动作（真机上生效的那条）"
wtype -M ctrl -M alt -k p 2>/dev/null
sleep 0.4
n=$(grep -ac "VT: 没有会话" "$LOG")
[ "$n" -ge 2 ] && pass "绑定的动作也走到了同一条路（$n 次）" ||
	fail "绑定的动作没生效（$n 次）"

mark "6) 没崩、没断言"
pgrep -x vidow >/dev/null && pass "合成器还活着" || fail "合成器挂了"
grep -aq "Assertion" "$LOG" && fail "日志里有 assert" || pass "日志里没有 assert"

echo
if [ $FAILED -eq 0 ]; then
	echo "test-vt.sh: OK"
else
	echo "test-vt.sh: 有失败项"
fi
exit $FAILED
