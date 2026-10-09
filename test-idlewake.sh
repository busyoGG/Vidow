#!/bin/bash
# 空载唤醒：没有窗口、也没人抓帧的时候，vidow 一次都不该醒。
#
# 抓帧看门狗（output.c 的 capture_watchdog）原来是「建好就 50ms 一次永远转下去」：
# 静止的屏幕永远不提交，而抓帧要靠一次输出提交来应答，所以它得在。代价是空载也
# 每秒醒 20 次 —— 实测 strace：21 秒里 epoll_wait 返回 427 次、timerfd_settime 852
# 次，期间一次抓帧都没有（空载 CPU 只有 0.05%，所以这条不是 CPU，是「进不了深睡」）。
# 现在它按需启动：screencopy 的抓帧请求踢一脚（vidow_output_capture_kick），
# 抓完没人等着就自己停。
#
# 判据（/proc/<pid>/status 的 voluntary_ctxt_switches —— 进程自愿让出 CPU 的次数，
# 空载时每次定时器唤醒都记一笔；本机实测改前 10 秒正好 200，改后 0）：
#   1) 空载 10 秒：增量 < 20
#   2) 一张从没画过东西的空屏，grim 照样抓得到（看门狗按需启动没把抓帧弄坏）
#   3) 壁纸上来后静置 2 秒（期间零唤醒），两次截图逐像素一样 —— 屏幕真的静止，
#      判据 1 的前提成立，而且这两次抓帧都得成功
#   4) 抓完之后再等 5 秒：增量仍然 < 10（抓完就停，不留下常转的闹钟）
#   5) vidow 全程活着
#
# 用法：./test-idlewake.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-idlewake-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENTS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENTS; do kill "$p" 2>/dev/null; done
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in *wall.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v grim >/dev/null || { echo "test-idlewake.sh: 需要 grim"; exit 1; }
python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-idlewake.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }
python3 -c "import numpy, PIL" 2>/dev/null ||
	{ echo "test-idlewake.sh: 需要 python3-numpy + python3-pil"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 2, mode = "2560x1440" }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=info
export DISPLAY= WAYLAND_DISPLAY=

mark "空载不空转（抓帧看门狗按需启动）"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || {
	echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
VPID=$(pgrep -xn vidow)
echo "    vidow pid=$VPID（屏上什么都没有）"

# 自愿上下文切换：进程每次「让出 CPU 等事件」记一笔。空载时唯一的来源就是定时器。
ctxt() { awk '$1=="voluntary_ctxt_switches:"{print $2}' "/proc/$VPID/status" 2>/dev/null; }

sleep 2
C0=$(ctxt)
sleep 10
C1=$(ctxt)
IDLE=$((C1 - C0))
echo "    空载 10 秒：自愿上下文切换 $C0 -> $C1（增量 $IDLE）"
if [ "$IDLE" -lt 20 ]; then
	pass "空载 10 秒唤醒增量 $IDLE < 20（50ms 常开的看门狗会给出 200）"
else
	fail "空载 10 秒醒了 $IDLE 次（< 20 才对：抓帧看门狗又变成常开了？）"
fi

# 1) 从没画过东西的空屏：抓帧得自己推帧才应答得出来
if timeout 10 grim -o HEADLESS-1 "$RUNDIR/empty.png" 2>"$RUNDIR/grim-empty.log"; then
	if python3 - "$RUNDIR/empty.png" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.uint8)
print("    空屏截图 %dx%d，最大亮度 %d" % (a.shape[1], a.shape[0], a.max()))
sys.exit(0 if a.shape[1] > 0 and a.max() == 0 else 1)
PY
	then
		pass "空屏（从没提交过一帧）上 grim 抓帧成功，而且确实是空的"
	else
		fail "空屏截图不是全黑（抓帧内容不对？）"
	fi
else
	fail "空屏上 grim 抓帧超时 / 失败（抓帧看门狗没被叫起来？）"
	tail -3 "$RUNDIR/grim-empty.log" | sed 's/^/        /'
fi

# 2) 有内容但静止的屏幕：壁纸画一次就不动了
setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 2
S0=$(ctxt)
sleep 2
S1=$(ctxt)
STATIC=$((S1 - S0))
echo "    壁纸静置 2 秒：唤醒增量 $STATIC"
[ "$STATIC" -lt 10 ] && pass "静止屏幕 2 秒里只有 $STATIC 次唤醒" ||
	fail "静止屏幕 2 秒里醒了 $STATIC 次（壁纸在重画？还是又有周期性唤醒？）"

if timeout 10 grim -o HEADLESS-1 "$RUNDIR/a.png" 2>/dev/null &&
		sleep 1 && timeout 10 grim -o HEADLESS-1 "$RUNDIR/b.png" 2>/dev/null; then
	pass "静止屏幕上连续两次抓帧都成功"
	if python3 - "$RUNDIR/a.png" "$RUNDIR/b.png" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
d = int(np.abs(a - b).max())
print("    两张截图最大差 %d，画面最大亮度 %d" % (d, int(a.max())))
sys.exit(0 if d == 0 and int(a.max()) > 0 else 1)
PY
	then
		pass "两张截图逐像素一样，而且不是全黑（屏幕确实静止）"
	else
		fail "两张静止截图不一样（屏幕在动？）"
	fi
else
	fail "静止屏幕上的抓帧失败"
fi

# 3) 抓完就停：不留下常转的闹钟
T0=$(ctxt)
sleep 5
T1=$(ctxt)
TAIL=$((T1 - T0))
echo "    抓帧后 5 秒：唤醒增量 $TAIL"
[ "$TAIL" -lt 10 ] && pass "抓完之后 5 秒里只有 $TAIL 次唤醒（看门狗自己停了）" ||
	fail "抓完之后 5 秒里醒了 $TAIL 次（看门狗没停？）"

if kill -0 "$VPID" 2>/dev/null; then
	pass "vidow 全程活着"
else
	fail "vidow 死了"
fi

echo
if [ "$FAILED" -eq 0 ]; then
	echo "test-idlewake.sh 全部通过"
else
	echo "test-idlewake.sh 有失败项"
fi
exit "$FAILED"
