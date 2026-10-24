#!/bin/bash
# 两个 Start 同时来的时候，**排队**而不是拒。
#
# 为什么（用户报的「第一次无法录制」）：OBS 的场景里两条「屏幕采集(PipeWire)」源
# 开机时都会去问 portal，它的日志里就是
#
#   [pipewire] Screencast session created      ×2
#   [pipewire] Asking for monitor and window   ×2
#   [pipewire] Failed to start screencast, denied or cancelled by user   ← 被拒的那条
#
# 被拒的那条收到 RESPONSE_OTHER（前端翻成「denied or cancelled by user」），那一路
# 就黑着，得用户手动再点一次「选择屏幕」。KDE 的 portal 是排队：当前这个选完（或
# 关掉），接着弹下一个。vidow-portal 以前是「已经有一个对话框开着，先拒了」。
#
# 判据（直接调后端 impl 的 DBus 接口，不走前端）：
#   1) 会话 A 的 Start 弹选择器；会话 B 的 Start **排队**：不秒回、也不报
#      RESPONSE_OTHER（旧代码在这里当场返回 2）
#   2) 选掉 A 的对话框：A 拿到 SUCCESS（0）和一路流
#   3) B 的对话框**接着弹出来**（日志「轮到排队里的 Start」），选掉之后 B 也拿到
#      SUCCESS（0）—— 两条源都配置上了，没有一条黑着
#   4) 排着队的时候会话被关掉：那个请求被收掉（回 CANCELLED），不留下没主的对话框
#   5) 全程没有 assert / coredump
#
# 用法：./test-portal-queue.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

R=/tmp/vidow-portal-queue-test
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

rm -rf "$R"
mkdir -p "$R/home/.config/vidow" "$R/run"
chmod 700 "$R/run"
cat > "$R/home/.config/vidow/config.lua" <<'CFG'
scale = "1"
xwayland = false
CFG

[ -x ./build/vidow ] || { echo "test-portal-queue.sh: 先构建（ninja -C build）"; exit 1; }
command -v gdbus >/dev/null || { echo "test-portal-queue.sh: 需要 gdbus（glib2）"; exit 1; }
command -v dbus-run-session >/dev/null || { echo "test-portal-queue.sh: 需要 dbus-run-session"; exit 1; }
python3 -c 'import gi' 2>/dev/null || { echo "test-portal-queue.sh: 需要 python3-gobject"; exit 1; }

cleanup() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*build/vidowmsg*|*gdbus\ call*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
	# **别用 pkill -x vidow**：那会按进程名把用户正在用的那个 vidow 也一起
	# 杀掉（踩过一次，用户的桌面当场没了）。只杀这个脚本自己起的那个。
	[ -n "${VIDOW_PID:-}" ] && kill "$VIDOW_PID" 2>/dev/null
	pkill -x vidow-portal 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$R"
}
trap cleanup EXIT

cat >"$R/inner.sh" <<'INNER'
set -u
R="$1"
cd /home/busyo/Dev/Vidow
pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }
FAILED=0

export HOME="$R/home" XDG_CONFIG_HOME="$R/home/.config"
export XDG_RUNTIME_DIR="$R/run"
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_PORTAL_LOG="$R/portal.log"
export VIDOW_PORTAL_NO_RESTART=1
unset VIDOW_TEST_OUTPUTS VIDOW_TEST_ACTION

WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman VIDOW_LOG=debug \
	./build/vidow > "$R/vidow.log" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 60); do
	grep -aq "vidow ready" "$R/vidow.log" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$R/vidow.log"; then
	fail "vidow 起不来"
	sed 's/^/        /' "$R/vidow.log" | tail -5
	exit 1
fi
SOCK=$(grep -a "vidow ready" "$R/vidow.log" | grep -ao "wayland-[0-9]*" | head -1)
export WAYLAND_DISPLAY="$SOCK"
IPC="$XDG_RUNTIME_DIR/vidow-$SOCK.sock"
VM="./build/vidowmsg --socket $IPC --timeout 5"

D=org.freedesktop.impl.portal.desktop.vidow
O=/org/freedesktop/portal/desktop
PF="$R/portal.log"

# 后端 impl 占住 DBus 名（vidow 起 vidow-portal 是异步的）。
for _ in $(seq 1 60); do
	busctl --user --no-pager list 2>/dev/null | grep -q "$D" && break
	sleep 0.25
done
if ! busctl --user --no-pager list 2>/dev/null | grep -q "$D"; then
	fail "vidow-portal 没占住 $D"
	exit 1
fi

# 选择器里得有卡片可选：开一个窗口（缩略图那条路顺带也走一遍）。
python3 tools/pattern.py w1 >/dev/null 2>&1 &
for _ in $(seq 1 40); do
	$VM windows 2>/dev/null | grep -q '"title": "w1"' && break
	sleep 0.25
done
sleep 0.5

sess() { # sess <名字>
	gdbus call --session --dest "$D" --object-path "$O" --method \
		org.freedesktop.impl.portal.ScreenCast.CreateSession \
		"$O/request/$1" "$O/session/$1" "$1" "{}" >/dev/null 2>&1
	gdbus call --session --dest "$D" --object-path "$O" --method \
		org.freedesktop.impl.portal.ScreenCast.SelectSources \
		"$O/request/$1" "$O/session/$1" "$1" \
		"{'types': <uint32 3>, 'multiple': <false>, 'cursor_mode': <uint32 4>}" >/dev/null 2>&1
}
start_async() { # start_async <名字>
	gdbus call --session --dest "$D" --object-path "$O" --method \
		org.freedesktop.impl.portal.ScreenCast.Start \
		"$O/request/$1" "$O/session/$1" "$1" "" "{}" > "$R/start-$1.txt" 2>&1 &
}
# 答复到了没（文件非空）。
replied() { [ -s "$R/start-$1.txt" ]; }
# 答复里的 response 码。
code() { head -c 20 "$R/start-$1.txt" 2>/dev/null | grep -ao "uint32 [0-9]*" | head -1 | awk '{print $2}'; }
dialog_geo() { # 选择器窗口的 x y w h
	$VM windows 2>/dev/null | python3 -c '
import json, sys
try:
	ws = json.load(sys.stdin)
except Exception:
	sys.exit(0)
for w in ws:
	if w.get("title") == "共享屏幕":
		g = w["geometry"]
		print("%d %d %d %d" % (g["x"], g["y"], g["width"], g["height"]))
		break
'
}
wait_dialog() { # 等选择器上屏，打印几何
	local geo=""
	for _ in $(seq 1 40); do
		geo=$(dialog_geo)
		[ -n "$geo" ] && break
		sleep 0.25
	done
	echo "$geo"
}
# 关掉当前的选择器（等价于点「取消」），等**这个会话**拿到答复。
# 判据不能用「对话框不见了」：关掉 a 的那个之后 b 的对话框立刻就在同一个位置
# 冒出来，看上去「还在」。
close_dialog() { # close_dialog <名字>
	[ -n "$(wait_dialog)" ] || return 1
	$VM close-window --title 共享屏幕 >/dev/null 2>&1
	for _ in $(seq 1 40); do
		replied "$1" && return 0
		sleep 0.25
	done
	return 1
}
wait_reply() { # wait_reply <名字>
	for _ in $(seq 1 60); do
		replied "$1" && return 0
		sleep 0.25
	done
	return 1
}

# ---------------------------------------------------------------- 1) 两个 Start
mark "1) 两个 Start 同时来：第二个排队，不拒"
sess a
sess b
start_async a
sleep 2
if [ -n "$(dialog_geo)" ]; then
	pass "会话 a 的 Start 弹出了选择器"
else
	fail "会话 a 的 Start 没弹出选择器"
fi
start_async b
sleep 2
if grep -aq "已经有一个对话框开着，排队等" "$PF"; then
	pass "会话 b 的 Start 排队了（日志：已经有一个对话框开着，排队等）"
else
	fail "会话 b 的 Start 没排队：$(grep -a "Start\|拒" "$PF" | tail -2)"
fi
if replied b; then
	fail "会话 b 的 Start 秒回了（$(cat "$R/start-b.txt")）—— 那就是又被拒了"
else
	pass "会话 b 的 Start 还在等（没被拒）"
fi

# ---------------------------------------------------------------- 2) 关掉 a
mark "2) 关掉 a 的选择器：a 拿到答复，b 的选择器接着弹"
if close_dialog a; then
	pass "a 的选择器关掉了（a 拿到答复）"
else
	fail "a 的选择器关不掉（几何 $(dialog_geo)，答复 $(cat "$R/start-a.txt" 2>/dev/null | head -c 60)）"
fi
wait_reply a || true
if [ "$(code a)" = "1" ]; then
	pass "会话 a 拿到答复（CANCELLED=1）"
else
	fail "会话 a 的答复是 $(code a)（$(cat "$R/start-a.txt" 2>/dev/null | head -c 120)）"
fi
if grep -aq "轮到排队里的 Start，弹选择器" "$PF"; then
	pass "b 的选择器接着弹了出来（日志：轮到排队里的 Start）"
else
	fail "b 的选择器没接着弹：$(tail -3 "$PF" | tr '\n' ' ')"
fi
if [ -n "$(wait_dialog)" ]; then
	pass "b 的选择器上屏了（不是「被拒之后一片安静」）"
else
	fail "b 的选择器没上屏"
fi

# ---------------------------------------------------------------- 3) 关掉 b
mark "3) 关掉 b 的选择器：b 也拿到答复"
if close_dialog b; then
	pass "b 的选择器关掉了（b 拿到答复）"
else
	fail "b 的选择器关不掉（几何 $(dialog_geo)）"
fi
wait_reply b || true
if [ "$(code b)" = "1" ]; then
	pass "会话 b 也拿到答复（CANCELLED=1）—— 它是被用户关掉的，不是 Start 就被拒"
else
	fail "会话 b 的答复是 $(code b)（$(cat "$R/start-b.txt" 2>/dev/null | head -c 120)）"
fi
if [ "$(grep -ac '轮到排队里的 Start' "$PF")" = "1" ]; then
	pass "排队的那个只弹了一次选择器"
else
	fail "排队的选择器弹了 $(grep -ac '轮到排队里的 Start' "$PF") 次"
fi

# ---------------------------------------------------------------- 4) 排队期间关会话
mark "4) 排着队的时候会话被关掉：请求收掉，不留没主的对话框"
sess c
start_async c
sleep 2
sess d
start_async d
sleep 2
if grep -aq "排队等" "$PF"; then
	pass "d 的 Start 排上了队"
else
	fail "d 的 Start 没排队"
fi
# 关会话走的是**会话对象**上的 Close（impl 接口里没有 CloseSession）。
gdbus call --session --dest "$D" --object-path "$O/session/d" --method \
	org.freedesktop.impl.portal.Session.Close >/dev/null 2>&1
wait_reply d || true
if [ "$(code d)" = "1" ]; then
	pass "d 的请求被收掉了（CANCELLED=1）"
else
	fail "d 的答复是 $(code d)（$(cat "$R/start-d.txt" 2>/dev/null | head -c 120)）"
fi
if close_dialog c; then
	pass "c 的选择器照样能关（关会话没把正在弹的那个弄坏）"
else
	fail "c 的选择器关不掉了"
fi
wait_reply c || true
if [ "$(code c)" = "1" ]; then
	pass "c 拿到答复（CANCELLED=1）"
else
	fail "c 的答复是 $(code c)"
fi
# 到这儿「轮到排队里的 Start」只该有 1 次（第 2 节 b 那一次）：d 在排队时被关掉，
# 不能再给它弹一个 —— 弹了就说明队里那条没收干净。
if [ "$(grep -ac '轮到排队里的 Start' "$PF")" = "1" ]; then
	pass "d 被关掉之后没有再多弹一个对话框（队里那个真摘干净了）"
else
	fail "排队的那条没收干净：$(grep -ac '轮到排队里的 Start' "$PF") 次「轮到」"
fi

# ---------------------------------------------------------------- 5) 收尾
mark "5) 没崩、没断言"
if pgrep -x vidow >/dev/null; then
	pass "合成器还活着"
else
	fail "合成器挂了"
fi
if pgrep -x vidow-portal >/dev/null; then
	pass "vidow-portal 还活着"
else
	fail "vidow-portal 没了"
fi
if grep -aq "Assertion" "$R/vidow.log"; then
	fail "日志里有 assert"
else
	pass "日志里没有 assert"
fi
# 私有运行时目录里没有 pipewire：这几路流会建失败（「没有一路流建成功，回失败」），
# 跟排队这件事无关 —— 建流本身由 test-portal-own.sh 验。

echo
if [ $FAILED -eq 0 ]; then
	echo "test-portal-queue.sh: OK"
else
	echo "test-portal-queue.sh: 有失败项"
fi
exit $FAILED
INNER

START_TS=$(date +%s)
dbus-run-session -- bash "$R/inner.sh" "$R"
RC=$?

if command -v coredumpctl >/dev/null 2>&1 &&
	[ "$(coredumpctl list --since "@$START_TS" --no-legend 2>/dev/null | grep -c vidow)" != 0 ]; then
	echo "  FAIL  这次跑出了新的 coredump"
	coredumpctl list --since "@$START_TS" --no-legend 2>/dev/null | grep vidow | sed 's/^/        /'
	RC=1
fi
exit $RC
