#!/bin/bash
# 窗口**滚出视口**之后，录屏 / 抓帧那一路仍然算在「窗口自己那块屏」上。
#
# 判据在 wincapture.c 的 vidow_win_grab_output（portal 窗口流和窗口抓帧源共用这一份）：
#   平铺的窗口   → 它那条列挂在哪块屏（view->column->output）
#   浮动 / 弹窗  → 现在画在哪块屏上（反查布局；浮动 / 弹窗不参与滚动）
#
# 为什么平铺的不能按「现在画在哪」反查：平铺窗口的 geom 里含着**滚动视口的偏移**
# （view.c 的 output_arrange：x = 工作区 + 列偏移 - 视口）。窗口滚出视口之后，它画
# 的那些像素落在隔壁屏的范围里，按中心反查就查出隔壁那块 —— 这一路流（和抓帧源）
# 就挂到错那块屏上，后果三件（用户报的「非 0 那块屏上录屏看不到光标」，实测）：
#   ① 光标去错那块屏的光标列表里找：指针明明在窗口上，也永远找不到，元数据一直
#      id=0 —— 消费者（OBS）那边就是「有画面、光标不进来」；
#   ② 流尺寸乘的是错那块屏的 scale（2 倍屏上分辨率只有一半，图是糊的）；
#   ③ 输出提交那条抓帧路盯的也是错那块屏。
# 窗口滚回视口、重新对齐到自己那块屏之后，**已经建好的流不会跟着换回来**。
#
# 这条用窗口抓帧源（ext-image-capture，build/capture）测，不碰 portal —— 尺寸就是
# 那个判据的直接产物（窗口逻辑尺寸 × 这块屏的 scale）。判定方式跟列顺序无关：
#   1) 四个窗口铺在 HEADLESS-2（缩放 2）上，列宽 0.35 让条带装不下（总宽 ~1790 > 可见 1268）
#   2) 量一次基准：把窗口挪到 HEADLESS-1（缩放 1）抓一次 → S1 = 逻辑尺寸 × 1
#   3) 挪回 HEADLESS-2，逐列聚焦，找一个「列在 HEADLESS-2 上、画出来的中心却落在
#      HEADLESS-1 的范围（0..1280）里」的窗口 —— 就是滚出视口之后那个
#   4) 抓它：尺寸必须是 S1 × 2（它那块屏的缩放）。修之前这里是 S1（乘了隔壁 1 倍屏）
#   5) 再把它聚焦回视口里抓一次：两次尺寸必须一样
#   6) 滚出视口那个窗口：合成器把客户端的 preferred_buffer_scale 钉在**它那块屏**的
#      缩放上（日志「钉住 preferred_buffer_scale = 2」，且之后不出现 = 1），并且窗口
#      表面树**真的裁到了自己那块屏里** —— wlroots 的 wlr_scene_surface 是按「可见面积
#      最大的那块输出」发 preferred_buffer_scale 的，不裁的话隔壁那块屏会赢：客户端被
#      叫去按它的缩放重画，2 倍屏上录 / 预览这个窗口就一直半分辨率糊着，滚回视口才清楚
#      （用户报的「窗口滚动出去后，又按照另一个屏幕的缩放变化了」）。
#      （客户端真按 1 倍重画这件事要真 DRM 会话才看得见，headless 这边钉我们发出去的
#      那个值 + 表面树确实裁了。）
#   7) 全程 vidow 没崩、没断言
#
# 用法：./test-win-scrolled-output.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-win-scrolled-output-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""
START_TS=$(date +%s)

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*build/capture*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

cleanup() {
	stop_clients
	for pid in $CLIENT_PIDS; do
		[ -n "$pid" ] || continue
		kill "$pid" 2>/dev/null
	done
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/vidow ] || { echo "test-win-scrolled-output.sh: 先构建（ninja -C build）"; exit 1; }
[ -x ./build/capture ] || { echo "test-win-scrolled-output.sh: 先构建（ninja -C build）"; exit 1; }
[ -x ./build/vidowmsg ] || { echo "test-win-scrolled-output.sh: 先构建（ninja -C build）"; exit 1; }
python3 -c 'import gi' 2>/dev/null || { echo "test-win-scrolled-output.sh: 需要 python3-gobject"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 两块屏位置钉死、缩放不同：这就是用户那台机器的几何（一块 4K 缩放 2 摆在 x=1280，
# 一块 1 倍屏在 0,0）。谁在 0 必须钉死 —— 这一条测的就是「滚出去之后落在 0 那块屏
# 的范围里，但这一路流不该跟着它走」。
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", x = 0, y = 0, scale = 1, mode = "1280x720" }
vidow.output{ name = "HEADLESS-2", x = 1280, y = 0, scale = 2, mode = "2560x1440" }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=info
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_RENDERER=pixman
export DISPLAY= WAYLAND_DISPLAY=
unset VIDOW_TEST_OUTPUTS VIDOW_TEST_ACTION

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG"; then
	echo "test-win-scrolled-output.sh: vidow 起不来"
	grep -a "ERROR" "$LOG" | head -5
	exit 1
fi
SOCK=$(grep -a "vidow ready" "$LOG" | grep -ao "wayland-[0-9]*" | head -1)
export WAYLAND_DISPLAY="$SOCK"
IPC="$XDG_RUNTIME_DIR/vidow-$SOCK.sock"
vm() { ./build/vidowmsg --socket "$IPC" --timeout 5 "$@"; }
alive() { [ -n "$VIDOW_PGID" ] && kill -0 "$VIDOW_PGID" 2>/dev/null; }

# 某个标题的窗口：x y w h output
wgeo() {
	vm windows 2>/dev/null | python3 -c '
import json,sys
want = sys.argv[1]
try:
    ws = json.load(sys.stdin)
except Exception:
    sys.exit(0)
if isinstance(ws, dict):
    ws = ws.get("windows", [])
for w in ws:
    if (w.get("title") or "") == want:
        g = w["geometry"]
        print("%d %d %d %d %s %s" % (g["x"], g["y"], g["width"], g["height"],
            w.get("output") or "", w.get("id") or ""))
        break
' "$1"
}

# 抓一路窗口（$1 标题，$2 秒数），回它拿到的第一个 buffer_size（"WxH"）。
cap_size() {
	timeout 60 ./build/capture "$1" "$2" >"$RUNDIR/cap-$1-$3.txt" 2>&1
	grep -a '^capture: buffer_size' "$RUNDIR/cap-$1-$3.txt" | head -1 | awk '{print $3}'
}
cap_frames() { grep -ac '^capture [0-9]' "$RUNDIR/cap-$1-$2.txt" 2>/dev/null; }
# "WxH" 的宽 × 2（整数，抓帧尺寸就是 lround(逻辑宽 × scale)）。
twice() { echo $(( ${1%x*} * 2 ))x$(( ${1#*x} * 2 )); }

# --- 1 四个窗口铺在 HEADLESS-2 上 -------------------------------------------
mark "1) 四个窗口铺在 HEADLESS-2（缩放 2）上，列宽 0.35 让条带装不下"
TITLES="w1 w2 w3 w4"
for t in $TITLES; do
	python3 tools/pattern.py "$t" >/dev/null 2>&1 &
	CLIENT_PIDS="$CLIENT_PIDS $!"
done
for _ in $(seq 1 60); do
	[ "$(vm windows 2>/dev/null | grep -ac '"title": "w')" -ge 4 ] && break
	sleep 0.25
done
NWIN=$(vm windows 2>/dev/null | grep -ac '"title": "w')
if [ "$NWIN" -ge 4 ]; then
	pass "四个窗口都在（$NWIN 个）"
else
	fail "窗口没起全（$NWIN 个）"
fi
for t in $TITLES; do
	vm move-window-to-output HEADLESS-2 --title "$t" >/dev/null 2>&1
	sleep 0.4
done
sleep 1
for t in $TITLES; do
	vm set-column-width 0.35 --title "$t" >/dev/null 2>&1
done
sleep 1.5
ON2=0
for t in $TITLES; do
	set -- $(wgeo "$t")
	[ "${5:-}" = "HEADLESS-2" ] && ON2=$((ON2 + 1))
done
if [ "$ON2" -ge 4 ]; then
	pass "四个窗口的列都在 HEADLESS-2 上"
else
	fail "只有 $ON2 个窗口的列在 HEADLESS-2 上（挪屏没生效）"
fi

# --- 2 量基准：1 倍屏上的抓帧尺寸 -------------------------------------------
mark "2) 基准：把 w1 挪到 HEADLESS-1（缩放 1）抓一次"
vm move-window-to-output HEADLESS-1 --title w1 >/dev/null 2>&1
sleep 1.5
set -- $(wgeo w1)
if [ "${5:-}" = "HEADLESS-1" ]; then
	pass "w1 在 HEADLESS-1 上（列在 ${5}，画在 $1 起）"
else
	fail "w1 没挪到 HEADLESS-1（列在 ${5:-?}）"
fi
S1=$(cap_size w1 6 base)
NF1=$(cap_frames w1 base)
echo "      1 倍屏上的抓帧尺寸：${S1:-读不出来}（${NF1:-0} 帧）"
if [ -n "$S1" ]; then
	pass "1 倍屏上拿到了 buffer_size（$S1）"
else
	fail "1 倍屏上没拿到 buffer_size"
fi
# 挪回 HEADLESS-2：条带又装不下了，滚动回来
vm move-window-to-output HEADLESS-2 --title w1 >/dev/null 2>&1
sleep 1.5

# --- 3 找一个滚出视口、画到 HEADLESS-1 范围里的窗口 -------------------------
mark "3) 找一个滚出视口、画到 HEADLESS-1 范围里的窗口"
# 聚焦哪一列，视口就滚到让它整列可见（niri 式最小滚动）。总宽 ~1790 > 可见 1268，
# 所以聚焦靠后的列时，靠前的列会被推到 HEADLESS-2 左边 —— 也就是 HEADLESS-1 的
# 布局范围（0..1280）里。判据就是「列在 HEADLESS-2 上，画出来的中心却 < 1280」。
TARGET=""
TGEO=""
TCX=""
for t in $TITLES; do
	vm focus-window --title "$t" >/dev/null 2>&1
	sleep 1.2
	for u in $TITLES; do
		set -- $(wgeo "$u")
		[ "$#" -ge 5 ] || continue
		[ "$5" = "HEADLESS-2" ] || continue
		cx=$(( $1 + $3 / 2 ))
		if [ "$cx" -lt 1280 ] && [ "$cx" -ge 0 ]; then
			TARGET="$u"
			TGEO="$1 $2 $3 $4 $5"
			TCX="$cx"
			break
		fi
	done
	[ -n "$TARGET" ] && break
done
if [ -n "$TARGET" ]; then
	pass "「$TARGET」滚出视口了：列还在 HEADLESS-2 上，画在 $(( ${TGEO%% *} )) 起（中心 $TCX < 1280，落在 HEADLESS-1 的范围里）"
else
	fail "没有窗口滚出视口（四列装得下？）—— 这一条测不到"
fi

# --- 4 滚出去抓一次：尺寸得按窗口那块屏的缩放算 ------------------------------
mark "4) 滚出视口抓帧：尺寸得按窗口自己那块屏（HEADLESS-2，缩放 2）算"
S_OUT=""
if [ -n "$TARGET" ]; then
	S_OUT=$(cap_size "$TARGET" 6 out)
	NF=$(cap_frames "$TARGET" out)
	echo "      滚出视口时的抓帧尺寸：${S_OUT:-读不出来}（${NF:-0} 帧；1 倍屏上是 ${S1:-?}）"
	if [ -n "$S_OUT" ]; then
		pass "滚出视口时拿到了 buffer_size（$S_OUT）"
	else
		fail "滚出视口时没拿到 buffer_size（滚出视口的窗口也得能抓 —— 取景器是私有场景，跟它在屏上摆哪无关）"
	fi
	if [ "${NF:-0}" -ge 10 ]; then
		pass "滚出视口照样出帧（$NF 帧）"
	else
		fail "滚出视口只出了 ${NF:-0} 帧"
	fi
	if [ -n "$S1" ] && [ "$S_OUT" = "$(twice "$S1")" ]; then
		pass "尺寸按窗口那块屏的缩放算（$S_OUT = $S1 × 2）"
	else
		fail "尺寸没按窗口那块屏算：$S_OUT，期望 $(twice "${S1:-0x0}")（乘了隔壁 1 倍屏的 scale 就是 $S1）"
	fi
else
	fail "没有滚出视口的窗口，第 4 节跳过"
fi

# --- 4b 客户端按几倍渲染：钉在它那块屏的缩放上 ------------------------------
mark "4b) 滚出视口时，客户端的 preferred_buffer_scale 钉在窗口那块屏（缩放 2）上"
if [ -n "$TARGET" ]; then
	# 钉是在第一次摆放时发生的，那会儿标题还没来 —— 按窗口编号（IPC 的 id）找。
	WID=$(vm windows 2>/dev/null | python3 -c '
import json,sys
want = sys.argv[1]
try:
    ws = json.load(sys.stdin)
except Exception:
    sys.exit(0)
if isinstance(ws, dict):
    ws = ws.get("windows", [])
for w in ws:
    if (w.get("title") or "") == want:
        print(w.get("id") or "")
        break
' "$TARGET")
	PIN=$(grep -a "surface ${WID:-?} .*: 钉住 preferred_buffer_scale" "$LOG" 2>/dev/null | tail -1)
	# 只看「钉到 2 倍之后」还有没有再翻回 1：窗口 map 时先在光标那块 1 倍屏上
	# 出现、随后被 move-window-to-output 搬到 2 倍屏，那一版 1 是当时的真归属，
	# 不是滚出视口后又跟着隔壁屏变。
	PIN_LINE=$(grep -an "surface ${WID:-?} .*: 钉住 preferred_buffer_scale = 2" "$LOG" 2>/dev/null | tail -1 | cut -d: -f1)
	PIN_BAD=""
	if [ -n "${PIN_LINE:-}" ]; then
		PIN_BAD=$(tail -n +$((PIN_LINE + 1)) "$LOG" 2>/dev/null | grep -a "surface ${WID:-?} .*: 钉住 preferred_buffer_scale = 1" | tail -1)
	fi
	if echo "${PIN:-}" | grep -q "= 2"; then
		pass "钉住了 2 倍（$(echo "$PIN" | sed 's/.*钉住/钉住/' | cut -c1-40)…）"
	else
		fail "没钉住 2 倍（日志里：${PIN:-没这行}）—— 客户端会被叫去按隔壁 1 倍屏渲染"
	fi
	if [ -z "${PIN_BAD:-}" ]; then
		pass "滚出视口之后没有出现「= 1」的钉法（不跟着隔壁那块屏变）"
	else
		fail "出现了一版「= 1」的钉法：$PIN_BAD"
	fi
	if grep -aq "clip: $TARGET 裁到自己那块屏里" "$LOG" 2>/dev/null; then
		pass "窗口表面树裁到自己那块屏里了（「可见面积」不会算到隔壁屏头上）"
	else
		fail "日志里没有「clip: $TARGET 裁到自己那块屏里」—— 可见面积算到隔壁屏上了"
	fi
else
	fail "没有滚出视口的窗口，第 4b 节跳过"
fi

# --- 5 滚回视口再抓一次：两次必须一样 ---------------------------------------
mark "5) 把同一个窗口聚焦回视口里再抓一次：两次尺寸必须一样"
S_IN=""
if [ -n "$TARGET" ]; then
	vm focus-window --title "$TARGET" >/dev/null 2>&1
	sleep 1.5
	set -- $(wgeo "$TARGET")
	cx=$(( $1 + $3 / 2 ))
	if [ "${5:-}" = "HEADLESS-2" ] && [ "$cx" -ge 1280 ]; then
		pass "「$TARGET」滚回视口里了（画在 $1 起，中心 $cx）"
	else
		fail "「$TARGET」没滚回视口（画在 $1 起，中心 $cx，列在 ${5:-?}）"
	fi
	S_IN=$(cap_size "$TARGET" 6 in)
	echo "      滚回视口后的抓帧尺寸：${S_IN:-读不出来}"
	if [ -n "$S_IN" ]; then
		pass "滚回视口后拿到了 buffer_size（$S_IN）"
	else
		fail "滚回视口后没拿到 buffer_size"
	fi
	if [ -n "$S_OUT" ] && [ "$S_OUT" = "$S_IN" ]; then
		pass "滚出去 / 滚回来两次尺寸一样（$S_OUT）—— 这一路算的是窗口那块屏，跟滚动无关"
	else
		fail "两次尺寸不一样（滚出去 ${S_OUT:-?}，滚回来 ${S_IN:-?}）—— 滚出视口时挂到了隔壁屏上"
	fi
else
	fail "没有滚出视口的窗口，第 5 节跳过"
fi

# --- 6 没崩、没断言 ---------------------------------------------------------
mark "6) 没崩、没断言"
if alive; then
	pass "vidow 还活着"
else
	fail "vidow 死了"
fi
if grep -aqE "assert|abort|Assertion" "$LOG"; then
	fail "日志里有 assert / abort：$(grep -aE 'assert|abort' "$LOG" | head -1)"
else
	pass "日志里没有 assert / abort"
fi

mark "结果"
if [ "$FAILED" -eq 0 ]; then
	echo "全过（用了 $(( $(date +%s) - START_TS )) 秒）"
else
	echo "有失败（用了 $(( $(date +%s) - START_TS )) 秒）；日志：$LOG"
fi
exit "$FAILED"
