#!/bin/bash
# 会话锁（ext-session-lock-v1）：
#   S1：global 注册 / 锁定握手 / 桌面被藏起来
#   S2：lock surface 生命周期 + 输出热插拔
#
# 判据（全部来自日志和像素）：
#   A `wayland-info` 里有 ext_session_lock_manager_v1（manager global 建出来了）
#   B 起 tools/locktest（最小锁屏客户端，本机没有真客户端）→ 5 秒内打出 LOCKED
#     （说明 configure → ack → commit → send_locked 整条握手通了；wlroots 不自动
#      发首个 configure，不主动 configure 客户端会永远卡住）
#   C grim 前后比对：锁后画面 = lock 客户端贴的纯色（桌面/窗口被藏起来了）
#   G1 锁定期间**插**一块新屏（VIDOW_TEST_OUTPUTS="12000:add:1280x720"）：
#      ① 日志里「锁定期间新屏 … 黑 rect 已就位」（桌面不会从新屏漏出去）
#      ② locktest 为新屏建了 lock surface 并收到 configure（S2 允许客户端给新屏建）
#      ③ grim 抓新屏：不是桌面（给数字：锁屏纯色占比 / 纯黑占比 / 桌面像素数）
#   G2 锁定期间**拔**一块屏（另起一遍 vidow，VIDOW_TEST_OUTPUTS="12000:remove:HEADLESS-2"）：
#      不崩、日志里记账清干净（黑 rect 收了、lock surface 记账收了、待呈现的账销了）、
#      剩下那块屏还是锁内容（桌面没漏）
#
# **不测**（后续步骤的事，见方案 §5）：D/E 输入路由（S3）、F 客户端崩溃（S4）、
# H 解锁恢复（S4）。所以这个脚本全过**不等于**可以对外宣称支持会话锁 —— 现在锁上
# 之后输入还会漏给未锁客户端（S3 才修）。
#
# 用法：./test-sessionlock.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-sessionlock-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
VPID=""
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

# 热插拔步骤相对 vidow 启动的毫秒数。取 12 秒：锁在 5~7 秒就完成了（上面 B/C 那几步），
# 12 秒足够晚，保证「插 / 拔」确实发生在锁定期间；脚本随后是等日志，不靠这个数当同步点。
HOTPLUG_MS=12000

start_vidow() { # $1 = VIDOW_TEST_OUTPUTS 的值（空 = 不装热插拔钩子）
	export VIDOW_TEST_OUTPUTS="$1"
	: > "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	grep -aq "vidow ready" "$LOG" || {
		echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	VPID=$(pgrep -xn vidow)
	echo "    vidow pid=$VPID WAYLAND_DISPLAY=$WAYLAND_DISPLAY"
}

stop_vidow() {
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
		VIDOW_PGID=""
	fi
}

stop_clients() {
	[ -n "$LOCK_PID" ] && kill "$LOCK_PID" 2>/dev/null
	LOCK_PID=""
	for p in $CLIENTS; do kill "$p" 2>/dev/null; done
	CLIENTS=""
	exec 9>&- 2>/dev/null
	sleep 0.3
}

start_bluewin() { # 摆一个「有窗口的桌面」：锁后「桌面被藏起来」才有东西可藏
	setsid nohup python3 ./tools/bluewin.py > "$RUNDIR/bluewin.log" 2>&1 </dev/null &
	CLIENTS="$CLIENTS $!"
	sleep 2
}

# 起 locktest（stdin 走 fifo），等它打出 LOCKED。$1 = fifo 路径，$2 = locktest 日志
start_locktest() {
	mkfifo "$1"
	exec 9<>"$1"
	setsid ./build/locktest > "$2" 2>&1 <&9 &
	LOCK_PID=$!
	for _ in $(seq 1 50); do grep -aq LOCKED "$2" && break; sleep 0.1; done
}

# 抓到的画面是不是「锁屏内容」（lock 客户端的纯色，或者没 lock surface 时的纯黑）：
# 打印锁屏色占比 / 纯黑占比 / 桌面像素数。$1 = 图，$2 = 标签
check_not_desktop() {
	python3 - "$1" "$2" <<'PY'
import sys
import numpy as np
from PIL import Image
LOCK = np.array((0x22, 0x66, 0x99), dtype=np.int16)
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
total = a.shape[0] * a.shape[1]
lock = int((np.abs(a - LOCK).max(axis=2) <= 2).sum())
black = int((a.max(axis=2) <= 2).sum())
# 既不是锁屏纯色、也不是纯黑 = 桌面（壁纸 / 窗口）漏出来了
desktop = total - lock - black
print("    %s %dx%d：锁屏色 %.1f%%、纯黑 %.1f%%、桌面像素 %d"
      % (sys.argv[2], a.shape[1], a.shape[0], lock * 100.0 / total,
         black * 100.0 / total, desktop))
sys.exit(0 if lock + black >= total * 0.99 and desktop == 0 else 1)
PY
}

# ============================================================ 第一遍：A / B / C / G1

mark "起 vidow（headless 两块屏，装 G1 热插拔钩子：${HOTPLUG_MS}ms 时插一块新屏）"
start_vidow "${HOTPLUG_MS}:add:1280x720"

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
start_bluewin
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

LOCKLOG="$RUNDIR/lock.log"
start_locktest "$RUNDIR/lock.in" "$LOCKLOG"
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

# ---------------------------------------------------------------- G1 插屏
mark "G1 锁定期间插一块新屏（${HOTPLUG_MS}ms 时 HEADLESS-3）"
# 等钩子真的插上：sessionlock.c 给新屏补黑 rect 时会打这一行。
for _ in $(seq 1 80); do grep -aq "sessionlock: 锁定期间新屏" "$LOG" && break; sleep 0.5; done
if grep -a "sessionlock: 锁定期间新屏" "$LOG" >/dev/null; then
	pass "新屏一到就补了黑 rect：$(grep -a 'sessionlock: 锁定期间新屏' "$LOG" | tail -1 | sed 's/.*sessionlock: //')"
else
	fail "日志里没有「锁定期间新屏 … 黑 rect 已就位」（新屏没被认出来？）"
	tail -5 "$LOG" | sed 's/^/        /'
fi
# 客户端有义务给新屏建 lock surface（它看得到新输出的 wl_output global）。
for _ in $(seq 1 40); do
	[ "$(grep -ac '^configure ' "$LOCKLOG")" -ge 3 ] && break
	sleep 0.25
done
NCONF=$(grep -ac '^configure ' "$LOCKLOG")
if [ "$NCONF" -ge 3 ]; then
	pass "locktest 给新屏也建了 lock surface 并收到 configure（共 $NCONF 次）：$(grep -a '^configure ' "$LOCKLOG" | tail -1)"
else
	fail "新屏没拿到 configure（还是 $NCONF 次）—— 客户端没看到新输出，或者合成器没 configure"
	grep -a . "$LOCKLOG" | tail -5 | sed 's/^/        /'
fi
sleep 0.5
if grim -o HEADLESS-3 "$RUNDIR/new3.png" 2>"$RUNDIR/grim-new3.log"; then
	if check_not_desktop "$RUNDIR/new3.png" "新屏 HEADLESS-3"; then
		pass "新屏上是锁屏内容，没有桌面（桌面像素 0）"
	else
		fail "新屏上有桌面像素漏出来（见上面数字）"
	fi
else
	fail "新屏 grim 抓帧失败（$(tail -1 "$RUNDIR/grim-new3.log")）"
fi

if kill -0 "$VPID" 2>/dev/null; then
	pass "第一遍 vidow 全程活着"
else
	fail "第一遍 vidow 死了"
fi

# ============================================================ 第二遍：G2 拔屏

mark "G2 锁定期间拔一块屏（另起一遍 vidow：${HOTPLUG_MS}ms 时拔 HEADLESS-2）"
stop_clients
stop_vidow
start_vidow "${HOTPLUG_MS}:remove:HEADLESS-2"
start_bluewin
LOCKLOG="$RUNDIR/lock2.log"
start_locktest "$RUNDIR/lock2.in" "$LOCKLOG"
if grep -aq LOCKED "$LOCKLOG"; then
	pass "第二遍也锁上了（LOCKED）"
else
	fail "第二遍没等到 LOCKED"
	tail -5 "$LOCKLOG" | sed 's/^/        /'
fi
# 等钩子真的拔掉：sessionlock.c 收账时会打这一行。
for _ in $(seq 1 80); do grep -aq "sessionlock: 输出 HEADLESS-2 拔了" "$LOG" && break; sleep 0.5; done
if grep -a "sessionlock: 输出 HEADLESS-2 拔了" "$LOG" >/dev/null; then
	pass "拔屏时记账清干净：$(grep -a 'sessionlock: 输出 HEADLESS-2 拔了' "$LOG" | tail -1 | sed 's/.*sessionlock: //')"
else
	fail "日志里没有拔屏收账那一行"
	tail -8 "$LOG" | sed 's/^/        /'
fi
grep -aq "test: 拔掉输出 HEADLESS-2" "$LOG" &&
	pass "钩子真的走了 wlr_output_destroy（backend 的真实拔线路径）" ||
	fail "日志里没有「test: 拔掉输出 HEADLESS-2」"
# 拔屏之后：vidow 不崩，剩下那块屏还是锁内容（桌面没漏回来）
if kill -0 "$VPID" 2>/dev/null; then
	pass "拔屏后 vidow 还活着"
else
	fail "拔屏把 vidow 搞崩了"
fi
if grim -o HEADLESS-2 "$RUNDIR/gone.png" 2>/dev/null; then
	fail "拔掉的 HEADLESS-2 还能抓帧（没真拔掉？）"
else
	pass "HEADLESS-2 真的没了（grim -o HEADLESS-2 失败）"
fi
sleep 0.5
if grim -o HEADLESS-1 "$RUNDIR/left1.png" 2>"$RUNDIR/grim-left1.log"; then
	if check_not_desktop "$RUNDIR/left1.png" "剩下的 HEADLESS-1"; then
		pass "剩下那块屏还是锁内容，桌面没漏回来"
	else
		fail "剩下那块屏上桌面漏回来了（见上面数字）"
	fi
else
	fail "拔屏后 HEADLESS-1 抓帧失败（$(tail -1 "$RUNDIR/grim-left1.log")）"
fi
if grep -a 'sessionlock:' "$LOG" | grep -aqE 'ERROR|失败|不认识'; then
	fail "日志里有 sessionlock 的错误行：$(grep -a 'sessionlock:' "$LOG" | grep -aE 'ERROR|失败|不认识' | tail -1)"
else
	pass "第二遍日志里没有 sessionlock 的错误"
fi

# ---------------------------------------------------------------- 收尾
echo
# 自己起的那一坨由 trap 收（cleanup 里 kill -TERM -- -PGID）；这里只看有没有别人留下的。
LEAK=""
for p in $(pgrep -x vidow 2>/dev/null); do
	[ "$(ps -o pgid= -p "$p" 2>/dev/null | tr -d ' ')" = "$VIDOW_PGID" ] && continue
	LEAK="$LEAK $p"
done
[ -n "$LEAK" ] && echo "注意：有不是这次起的 vidow 进程还活着：$LEAK"
if [ "$FAILED" -eq 0 ]; then
	echo "test-sessionlock.sh 全部通过（S1：能锁、能黑屏、能握手；S2：lock surface 生命周期 + 输出热插拔）"
else
	echo "test-sessionlock.sh 有失败项"
fi
exit "$FAILED"
