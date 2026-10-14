#!/bin/bash
# 会话锁 × 窗口抓帧：锁定期间窗口抓帧**不能**抓到被藏起来的窗口内容。
#
# 为什么单独一条：输出抓帧（grim / 录屏 / portal 输出流）渲染的是 server->scene 那棵
# 正常树，锁上时它整棵关掉，所以天然不泄漏（见 test-sessionlock.sh 的抓帧审计 ①②）。
# 窗口抓帧走的是**另一棵树**（src/wincapture.c 的镜面场景），跟正常树没关系 ——
# 不堵的话锁定期间照样能把被藏窗口的画面抓出来。
#
# 客户端用 build/capture（ext-image-copy-capture 的窗口源，跟 xdg-desktop-portal
# 选择器 / OBS 窗口源同一条路：portal.c 的 capture_request → wincapture.c），
# VIDOW_CAPTURE_DUMP=<path> 让它把最新一帧写成 PPM 好数像素。
#
# 判据：
#   1) 锁前：抓到的帧里有窗口内容（蓝窗像素占比 > 0）—— 基线，证明这条路本来能抓
#   2) 锁定期间：抓到的帧里窗口内容像素 = 0（全是黑 / 透明）
#   3) 解锁后：同一个窗口再抓，窗口内容像素回来（功能没被堵坏）
#   4) 锁定期间抓帧**没失败**（回的是黑帧，不是 failed 风暴）—— 客户端不用换缓冲
#   5) 合成器没崩、日志里没有 assert
#
# 用法：./test-wincapture-lock.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-wincapture-lock-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENTS=""
LOCK_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	exec 9>&- 2>/dev/null
	[ -n "$LOCK_PID" ] && kill "$LOCK_PID" 2>/dev/null
	for p in $CLIENTS; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import numpy, PIL" 2>/dev/null ||
	{ echo "test-wincapture-lock.sh: 需要 python3-numpy + python3-pil"; exit 1; }
python3 -c "import gi; gi.require_version('Gtk','3.0')" 2>/dev/null ||
	{ echo "test-wincapture-lock.sh: 需要 python3-gobject + gtk3"; exit 1; }
[ -x ./build/vidow ] || { echo "test-wincapture-lock.sh: 先 ninja -C build"; exit 1; }
[ -x ./build/capture ] || { echo "test-wincapture-lock.sh: 先 ninja -C build"; exit 1; }
[ -x ./build/locktest ] || { echo "test-wincapture-lock.sh: 先 ninja -C build"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", x = 0 }
vidow.output{ name = "HEADLESS-2", x = 1280 }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER_ALLOW_SOFTWARE=1
export HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY=
unset VIDOW_TEST_OUTPUTS VIDOW_TEST_ACTION

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
export WAYLAND_DISPLAY
VPID=$(pgrep -xn vidow)
echo "    vidow pid=$VPID WAYLAND_DISPLAY=$WAYLAND_DISPLAY"

# 纯蓝窗口：窗口内容的判据就是蓝像素（tools/bluewin.py）。
setsid nohup python3 ./tools/bluewin.py > "$RUNDIR/bluewin.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 2

# 抓 3 秒，把最新一帧写成 PPM，然后数像素。$1 = 输出图路径，$2 = 标签
grab() {
	rm -f "$1"
	VIDOW_CAPTURE_DUMP="$1" setsid timeout 8 ./build/capture bluewin 3 10 \
		> "$RUNDIR/cap-$2.txt" 2>&1
	local n
	n=$(grep -ac '^capture [0-9]' "$RUNDIR/cap-$2.txt")
	local f
	f=$(grep -ac '这一帧失败' "$RUNDIR/cap-$2.txt")
	echo "    $2：抓到 $n 帧、failed $f 次"
	[ -s "$1" ] || { echo "    $2：没写出图（抓帧没成功？）"; tail -3 "$RUNDIR/cap-$2.txt" | sed 's/^/        /'; }
}

# 数像素：$1 = 图，$2 = 标签，$3 = 期望（"window" = 必须有窗口内容 / "none" = 必须没有）
check_pixels() {
	python3 - "$1" "$2" "$3" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
total = a.shape[0] * a.shape[1]
# 蓝窗的内容（tools/bluewin.py 画的是纯蓝 0,0,255）
blue = int(((a[:, :, 2] > 200) & (a[:, :, 0] < 60) & (a[:, :, 1] < 60)).sum())
# 任何「不是纯黑」的像素：锁定期整帧只该是黑的（镜面关掉 → 没画东西）
nonblack = int((a.max(axis=2) > 8).sum())
print("    %s %dx%d：蓝窗像素 %d（%.2f%%）、非黑像素 %d（%.2f%%）"
      % (sys.argv[2], a.shape[1], a.shape[0], blue, blue * 100.0 / total,
         nonblack, nonblack * 100.0 / total))
want = sys.argv[3]
ok = blue > total * 0.05 if want == "window" else nonblack <= total * 0.01
sys.exit(0 if ok else 1)
PY
}

# ---------------------------------------------------------------- 1) 锁前基线
mark "1) 锁前：窗口抓帧源能抓到窗口（基线）"
grab "$RUNDIR/free.ppm" "锁前"
if check_pixels "$RUNDIR/free.ppm" "锁前" window; then
	pass "锁前抓到了窗口内容（蓝窗像素 > 5%）—— 这条路本来就能抓"
else
	fail "锁前都抓不到窗口内容（客户端 / 源没起来？）"
fi

# ---------------------------------------------------------------- 2) 锁定期间
mark "2) 锁定期间：窗口抓帧不能有窗口内容"
mkfifo "$RUNDIR/lock.in"
exec 9<>"$RUNDIR/lock.in"
setsid ./build/locktest > "$RUNDIR/lock.log" 2>&1 <&9 &
LOCK_PID=$!
for _ in $(seq 1 50); do grep -aq LOCKED "$RUNDIR/lock.log" && break; sleep 0.1; done
if grep -aq LOCKED "$RUNDIR/lock.log"; then
	pass "锁上了（locktest 收到 LOCKED）"
else
	fail "没等到 LOCKED"
	tail -5 "$RUNDIR/lock.log" | sed 's/^/        /'
fi
sleep 0.5
grab "$RUNDIR/locked.ppm" "锁定期间"
if [ -s "$RUNDIR/locked.ppm" ]; then
	if check_pixels "$RUNDIR/locked.ppm" "锁定期间" none; then
		pass "锁定期间抓到的帧里没有窗口内容（像素见上）"
	else
		fail "锁定期间抓到了窗口内容 —— 镜面场景绕过了被关掉的那棵树"
	fi
	if [ "$(grep -ac '这一帧失败' "$RUNDIR/cap-锁定期间.txt")" -eq 0 ]; then
		pass "锁定期间一帧都没失败（回的是黑帧，客户端不用换缓冲）"
	else
		fail "锁定期间抓帧在回 failed（$(grep -ac '这一帧失败' "$RUNDIR/cap-锁定期间.txt") 次）"
	fi
else
	fail "锁定期间没写出图（抓帧全失败？）"
fi

# ---------------------------------------------------------------- 3) 解锁后
mark "3) 解锁后：同一个窗口抓帧照旧能用"
printf 'u\n' >&9
for _ in $(seq 1 50); do grep -aq "sessionlock: 解锁了" "$LOG" && break; sleep 0.1; done
grep -a "sessionlock: 解锁了" "$LOG" >/dev/null &&
	pass "解锁了" ||
	fail "没解锁（日志里没有「解锁了」）"
sleep 0.6
grab "$RUNDIR/after.ppm" "解锁后"
if [ -s "$RUNDIR/after.ppm" ]; then
	if check_pixels "$RUNDIR/after.ppm" "解锁后" window; then
		pass "解锁后又能抓到窗口内容了（功能没被堵坏）"
	else
		fail "解锁后抓不到窗口内容了（把功能堵坏了）"
	fi
else
	fail "解锁后没写出图"
fi

# ---------------------------------------------------------------- 4) 收尾
mark "4) 没崩、没断言"
if kill -0 "$VPID" 2>/dev/null; then
	pass "合成器还活着"
else
	fail "合成器挂了"
fi
grep -aq "Assertion" "$LOG" && fail "日志里有 assert" || pass "日志里没有 assert"

echo
if [ $FAILED -eq 0 ]; then
	echo "test-wincapture-lock.sh: OK"
else
	echo "test-wincapture-lock.sh: 有失败项"
fi
exit $FAILED
