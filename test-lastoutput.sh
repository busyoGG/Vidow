#!/bin/bash
# 屏没了之后，列 / 列里的窗口不能留着**野指针**。
#
# 靶子：`ws_set_output_full()` 里换屏那一段原来是
#     if (output == NULL) return;          ← 提前返回
#     wl_list_for_each(col, &ws->columns, link) column_set_output(col, output);
# 于是「这块工作区现在没有屏」的时候（最后一块屏也没了：关掉嵌套会话的窗口、
# 拔掉唯一一块显示器），列和列里的窗口的 output 还指着那块**已经被释放**的屏。
# 之后谁读它谁拿到垃圾：IPC 里输出名是一串乱码（`vidowmsg windows` 的 output
# 字段），XWayland 的 X 屏幕还会变成 0x0 —— X11 客户端看不见也点不动。
# 用户报的「xwayland 无法点击交互，但 CSD 区域可以拖拽（微信）」就是这条路：
# 他们的 vidow 是嵌套跑的，把 vidow 那个窗口关了 = 最后一块屏没了。
#
# 判据：
#   1) 两块屏、各一个窗口：拔掉其中一块（真 wlr_output_destroy）之后，挪过来的
#      窗口的 output 是**剩下那块屏的名字**，不是乱码也不是 NULL
#   2) 插回来之后，原来那块屏上的窗口 output 又变回它自己的名字（按 home 认领）
#   3) 嵌套会话：把外层那个窗口关掉（= 最后一块屏没了）之后
#      —— 每个窗口的 output 是 null（不是乱码）
#      —— 每块工作区的 output 是 null，但 home 还记得那块屏的名字
#      —— 记忆里那块屏 present=false（插回来还要按它认领）
#      —— vidow 还活着，且不崩
#   4) 这种状态下有 X 客户端连上来：日志里明说「一块屏都没有 → X 屏幕 0x0，
#      X11 客户端看不见也点不动」（不然用户只会去查输入，查半天是会话没屏）
#   5) 这种状态下 map 上来的窗口照样要收下（挂在一条现在没屏的工作区上）——
#      不许崩（原来这里解引用空工作区，一 map 就 SEGV）
#   6) 屏回来的时候，没屏期间收下的窗口跟着那块工作区一起回来（output 认领
#      回去、窗口重新上屏）
#
# 用法：./test-lastoutput.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-lastoutput-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""
NEST_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

# 本测试起的 vidow：**环境里带着本测试 RUNDIR 的那些**。别只认 $! ——
# `setsid … &` 那一支里 setsid 有可能 fork，$! 是已经退出的父进程，kill 它等于
# 什么都没做（实测每跑一遍漏一个 vidow 在那儿）。
our_vidow_pids() {
	for p in /proc/[0-9]*; do
		local pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		case "$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)" in
		*"build/vidow"*) ;;
		*) continue ;;
		esac
		[ -r "$p/environ" ] || continue
		case "$(tr '\0' '\n' < "$p/environ" 2>/dev/null)" in
		*"$RUNDIR"*) echo "$pid" ;;
		esac
	done 2>/dev/null
}

kill_strays() {
	local pid
	for pid in $(our_vidow_pids); do
		kill -TERM "$pid" 2>/dev/null
	done
	sleep 0.4
	for pid in $(our_vidow_pids); do
		kill -KILL "$pid" 2>/dev/null
	done
}

cleanup() {
	for p in $NEST_PID $VIDOW_PID; do
		[ -n "$p" ] && kill -TERM -- "-$p" 2>/dev/null
	done
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	kill_strays
	sleep 0.3
	for p in $NEST_PID $VIDOW_PID; do
		[ -n "$p" ] && kill -KILL -- "-$p" 2>/dev/null
	done
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi" 2>/dev/null || { echo "test-lastoutput.sh: 需要 python3-gobject"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-lastoutput.sh: 先 ninja -C build"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run" "$RUNDIR/nest-home/.config/vidow"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_LIBINPUT_NO_DEVICES=1
export DISPLAY= WAYLAND_DISPLAY=

alive() { kill -0 "$VIDOW_PID" 2>/dev/null; }

# 一行一条窗口：id 标题 output 字段（NULL 打 (null)）
win_outputs() {
	timeout 5 ./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
for w in json.loads(raw):
    out = w.get("output")
    print("%s %s" % (w.get("title"), out if out is not None else "(null)"))'
}
ws_outputs() {
	timeout 5 ./build/vidowmsg workspaces 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
for w in json.loads(raw):
    out = w.get("output")
    print("out=%s home=%s" % (out if out is not None else "(null)", w.get("home")))'
}
# 输出名只有 ASCII（HEADLESS-1 / WL-1）；出现别的字节 = 野指针读出来的垃圾
outputs_sane() {
	timeout 5 ./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
bad = []
for w in json.loads(raw):
    out = w.get("output")
    if out is not None and not out.replace("-", "").isalnum():
        bad.append(repr(out))
print(" ".join(bad) if bad else "ok")'
}

# ---------------------------------------------------------------- 1) 两块屏
mark "1/2 两块屏：拔掉其中一块，挪过来的窗口不能指着那块已经释放的屏"
printf 'vidow.output{ name = "HEADLESS-1", scale = 1 }\nvidow.output{ name = "HEADLESS-2", scale = 1 }\nwindow_close_ms = 0\n' \
	> "$CFGDIR/.config/vidow/config.lua"

BIN=${VIDOW_BIN:-./build/vidow}
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 VIDOW_LOG=debug \
	VIDOW_TEST_OUTPUTS="9000:remove:HEADLESS-1,16000:add:1280x720:HEADLESS-1" \
	setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-lastoutput.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5
	exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

for t in w1 w2; do
	setsid nohup python3 ./tools/pattern.py "$t" > "$RUNDIR/$t.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 1.4
done
# 一个窗口放一块屏上：HEADLESS-1 是左边那块，新窗口开在焦点屏上 —— 直接把 w1
# 搬到另一块屏，保证两边各一个。
timeout 5 ./build/vidowmsg move-window-to-output HEADLESS-1 --title w1 >/dev/null 2>&1
sleep 1
BEFORE=$(win_outputs)
echo "  拔之前：$(echo "$BEFORE" | tr '\n' ' ')"
if [ "$(echo "$BEFORE" | awk '{print $2}' | sort -u | wc -l)" = 2 ]; then
	pass "两个窗口分别住在两块屏上（$(echo "$BEFORE" | tr '\n' ' ')）"
else
	fail "两个窗口没分到两块屏上：$BEFORE"
fi

# 等钩子把 HEADLESS-1 拔掉
for _ in $(seq 1 40); do
	grep -aq "hotplug: 拔掉 HEADLESS-1" "$LOG" && break
	sleep 0.25
done
sleep 1
if ! alive; then
	fail "拔掉一块屏之后 vidow 没了（崩了）"
else
	pass "拔掉一块屏之后 vidow 还活着"
fi
SANITY=$(outputs_sane)
if [ "$SANITY" = "ok" ]; then
	pass "每个窗口的 output 都是正常的输出名（没有野指针读出来的乱码）"
else
	fail "有窗口的 output 是乱码（野指针）：$SANITY"
fi
AFTER=$(win_outputs)
if echo "$AFTER" | awk '{print $2}' | grep -qv "^HEADLESS-2$"; then
	fail "拔掉 HEADLESS-1 之后还有窗口不指着 HEADLESS-2：$AFTER"
else
	pass "两个窗口都挪到了活着的 HEADLESS-2 上：$(echo "$AFTER" | tr '\n' ' ')"
fi

# 插回来：原来那块屏上的窗口要按 home 认领回去
for _ in $(seq 1 60); do
	grep -aq "test: 插上" "$LOG" && break
	sleep 0.25
done
sleep 2
BACK=$(win_outputs)
if echo "$BACK" | grep -q "^w1 HEADLESS-1$"; then
	pass "HEADLESS-1 插回来之后 w1 认领回自己那块屏（$BACK 里）"
else
	fail "插回来之后 w1 没回到 HEADLESS-1：$BACK"
fi
alive && pass "热插拔一整轮之后 vidow 还活着" || fail "热插拔过程里 vidow 没了"

kill -TERM -- "-$VIDOW_PID" 2>/dev/null
sleep 0.6
kill -KILL -- "-$VIDOW_PID" 2>/dev/null
for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
CLIENT_PIDS=""
VIDOW_PID=""
kill_strays
sleep 0.5

# ------------------------------------------------- 2) 嵌套会话：最后一块屏没了
mark "2/2 嵌套会话：外层窗口被关掉（最后一块屏没了）—— 不许留野指针"
cat > "$RUNDIR/nest-home/.config/vidow/config.lua" <<'CFG'
window_close_ms = 0
xwayland = true
CFG
printf 'vidow.output{ name = "HEADLESS-1", scale = 1 }\nwindow_close_ms = 0\n' \
	> "$CFGDIR/.config/vidow/config.lua"

VIDOW_LOG=debug WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
	setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

HOME="$RUNDIR/nest-home" WLR_BACKENDS=wayland VIDOW_LOG=debug \
	setsid "$BIN" > "$RUNDIR/nest.log" 2>&1 &
NEST_PID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$RUNDIR/nest.log" && break; sleep 0.25; done
NEST_WD=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$RUNDIR/nest.log" |
	tail -1 | cut -d= -f2)
if [ -z "$NEST_WD" ]; then
	fail "嵌套的 vidow 没起来"
else
	pass "嵌套的 vidow 起来了（WAYLAND_DISPLAY=$NEST_WD）"
fi

HOME="$RUNDIR/nest-home" WAYLAND_DISPLAY="$NEST_WD" setsid nohup \
	python3 ./tools/pattern.py n1 > "$RUNDIR/n1.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 2
# 外层（A）那边把嵌套会话的那个窗口关掉：它的输出就没了
OUTER_WIN=$(timeout 5 ./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
ws = json.loads(raw)
print(max(w["id"] for w in ws) if ws else "")')
if [ -n "$OUTER_WIN" ]; then
	timeout 5 ./build/vidowmsg close-window --id "$OUTER_WIN" >/dev/null 2>&1
	pass "外层那个窗口（id=$OUTER_WIN）关掉了"
else
	fail "外层的窗口列表是空的，没法模拟「最后一块屏没了」"
fi
sleep 2

if ! kill -0 "$NEST_PID" 2>/dev/null; then
	fail "最后一块屏没了之后嵌套的 vidow 没了（崩了）"
else
	pass "最后一块屏没了之后 vidow 还活着"
fi
NOUT=$(HOME="$RUNDIR/nest-home" WAYLAND_DISPLAY="$NEST_WD" \
	timeout 5 ./build/vidowmsg outputs 2>/dev/null)
if [ "$(echo "$NOUT" | tr -d ' \n')" = "[]" ]; then
	pass "输出列表空了（一块屏都没有了）"
else
	fail "输出列表不是空的：$NOUT"
fi
NSANITY=$(HOME="$RUNDIR/nest-home" WAYLAND_DISPLAY="$NEST_WD" \
	timeout 5 ./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
bad = [repr(w.get("output")) for w in json.loads(raw)
       if w.get("output") is not None and
       not w["output"].replace("-", "").isalnum()]
print(" ".join(bad) if bad else "ok")')
if [ "$NSANITY" = "ok" ]; then
	pass "嵌套会话里每个窗口的 output 都是 null（不是野指针）"
else
	fail "嵌套会话里有窗口的 output 是乱码（野指针）：$NSANITY"
fi
NWS=$(HOME="$RUNDIR/nest-home" WAYLAND_DISPLAY="$NEST_WD" \
	timeout 5 ./build/vidowmsg workspaces 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
ws = json.loads(raw)
print("out_null=%s home_ok=%s" % (
    all(w.get("output") is None for w in ws),
    all(w.get("home") == "WL-1" for w in ws)))')
if [ "$NWS" = "out_null=True home_ok=True" ]; then
	pass "工作区 output 都是 null，但 home 还记得那块屏（WL-1）"
else
	fail "工作区的 output/home 不对：$NWS"
fi
NMEM=$(HOME="$RUNDIR/nest-home" WAYLAND_DISPLAY="$NEST_WD" \
	timeout 5 ./build/vidowmsg memories 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
ms = json.loads(raw)
print("ok" if any(m.get("key") == "WL-1" and m.get("present") is False
                 for m in ms) else "bad")')
if [ "$NMEM" = "ok" ]; then
	pass "记忆里 WL-1 还在（present=false）：插回来还能认领"
else
	fail "记忆里没有 WL-1 那条（屏回来就认不回来了）"
fi

# 这种状态下有 X 客户端连上来：日志要明说是「一块屏都没有」
NEST_DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$RUNDIR/nest.log" | tail -1 | cut -d= -f2)
if [ -n "$NEST_DISPLAY" ]; then
	DISPLAY="$NEST_DISPLAY" setsid nohup xev -geometry 100x100 \
		> "$RUNDIR/xev.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 24); do
		grep -aq "一块屏都没有" "$RUNDIR/nest.log" && break
		sleep 0.25
	done
	if grep -aq "一块屏都没有" "$RUNDIR/nest.log"; then
		pass "X 客户端连上来时日志明说了「一块屏都没有 → X 屏幕 0x0」"
	else
		fail "X 屏幕 0x0 的时候日志没说明白（用户查不到原因）"
	fi
	# 这块 X 客户端的窗口**照样会 map**（XWayland 还活着），vidow 必须收下它 ——
	# 以前这里直接解引用空工作区（view.c 的 view_ensure_column），一 map 就
	# SEGV：用户报的「打开 kate 直接崩」就是这条（嵌套会话的外层窗口关掉之后
	# 再开任何窗口都撞得上）。这条断言就在那个当口查，别等收尾。
	NWIN_BEFORE=1
	for _ in $(seq 1 24); do
		NWIN=$(HOME="$RUNDIR/nest-home" WAYLAND_DISPLAY="$NEST_WD" \
			timeout 5 ./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
try:
    print(len(json.loads(raw)))
except Exception:
    print(-1)')
		[ "$NWIN" -gt "$NWIN_BEFORE" ] 2>/dev/null && break
		sleep 0.25
	done
	if ! kill -0 "$NEST_PID" 2>/dev/null; then
		fail "没屏的时候 X 客户端 map 上来，vidow 崩了（空工作区被解引用）"
	elif [ "${NWIN:-0}" -gt "$NWIN_BEFORE" ] 2>/dev/null; then
		pass "没屏的时候 map 上来的窗口被收下了（窗口数 $NWIN_BEFORE → $NWIN）"
	else
		fail "没屏时 map 上来的窗口没进列表（窗口数 $NWIN）"
	fi
else
	fail "嵌套会话没有 DISPLAY，XWayland 没起来"
fi
kill -0 "$NEST_PID" 2>/dev/null &&
	pass "XWayland 在这状态下起来又走了一遭，vidow 还在" ||
	fail "XWayland 那条路把 vidow 带崩了"

# ------------------------------------------------- 3/3 没屏时开窗 → 屏回来
mark "3/3 一块屏都没有的时候开新窗口：屏回来窗口跟着回来"
printf 'window_close_ms = 0\nxwayland = true\n' \
	> "$CFGDIR/.config/vidow/config.lua"
# 先 add 一块（headless 后端得先「认下来」，全拔光之后就没法再插了），
# 再把三块全拔掉 —— 这时候 server->outputs 是空的；最后插回来一块
# （名字用「上次拔掉那块」，模拟同一根线又插上了）。
VIDOW_LOG=debug WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 \
	VIDOW_TEST_OUTPUTS="1000:add:1280x720:HEADLESS-3,9000:remove:HEADLESS-1,10000:remove:HEADLESS-2,11000:remove:HEADLESS-3,16000:add:1280x720:last" \
	setsid "$BIN" > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-lastoutput.sh: vidow 没起来"; grep -a "ERROR" "$LOG" | head -5
	exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 2
setsid nohup python3 ./tools/pattern.py w1 > "$RUNDIR/w1.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.5
for _ in $(seq 1 60); do grep -aq "拔掉输出 HEADLESS-3" "$LOG" && break; sleep 0.5; done
sleep 1
NOUT=$(timeout 5 ./build/vidowmsg outputs 2>/dev/null)
if [ "$(echo "$NOUT" | tr -d ' \n')" = "[]" ]; then
	pass "三块屏全拔光（输出列表空了）"
else
	fail "输出列表不是空的：$NOUT"
fi
# 没屏的时候开一个新窗口（Wayland 一个、X11 一个）
setsid nohup python3 ./tools/pattern.py w2 > "$RUNDIR/w2.log" 2>&1 </dev/null &
CLIENT_PIDS="$CLIENT_PIDS $!"
if [ -n "${DISPLAY_NOW:-}" ]; then :; fi
sleep 3
if ! alive; then
	fail "没屏的时候开新窗口把 vidow 带崩了"
else
	pass "没屏的时候开新窗口：vidow 还活着"
fi
NWIN=$(timeout 5 ./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
print(len(json.loads(raw)))')
if [ "$NWIN" = "2" ]; then
	pass "两个窗口都被收下了（挂在现在没屏的工作区上）"
else
	fail "没屏时开的新窗口没进列表（窗口数 $NWIN）"
fi
# 屏插回来：工作区按名字认领回去，窗口跟着回来（output 不再是 null）
for _ in $(seq 1 60); do grep -aq "test: 插上" "$LOG" && break; sleep 0.5; done
sleep 3
if ! alive; then
	fail "屏插回来之后 vidow 没了"
else
	pass "屏插回来之后 vidow 还活着"
fi
BACK=$(timeout 5 ./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
raw = sys.stdin.buffer.read().decode("utf-8", "replace")
outs = sorted({w.get("output") for w in json.loads(raw)})
print(",".join(str(o) for o in outs))')
if [ "$BACK" = "HEADLESS-3" ]; then
	pass "插回来之后每个窗口都归到 HEADLESS-3 上了（没屏期间收下的也在）"
else
	fail "插回来之后窗口的 output 是「$BACK」，期望 HEADLESS-3（窗口没跟着工作区回来）"
fi
kill -TERM -- "-$VIDOW_PID" 2>/dev/null
sleep 0.6
kill -KILL -- "-$VIDOW_PID" 2>/dev/null
VIDOW_PID=""
for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
CLIENT_PIDS=""
kill_strays
sleep 0.5
LEFT=$(our_vidow_pids | wc -l)
if [ "$LEFT" = "0" ]; then
	pass "收尾干净：本测试起的 vidow 一个不剩"
else
	fail "收尾还剩 $LEFT 个 vidow（$(our_vidow_pids | tr '\n' ' ')）"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "test-lastoutput.sh: 全部通过"
else
	echo "test-lastoutput.sh: 有失败项"
	echo
	echo "--- 日志里的热插拔轨迹 ---"
	grep -aE "hotplug|output .*: |一块屏都没有|test: " "$LOG" "$RUNDIR/nest.log" |
		tail -15
fi
exit $FAILED
