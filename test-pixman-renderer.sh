#!/bin/bash
# 渲染器不是 GLES2（软件兜底的 pixman，或 WLR_RENDERER=pixman）时，vidow 必须活着。
#
# 靶子是 core dump 里那条 abort：
#   vidow: render/gles2/renderer.c:38: gles2_get_renderer:
#          Assertion `wlr_renderer_is_gles2(wlr_renderer)' failed.
#   栈：#4 __assert_fail  #5 libwlroots+0x224c4  #6 libwlroots+0x2255f
#       #7 gl_make_current  #8 vidow_fx_flush  #9 vidow_output_render  #10 output_frame
#
# 机制：wlr_gles2_renderer_get_egl() 第一行就是那条断言 —— 渲染器不是 GLES2 时它
# **不是返回 NULL，是当场 abort**。vidow_fx_flush()（一帧一次的 GL 提交）原来是
# 无条件调的，于是「软件渲染兜底」这种完全合法的情况下，第一帧就 abort。
# 装饰 / 模糊那条路本来就在 shader_ok 底下（渲染器不是 GLES2 时它直接不跑），
# 只有这个新加的每帧提交漏了。
#
# 判据：
#   1) 前提：vidow 真的在用 pixman（日志有「Creating pixman renderer」）
#   2) 它自己认出「渲染器不是 GLES2」，关掉装饰 / 模糊（日志那行 ERROR）
#   3) 进程活着（不是 abort / 段错误），日志里没有 Assertion
#   4) 窗口照样上屏（像素级：客户端那块颜色在）—— 关掉装饰不等于不渲染
#   5) 全程没有核心转储（这条是「回退代码必红」的那一条）
#
# 用法：./test-pixman-renderer.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-pixman-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in *bluewin.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v grim >/dev/null || { echo "test-pixman-renderer.sh: 需要 grim"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
blur_enabled = true
window_open_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER=pixman
export HOME="$CFGDIR" VIDOW_LOG=debug DISPLAY= WAYLAND_DISPLAY=

mark "软件渲染器（pixman）下，vidow 必须活着"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
for _ in $(seq 1 40); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$LOG"; then
	fail "vidow 没起来"
	grep -aE "Assertion|assert|abort" "$LOG" | head -3 | sed 's/^/    /'
	exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)

# 一个纯蓝的普通窗口：关掉装饰之后客户区照样得上屏
setsid nohup python3 ./tools/bluewin.py > "$RUNDIR/blue.log" 2>&1 </dev/null &
sleep 2.5
grim -o HEADLESS-1 "$RUNDIR/shot.png" 2>/dev/null
# 再让它多跑一会儿：崩溃发生在「第一帧之后的某一帧」也一样抓得到
sleep 2.0
alive=0
kill -0 "$VIDOW_PID" 2>/dev/null && alive=1

mark "断言"
grep -a "Creating pixman renderer" "$LOG" | head -1 | sed 's/^/    /'
grep -a "renderer is not GLES2" "$LOG" | head -1 | sed 's/^/    /'

[ "$(grep -ac 'Creating pixman renderer' "$LOG")" -ge 1 ] &&
	pass "前提：vidow 真的在用 pixman 渲染器" ||
	fail "前提：日志里没有「Creating pixman renderer」"

[ "$(grep -ac 'renderer is not GLES2' "$LOG")" -ge 1 ] &&
	pass "它自己认出「渲染器不是 GLES2」，关掉装饰 / 模糊" ||
	fail "没有「renderer is not GLES2」那行"

[ "$alive" = 1 ] && pass "进程活着（没 abort）" || fail "进程死了"

if grep -aq "Assertion" "$LOG"; then
	fail "日志里有 Assertion：$(grep -a 'Assertion' "$LOG" | head -1)"
else
	pass "日志里没有 Assertion"
fi

python3 - "$RUNDIR/shot.png" <<'PY'
import sys
from PIL import Image
try:
    im = Image.open(sys.argv[1]).convert("RGB")
except OSError as e:
    print("  FAIL  读截图失败：%s" % e)
    sys.exit(1)
blue = sum(1 for p in im.getdata() if p[2] > 150 and p[0] < 90 and p[1] < 90)
ok = blue > 10000
print(("  PASS  " if ok else "  FAIL  ") +
      "窗口照样上屏（纯蓝像素 %d 个）" % blue)
sys.exit(0 if ok else 1)
PY
[ $? -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "test-pixman-renderer.sh: OK"
else
	echo "test-pixman-renderer.sh: 有失败项"
fi
exit $FAILED
