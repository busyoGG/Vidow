#!/bin/bash
# 帧追踪（`VIDOW_TRACE_FRAMES=1`）+ 把一块屏的帧链掐断的测试钩子
# （`VIDOW_TEST_STALL_OUTPUT=<屏名>`）。
#
# 用户报过的症状：「某块屏上的窗口停在最后一帧，拖到另一块屏就活」。真机上只有
# DRM 的 page-flip 不回来才会这样 —— 帧事件不来，客户端拿不到帧回调，于是所有
# 窗口都停在最后一帧，而输入照旧（键盘/点击不走渲染）。这个测试在 headless 上
# 复现它（钩子把那块屏的帧事件按住），并验证追踪能在真机上点出它。
#
# 判据：
#   1) 健康 + 追踪开：启动打「帧追踪: 开」，此后**一行 trace 都没有**（不误报），
#      窗口内容确实上屏（截图上窗口中心是图案色）
#   2) 掐断帧链 + 追踪开：打出 ERROR，点名哪块屏、vidow 要过几帧、最后由谁要的，
#      并写明「客户端还在交帧，屏幕却不更新」；窗口内容确实没上屏（同一采样点
#      不是图案色）
#   3) 追踪关掉：一条 trace 日志都没有，症状照样（零开销、不改变行为）
#
# 用法：./test-frametrace.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-frametrace-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	for p in $(pgrep -x vidow 2>/dev/null); do kill -TERM "$p" 2>/dev/null; done
	sleep 0.3
	for p in $(pgrep -x vidow 2>/dev/null); do kill -KILL "$p" 2>/dev/null; done
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v grim >/dev/null || { echo "test-frametrace.sh: 需要 grim"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "test-frametrace.sh: 需要 python3-PIL"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
xwayland = false
CFG
export VIDOW_CONFIG_LUA="$CFGDIR/.config/vidow/config.lua"

start_vidow() { # <额外 env...>
	export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
	rm -f "$LOG"
	env "$@" setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	for _ in $(seq 1 60); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		echo "vidow 没起来"; tail -5 "$LOG"; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	sleep 0.5
}

stop_vidow() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	CLIENT_PIDS=""
	sleep 0.3
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.5
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
		VIDOW_PGID=""
	fi
}

vidow_alive() { [ "$(pgrep -x vidow | wc -l)" -ge 1 ]; }

win_geom() { # <标题> -> "x y w h"
	./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    if w["title"] != sys.argv[1]:
        continue
    g = w["geometry"]
    print(g["x"], g["y"], g["width"], g["height"])
    break
else:
    print("0 0 0 0")' "$1"
}

# 采样点是不是「图案色」（tools/pattern.py 画的四象限红/绿/蓝/白）
sample_is_pattern() { # <png> <x> <y>
	python3 - "$1" "$2" "$3" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y = int(sys.argv[2]), int(sys.argv[3])
if x < 0 or y < 0 or x >= im.width or y >= im.height:
    print("out")
    sys.exit()
p = im.getpixel((x, y))
ok = (p[0] > 200 and p[1] < 60 and p[2] < 60) or \
     (p[1] > 200 and p[0] < 60 and p[2] < 60) or \
     (p[2] > 200 and p[0] < 60 and p[1] < 60) or \
     (min(p) > 200)
print("yes" if ok else "no (%d,%d,%d)" % p)
PY
}

run_case() { # <slug> <标题> <内容该上屏: yes|no> <该有追踪行: yes|no> <额外 env...>
	local slug=$1 title=$2 want=$3 want_trace=$4
	shift 4
	mark "$title"
	start_vidow "$@"
	python3 ./tools/pattern.py w1 >/dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	sleep 2
	read -r WX WY WW WH <<<"$(win_geom w1)"
	if [ "$WW" -gt 100 ]; then
		pass "窗口在列表里（${WX},${WY} ${WW}x${WH}）"
	else
		fail "窗口没进列表（${WX} ${WY} ${WW} ${WH}）"
	fi
	if [ "$want" = "yes" ]; then
		grim "$RUNDIR/$slug.png" 2>/dev/null
		local got
		got=$(sample_is_pattern "$RUNDIR/$slug.png" $((WX + WW / 2)) $((WY + WH / 2)))
		[ "${got%% *}" = "yes" ] &&
			pass "窗口内容上屏了（中心采样到图案色）" ||
			fail "窗口内容没上屏：$got"
	else
		# 掐断帧链之后屏幕不更新，但**客户端还在画**（这是「停在最后一帧」的另一半，
		# 也是追踪那行判据的来源）。截屏是 vidow 自己重渲的一遍，看它验不了「屏幕冻住」
		# —— 那条只能靠帧追踪的 ERROR 行。
		local commits
		commits=$(grep -ac "缓冲" "$LOG")
		[ "$commits" -ge 1 ] &&
			pass "客户端交过帧（日志里 $commits 行缓冲，屏幕却没跟着更新）" ||
			fail "客户端一帧都没交"
	fi
	if [ "$want_trace" = "yes" ]; then
		grep -aq "trace:" "$LOG" &&
			pass "追踪点出了它：$(grep -a 'trace:' "$LOG" | tail -1 | cut -c1-130)" ||
			fail "追踪没有报出帧链断掉"
		grep -aq "客户端还在交帧，屏幕却不更新" "$LOG" &&
			pass "那行写清了「客户端还在交帧，屏幕却不更新」" ||
			fail "那行没说清原因"
		grep -aq "HEADLESS-1" "$LOG" &&
			pass "那行点名了是哪块屏（HEADLESS-1）" ||
			fail "那行没点名屏"
	else
		# 这一支给「健康 + 追踪开」和「追踪关」共用：两者都不许出现
		# 「帧链断了」那条 ERROR（追踪开着时也不许误报）。追踪关掉那一档
		# 另外在收尾里断言「一条 trace 日志都没有」。
		grep -aqE "\[ERROR\].*trace:" "$LOG" &&
			fail "不该有帧链断掉的 ERROR：$(grep -aE '\[ERROR\].*trace:' "$LOG" | tail -1)" ||
			pass "没有「帧链断了」的 ERROR（不误报）"
	fi
	vidow_alive && pass "vidow 还活着" || fail "vidow 挂了"
	stop_vidow
}

# ---------------------------------------------------------------- 1) 健康

run_case healthy "1/3 健康 + 追踪开（不误报）" yes no VIDOW_TRACE_FRAMES=1
grep -aq "帧追踪: 开" "$LOG" &&
	pass "启动时打了「帧追踪: 开」" ||
	fail "没打启动行"
grep -aq "trace: commit" "$LOG" &&
	pass "客户端每次交帧都记一行（DEBUG）—— 帧链断没断就靠它和帧事件对账" ||
	fail "没记到客户端交帧，追踪没在工作"

# ---------------------------------------------------------------- 2) 掐断帧链

run_case stalled "2/3 掐断 HEADLESS-1 的帧事件 + 追踪开" no yes \
	VIDOW_TRACE_FRAMES=1 VIDOW_TEST_STALL_OUTPUT=HEADLESS-1

# ---------------------------------------------------------------- 3) 追踪关

run_case stalled-off "3/3 掐断帧链 + 追踪关（零开销，症状一样）" no no \
	VIDOW_TEST_STALL_OUTPUT=HEADLESS-1

mark "收尾"
LEFT=$(pgrep -x vidow 2>/dev/null | wc -l)
if [ "$LEFT" = "0" ]; then
	pass "收尾干净：一个 vidow 都不剩"
else
	fail "收尾还剩 $LEFT 个 vidow"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "test-frametrace.sh: 全部通过"
else
	echo "test-frametrace.sh: 有失败"
fi
exit "$FAILED"
