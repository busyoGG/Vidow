#!/bin/bash
# 会话锁 S1（ext-session-lock-v1）：global 注册 / 锁定握手 / 桌面被藏起来。
#
# 只验方案 §3.2 的 A/B/C 三条（用户明确要求「不要做多余的无用测试」）：
#   A `wayland-info` 里有 ext_session_lock_manager_v1（manager global 建出来了）
#   B 起 tools/locktest（最小锁屏客户端，本机没有真客户端）→ 5 秒内打出 LOCKED
#     （说明 configure → ack → commit → send_locked 整条握手通了；wlroots 不自动
#      发首个 configure，不主动 configure 客户端会永远卡住）
#   C grim 前后比对：锁后画面 = lock 客户端贴的纯色（桌面/窗口被藏起来了）
#
# **不测**（后续步骤的事，见方案 §5）：D/E 输入路由（S3）、F 客户端崩溃（S4）、
# G 输出热插拔（S2）、H 解锁恢复（S4）。所以这个脚本全过**不等于**可以对外宣称
# 支持会话锁 —— 现在锁上之后输入还会漏给未锁客户端（S3 才修）。
#
# 用法：./test-sessionlock.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-sessionlock-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
LOCKLOG="$RUNDIR/lock.log"
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

command -v wayland-info >/dev/null || { echo "test-sessionlock.sh: 需要 wayland-info"; exit 1; }
command -v grim >/dev/null || { echo "test-sessionlock.sh: 需要 grim"; exit 1; }
python3 -c "import numpy, PIL" 2>/dev/null ||
	{ echo "test-sessionlock.sh: 需要 python3-numpy + python3-pil"; exit 1; }
python3 -c "import gi; gi.require_version('Gtk','3.0')" 2>/dev/null ||
	{ echo "test-sessionlock.sh: 需要 python3-gobject + gtk3（有窗口的客户端）"; exit 1; }
[ -x ./build/locktest ] || { echo "test-sessionlock.sh: 先 ninja -C build（缺 ./build/locktest）"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 两块屏，谁在左边写死（headless 的自动排是反的）。
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", x = 0 }
vidow.output{ name = "HEADLESS-2", x = 1280 }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER_ALLOW_SOFTWARE=1
export HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY=

mark "起 vidow（headless 两块屏）"
setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || {
	echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
VPID=$(pgrep -xn vidow)
echo "    vidow pid=$VPID WAYLAND_DISPLAY=$WAYLAND_DISPLAY"

# ---------------------------------------------------------------- A
mark "A global 注册"
GLOBALS=$(wayland-info 2>/dev/null | grep -c ext_session_lock_manager_v1)
if [ "$GLOBALS" -ge 1 ]; then
	pass "wayland-info 里有 ext_session_lock_manager_v1（$GLOBALS 处）"
else
	fail "wayland-info 里没有 ext_session_lock_manager_v1"
fi

# ---------------------------------------------------------------- B
mark "B 锁定握手（locktest 5 秒内打出 LOCKED）"
# 先摆一个有窗口的桌面：锁后「桌面被藏起来」才有东西可藏。
setsid nohup python3 ./tools/bluewin.py > "$RUNDIR/bluewin.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 2
grim -o HEADLESS-1 "$RUNDIR/before1.png" 2>"$RUNDIR/grim-before.log" &&
	grim -o HEADLESS-2 "$RUNDIR/before2.png" 2>>"$RUNDIR/grim-before.log" &&
	pass "锁之前 grim 抓到了桌面（before1.png / before2.png）" ||
	fail "锁之前 grim 就抓不到（$(tail -1 "$RUNDIR/grim-before.log")）"
if python3 - "$RUNDIR/before1.png" "$RUNDIR/before2.png" <<'PY'
import sys
import numpy as np
from PIL import Image
# 蓝窗（tools/bluewin.py）落在哪块屏不固定（实测在 HEADLESS-2）：两块里至少一块
# 上得有它 —— 不然「锁后桌面被藏起来」就没东西可藏，C 也就没证据。
for path in sys.argv[1:]:
    a = np.asarray(Image.open(path).convert('RGB'), dtype=np.uint8)
    blue = int((a[:, :, 2] > 200).sum())
    print("    %s：蓝窗像素 %d" % (path.rsplit('/', 1)[-1], blue))
    if blue > 0:
        sys.exit(0)
sys.exit(1)
PY
then
	pass "锁之前屏上确实有个窗口（蓝窗在，C 才有意义）"
else
	fail "两块屏上都没有蓝窗（客户端没起来？C 的「藏桌面」就没证据了）"
fi

mkfifo "$RUNDIR/lock.in"
# O_RDWR 打开 FIFO 不会阻塞（只有读端会），测试收尾还能往里写 u。
exec 9<>"$RUNDIR/lock.in"
setsid ./build/locktest > "$LOCKLOG" 2>&1 <&9 &
LOCK_PID=$!
for _ in $(seq 1 50); do grep -aq LOCKED "$LOCKLOG" && break; sleep 0.1; done
if grep -aq LOCKED "$LOCKLOG"; then
	pass "locktest 收到 locked（B 通：configure/ack/commit/send_locked 全通）"
else
	fail "5 秒内没等到 LOCKED"
	tail -5 "$LOCKLOG" | sed 's/^/        /'
fi
NCONF=$(grep -ac '^configure ' "$LOCKLOG")
if [ "$NCONF" -ge 2 ]; then
	pass "两块屏各收到一次 configure（$NCONF 次，含尺寸）"
else
	fail "只收到 $NCONF 次 configure（两块屏各一次才对）"
	grep -a . "$LOCKLOG" | sed 's/^/        /'
fi
grep -a "sessionlock: locked 已发" "$LOG" >/dev/null &&
	pass "vidow 日志里有「locked 已发」" ||
	fail "vidow 日志里没有「locked 已发」"

# ---------------------------------------------------------------- C
mark "C 桌面被藏起来（grim 锁后 = 纯色）"
sleep 0.5
if grim -o HEADLESS-1 "$RUNDIR/after1.png" 2>"$RUNDIR/grim-after.log" &&
		grim -o HEADLESS-2 "$RUNDIR/after2.png" 2>>"$RUNDIR/grim-after.log"; then
	pass "锁之后两块屏都抓帧成功（after1.png / after2.png）"
	if python3 - "$RUNDIR/before1.png" "$RUNDIR/before2.png" \
			"$RUNDIR/after1.png" "$RUNDIR/after2.png" <<'PY'
import sys
import numpy as np
from PIL import Image
LOCK = (0x22, 0x66, 0x99)
before = [np.asarray(Image.open(p).convert('RGB'), dtype=np.int16) for p in sys.argv[1:3]]
after = [np.asarray(Image.open(p).convert('RGB'), dtype=np.int16) for p in sys.argv[3:5]]
ok = True
for name, a, b in zip(("HEADLESS-1", "HEADLESS-2"), before, after):
    hit = int((np.abs(b - np.array(LOCK, dtype=np.int16)).max(axis=2) <= 2).sum())
    frac = hit / (b.shape[0] * b.shape[1])
    diff = int(np.abs(a - b).max())
    print("    %s：锁前最大亮度 %d、锁后锁屏色占 %.1f%%、前后最大差 %d"
          % (name, int(a.max()), frac * 100.0, diff))
    if frac < 0.99 or diff == 0:
        ok = False
sys.exit(0 if ok else 1)
PY
	then
		pass "两块屏锁后都是 lock 客户端的纯色，且和锁前不同（桌面/窗口确实被藏起来了）"
	else
		fail "锁后画面不是 lock 客户端的纯色（黑幕没盖住？还是抓帧绕过了场景树？）"
	fi
else
	fail "锁之后 grim 抓帧失败（$(tail -1 "$RUNDIR/grim-after.log")）"
fi

if kill -0 "$VPID" 2>/dev/null; then
	pass "vidow 全程活着"
else
	fail "vidow 死了"
fi

echo
if [ "$FAILED" -eq 0 ]; then
	echo "test-sessionlock.sh 全部通过（S1：能锁、能黑屏、能握手）"
else
	echo "test-sessionlock.sh 有失败项"
fi
exit "$FAILED"
