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
#   D 锁定期间注入一个**已绑定**的键（super+j → focus-workspace 2）：
#      ① 工作区 / 焦点前后不变；② 日志里「绑定 … 锁定期间不生效（白名单只有 VT 切换）」
#      = 键确实走到了绑定表，只是被白名单挡住（不是「注入本来就不触发键位」）；
#      ③ locktest 打出了 key down / key up = 键送到了锁屏界面
#   E 锁定期间指针（move-cursor + pointer-button + pointer-scroll）：
#      ① 光标压到**没**拿焦点的那个窗口上，focus_follows_mouse 开着也不换焦点；
#      ② locktest 收到 pfocus / button / axis = 指针只给了锁屏界面
#   S3 语义① mouse_move_enables_dpms：锁定期 output-power off + move-cursor →
#      日志「输入唤醒」且屏回来了（否则屏关着没法输密码）
#   S3 语义② focus_follows_mouse：配成 true 也不把焦点给被藏起来的窗口（同 E ①）
#
#   F kill -9 locktest（客户端在锁定期崩溃）→ **不解锁**（协议 XML:111-113 的
#      「黑屏死锁」态）：vidow 不崩、日志有「客户端崩溃 / 被 kill，按协议保持锁定」、
#      grim 仍不是桌面（桌面像素 0，给数字）、注入已绑定键仍不生效
#   H locktest stdin 写 u（unlock_and_destroy）→ 桌面回来：日志「解锁了，桌面树恢复」、
#      grim 与锁前**逐像素相同 ≥99%**（给数字）、注入已绑定键真的切了工作区、
#      解锁后屏上不再有大块锁屏色（layer_lock 那两棵树关掉了）
#   抓帧审计（方案 §4 风险 3，锁定期间）：vidow 的输出抓帧是**自己渲染一帧**，
#      锁定期间必须实测不含桌面 —— ① grim（zwlr_screencopy → src/screencopy.c →
#      src/capture.c 自己渲染）② wf-recorder 录像抽一帧（screencopy 连续帧 / damage
#      那条路）。两条都断言「锁屏色 + 纯黑 = 100%、桌面像素 0」（给数字）
#
# **不测**：ext-image-copy-capture 输出源（xdg-desktop-portal 截图）/ KDE screencast
# 输出流没有便宜的免 portal 客户端 —— 那两条和 ① ② 走的是**同一个** vidow_capture_grab*
# （src/capture.c:226，见 src/screencopy.c:388 / src/screencast.c:1048,1069），
# 靠代码路径证据，不是实测。
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
RECPID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	exec 9>&- 2>/dev/null
	[ -n "$LOCK_PID" ] && kill "$LOCK_PID" 2>/dev/null
	[ -n "$RECPID" ] && kill -INT -- "-$RECPID" 2>/dev/null
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
# F / H 要「真键盘」那条路（wtype 造虚拟键盘 → input.c 的 keyboard_key）：vidowmsg
# send-key 走的是注入那条路，它平时**本来就不触发键位表**，量不了「解锁后键位恢复」。
command -v wtype >/dev/null || { echo "test-sessionlock.sh: 需要 wtype"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 两块屏，谁在左边写死（headless 的自动排是反的）。
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", x = 0 }
vidow.output{ name = "HEADLESS-2", x = 1280 }
-- S3：这两条是**故意**打开的，用来验锁定期间的两条语义 ——
--   mouse_move_enables_dpms：屏关着的时候指针一动要能唤醒（否则没法输密码）
--   focus_follows_mouse：即使开着，也不许把焦点给被藏起来的窗口
mouse_move_enables_dpms = true
focus_follows_mouse = true
-- D 用：一个**已绑定**的键（注入它，看锁定期间工作区会不会变）。
-- 动作名是 "workspace 2"（README 动作表）。以前这里写的是 "focus-workspace 2" ——
-- 那不是动作（vidow 打「不认识的动作」），D 的断言照样成立（白名单在动作解析之前就
-- 挡下了），但 H 要「解锁后键位真的生效」，动作名必须是对的。
vidow.bind("Mod+J", "workspace 2")
-- 手势：锁定瞬间不许动这张配置表（vidow_gesture_clear 是配置重载用的，清的是
-- 用户绑的手势）。测试拿 vidowmsg state 的 gestures 计数当判据。
vidow.gesture{ fingers = 4, direction = "up", action = "workspace up" }
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
# 打印锁屏色占比 / 纯黑占比 / 桌面像素数。$1 = 图，$2 = 标签，$3 = 颜色容差（缺省 2；
# 视频帧过了一趟 YUV420 往返，色偏能到 ±10，那条路给 24）。
check_not_desktop() {
	python3 - "$1" "$2" "${3:-2}" <<'PY'
import sys
import numpy as np
from PIL import Image
LOCK = np.array((0x22, 0x66, 0x99), dtype=np.int16)
TOL = int(sys.argv[3])
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
total = a.shape[0] * a.shape[1]
lock = int((np.abs(a - LOCK).max(axis=2) <= TOL).sum())
black = int((a.max(axis=2) <= 2).sum())
# 既不是锁屏纯色、也不是纯黑 = 桌面（壁纸 / 窗口）漏出来了
desktop = total - lock - black
print("    %s %dx%d：锁屏色 %.2f%%（容差 ±%d）、纯黑 %.2f%%、桌面像素 %d（%.2f%%）"
      % (sys.argv[2], a.shape[1], a.shape[0], lock * 100.0 / total, TOL,
         black * 100.0 / total, desktop, desktop * 100.0 / total))
sys.exit(0 if lock + black >= total * 0.99 and desktop <= total * 0.01 else 1)
PY
}

# 锁前 / 解锁后逐像素比对（H 的判据：桌面真的回来了）。$1 $2 = 两张图，$3 = 标签
compare_desktop() {
	python3 - "$1" "$2" "$3" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
if a.shape != b.shape:
    print("    %s 尺寸不同：%s vs %s" % (sys.argv[3], a.shape, b.shape))
    sys.exit(1)
d = np.abs(a - b).max(axis=2)
total = d.size
same = int((d <= 2).sum())
print("    %s：逐像素相同 %.2f%%、最大差 %d、平均差 %.3f"
      % (sys.argv[3], same * 100.0 / total, int(d.max()), float(d.mean())))
sys.exit(0 if same >= total * 0.99 else 1)
PY
}

# 录像抽出来的那一帧：除了「是锁屏色」，还要跟**锁前的桌面图**逐像素比一遍 —— 视频帧
# 过了一趟 YUV420 往返（色偏 ±10 量级），拿容差 24 比，两条一起看才说得清「录屏这条
# 路没把桌面录进去」。$1 = 锁前桌面图，$2 = 录像帧，$3 = 标签
check_video_frame() {
	python3 - "$1" "$2" "$3" <<'PY'
import sys
import numpy as np
from PIL import Image
LOCK = np.array((0x22, 0x66, 0x99), dtype=np.int16)
TOL = 24
a = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
total = a.shape[0] * a.shape[1]
lock = int((np.abs(a - LOCK).max(axis=2) <= TOL).sum())
like = -1
try:
    d = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
    if d.shape == a.shape:
        like = int((np.abs(a - d).max(axis=2) <= TOL).sum())
except Exception:
    pass
print("    %s %dx%d：锁屏色 %.2f%%、与锁前桌面相同 %s"
      % (sys.argv[3], a.shape[1], a.shape[0], lock * 100.0 / total,
         "%.2f%%" % (like * 100.0 / total) if like >= 0 else "(没有对照图)"))
sys.exit(0 if lock >= total * 0.99 and (like < 0 or like <= total * 0.01) else 1)
PY
}

# 探测用的两个小工具（S3 的 D / E 要「前后值」）：当前活动工作区 / 焦点窗口。
msg() { ./build/vidowmsg "$@" 2>/dev/null; }
active_ws() {
	msg workspaces | python3 -c '
import sys, json
try:
    ws = json.load(sys.stdin)
except Exception:
    print("(读不到)"); sys.exit(0)
print("active=" + str([(w.get("idx"), w.get("name")) for w in ws if w.get("active")]))'
}
focused_win() {
	msg focused | python3 -c '
import sys, json
try:
    d = json.load(sys.stdin)
except Exception:
    print("(读不到)"); sys.exit(0)
win = d.get("window") or {}
print("focused=%s(%s)" % (d.get("window_id"), win.get("title")))'
}
# 没拿到键盘焦点的那个窗口的几何中心（E 的靶子：光标压过去，焦点不许跟过去）
target_center() {
	msg windows | python3 -c '
import sys, json
try:
    ws = json.load(sys.stdin)
except Exception:
    sys.exit(1)
free = [w for w in ws if not w.get("focused") and not w.get("popup")]
if not free:
    sys.exit(1)
g = free[0]["geometry"]
print("%d %d" % (g["x"] + g["width"] // 2, g["y"] + g["height"] // 2))'
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
# 两个窗口：E 要拿「没拿到键盘焦点的那个」当靶子（光标压过去，焦点不许跟过去）。
start_bluewin
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

# 锁之前记一笔手势配置数（S3：锁定不许动这张配置表）
GEST_BEFORE=$(msg state | python3 -c '
import sys, json
print(json.load(sys.stdin).get("gestures"))' 2>/dev/null)

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

# ---------------------------------------------------------------- D 键盘
mark "D 锁定期间注入一个已绑定的键（super+j → focus-workspace 2）"
WS_BEFORE=$(active_ws)
FOC_BEFORE=$(focused_win)
msg send-key tap super+j >/dev/null
msg send-key tap super+j >/dev/null
sleep 0.4
WS_AFTER=$(active_ws)
FOC_AFTER=$(focused_win)
echo "    工作区：$WS_BEFORE  ->  $WS_AFTER"
echo "    焦点：  $FOC_BEFORE  ->  $FOC_AFTER"
if [ "$WS_BEFORE" = "$WS_AFTER" ] && [ "$FOC_BEFORE" = "$FOC_AFTER" ]; then
	pass "工作区 / 焦点都没变（注入的已绑定键没生效）"
else
	fail "锁定期间注入的已绑定键改变了工作区 / 焦点"
fi
# 这一行是「键确实走到了绑定表、只是被白名单挡住」的直接证据：
# 光看「工作区没变」不够 —— 注入那条路平时本来就不触发键位（见 input.c）。
if grep -a "锁定期间不生效" "$LOG" | grep -aq "Mod+J"; then
	pass "日志：$(grep -a '锁定期间不生效' "$LOG" | tail -1 | sed 's/.*sessionlock: //')"
else
	fail "日志里没有「绑定 Mod+J … 锁定期间不生效」（白名单没生效？还是键没走到绑定表？）"
fi
# locktest 那一侧：键送到了锁屏界面
if grep -aq '^key ' "$LOCKLOG"; then
	pass "locktest 收到了键：$(grep -a '^key ' "$LOCKLOG" | tr '\n' ' ' | sed 's/ $//')"
else
	fail "locktest 没收到任何按键（键没送给 lock surface）"
fi

# 锁定不许动「手势配置表」（那是配置，不是「在跑的那个手势」）
GEST_AFTER=$(msg state | python3 -c '
import sys, json
print(json.load(sys.stdin).get("gestures"))' 2>/dev/null)
if [ -n "$GEST_BEFORE" ] && [ "$GEST_BEFORE" = "$GEST_AFTER" ] &&
		case "$GEST_AFTER" in *竖扫*) true ;; *) false ;; esac; then
	pass "手势配置表没被锁定动过（$GEST_AFTER）"
else
	fail "手势配置表变了：$GEST_BEFORE -> $GEST_AFTER（锁定不该清配置）"
fi

# ---------------------------------------------------------------- E 指针
mark "E 锁定期间指针只给锁屏界面（点击 / 滚动不漏给被藏起来的窗口）"
if TARGET=$(target_center); then
	FOC_BEFORE=$(focused_win)
	msg move-cursor $TARGET >/dev/null
	msg pointer-button press left >/dev/null
	msg pointer-button release left >/dev/null
	msg pointer-scroll 0 60 >/dev/null
	sleep 0.4
	FOC_AFTER=$(focused_win)
	echo "    光标压到 $TARGET（一个没拿焦点的窗口），焦点：$FOC_BEFORE -> $FOC_AFTER"
	if [ "$FOC_BEFORE" = "$FOC_AFTER" ]; then
		pass "焦点没被抢走（focus_follows_mouse 开着也不跟被藏起来的窗口）"
	else
		fail "光标压到被藏起来的窗口上就把焦点抢走了"
	fi
else
	fail "找不到「没拿焦点的窗口」当靶子（E 没法量）"
fi
PEV=$(grep -acE '^(pfocus|button|axis|motion)' "$LOCKLOG")
if grep -aq '^pfocus' "$LOCKLOG" && grep -aq '^button ' "$LOCKLOG" && grep -aq '^axis ' "$LOCKLOG"; then
	pass "locktest 收到了指针：$(grep -aE '^(pfocus|button|axis)' "$LOCKLOG" | sort -u | tr '\n' ' ')"
else
	fail "locktest 没收到完整的指针事件（pfocus/button/axis 有缺，共 $PEV 行）"
fi

# ------------------------------------------------- S3 语义①：DPMS 唤醒照旧
mark "S3 语义①：锁定期间 mouse_move_enables_dpms 照旧能唤醒关掉的屏"
msg output-power HEADLESS-1 off >/dev/null
sleep 0.4
WOKE_BEFORE=$(grep -ac "输入唤醒" "$LOG")
ENABLED_OFF=$(msg outputs | python3 -c '
import sys, json
print([o.get("enabled") for o in json.load(sys.stdin) if o.get("name") == "HEADLESS-1"])' 2>/dev/null)
msg move-cursor 20 20 >/dev/null
sleep 0.5
WOKE_AFTER=$(grep -ac "输入唤醒" "$LOG")
ENABLED_ON=$(msg outputs | python3 -c '
import sys, json
print([o.get("enabled") for o in json.load(sys.stdin) if o.get("name") == "HEADLESS-1"])' 2>/dev/null)
echo "    output-power off 后 enabled=$ENABLED_OFF；move-cursor 后 enabled=$ENABLED_ON"
if [ "$WOKE_AFTER" -gt "$WOKE_BEFORE" ] && [ "$ENABLED_ON" = "[True]" ]; then
	pass "指针一动屏就回来了：$(grep -a '输入唤醒' "$LOG" | tail -1 | sed 's/.*vidow: //')"
else
	fail "锁定期间指针没唤醒关掉的屏（输入唤醒 行数 $WOKE_BEFORE -> $WOKE_AFTER，enabled=$ENABLED_ON）"
fi
CFG_FFM=$(msg config focus_follows_mouse | python3 -c '
import sys, json
try:
    print(json.load(sys.stdin).get("value"))
except Exception:
    print("(读不到)")')
if [ "$CFG_FFM" = "true" ]; then
	pass "focus_follows_mouse 确实是 true（E 那条「不生效」才有意义）"
else
	fail "focus_follows_mouse = $CFG_FFM（不是 true；E 的判据就不成立）"
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

# ============================================================ 第三遍：F + 抓帧审计

mark "第三遍：锁定期间抓帧审计（方案 §4 风险 3）+ F 客户端崩溃保持锁定"
stop_clients
stop_vidow
start_vidow ""
start_bluewin
grim -o HEADLESS-1 "$RUNDIR/free1.png" 2>/dev/null &&
	pass "第三遍：锁前桌面抓到了（free1.png，审计的对照）" ||
	fail "第三遍：锁前 grim 抓帧失败"
LOCKLOG="$RUNDIR/lock3.log"
start_locktest "$RUNDIR/lock3.in" "$LOCKLOG"
if grep -aq LOCKED "$LOCKLOG"; then
	pass "第三遍：锁上了（LOCKED）"
else
	fail "第三遍没等到 LOCKED"
	tail -5 "$LOCKLOG" | sed 's/^/        /'
fi
sleep 0.5

# ---------------------------------------------------------------- 抓帧审计
#
# 为什么要实测（方案 §4 风险 3）：vidow 的输出抓帧是**自己渲染一帧**，不是把屏幕缓冲
# 拷给客户端。锁定期间那台「自己渲染」必须走同一棵「正常树已关」的场景图，否则
# grim / 录屏能拿到锁屏前的桌面。三条输出抓帧路径（代码路径见报告）：
#   ① zwlr_screencopy（grim）→ src/screencopy.c:388 → src/capture.c 的
#      vidow_capture_grab → capture_render_box_size → wlr_scene_output_build_state
#      （渲染的是 server->scene 整棵树，被 set_enabled(false) 的树天然不参与）
#   ② 连续帧 / damage 那条（wf-recorder 默认 copy_with_damage）→ 同一个 vidow_capture_grab
#   ③ ext-image-copy-capture 输出源（xdg-desktop-portal 截图）/ KDE screencast 输出流
#      → src/capture.c:226 与 src/screencast.c:1048/1069，同一个 vidow_capture_grab_into
# ① ② 用真客户端抓真像素；③ 没有便宜的免 portal 客户端，靠「和 ①② 同一个函数」这条
# 代码路径证据（本机没装 ext-image-copy-capture 的输出源客户端）。
mark "抓帧审计 ①：grim（zwlr_screencopy → src/screencopy.c → src/capture.c 自己渲染）"
if grim -o HEADLESS-1 "$RUNDIR/audit-grim.png" 2>"$RUNDIR/grim-audit.log"; then
	if check_not_desktop "$RUNDIR/audit-grim.png" "锁定期间 grim"; then
		pass "grim 抓出来的是锁屏内容，桌面像素 0（见上面数字）"
	else
		fail "grim 抓出来的帧里有桌面像素（抓帧绕过了被关掉的场景树）"
	fi
else
	fail "锁定期间 grim 抓帧失败（$(tail -1 "$RUNDIR/grim-audit.log")）"
fi

mark "抓帧审计 ②：wf-recorder 录 2 秒 → ffmpeg 抽一帧（screencopy 连续帧 / damage 路）"
if command -v wf-recorder >/dev/null && command -v ffmpeg >/dev/null; then
	setsid wf-recorder -o HEADLESS-1 -c libx264 -p crf=28 \
		-f "$RUNDIR/rec-locked.mkv" \
		> "$RUNDIR/rec.log" 2>&1 &
	RECPID=$!
	sleep 2
	kill -INT -- "-$RECPID" 2>/dev/null
	sleep 1.5
	if [ -s "$RUNDIR/rec-locked.mkv" ] &&
			ffmpeg -y -loglevel error -i "$RUNDIR/rec-locked.mkv" \
			-frames:v 1 "$RUNDIR/audit-rec.png" 2>"$RUNDIR/ffmpeg.log"; then
		if check_video_frame "$RUNDIR/free1.png" "$RUNDIR/audit-rec.png" \
				"锁定期间录像第 1 帧"; then
			pass "录像里那一帧是锁屏内容、和锁前桌面几乎不相同（见上面数字）"
		else
			fail "录像里有桌面像素（录屏那条路漏了）"
		fi
	else
		fail "wf-recorder / ffmpeg 这一路没拿到帧（$(tail -2 "$RUNDIR/rec.log" | tr '\n' ' ')）"
	fi
else
	echo "    （跳过：没装 wf-recorder 或 ffmpeg）"
fi

# ---------------------------------------------------------------- F
mark "F 锁定期间 kill -9 locktest → 按协议**不解锁**（黑屏死锁，XML:111-113）"
WS_BEFORE=$(active_ws)
FOC_BEFORE=$(focused_win)
kill -9 "$LOCK_PID" 2>/dev/null
LOCK_PID=""
sleep 0.8
if kill -0 "$VPID" 2>/dev/null; then
	pass "客户端被 kill -9 后 vidow 还活着"
else
	fail "客户端被 kill -9 把 vidow 搞崩了"
fi
if grep -a "客户端崩溃 / 被 kill" "$LOG" >/dev/null; then
	pass "日志：$(grep -a '客户端崩溃 / 被 kill' "$LOG" | tail -1 | sed 's/.*sessionlock: //')"
else
	fail "日志里没有「客户端崩溃 / 被 kill，按协议保持锁定」"
fi
if grim -o HEADLESS-1 "$RUNDIR/dead.png" 2>"$RUNDIR/grim-dead.log"; then
	if check_not_desktop "$RUNDIR/dead.png" "崩溃后 HEADLESS-1"; then
		pass "崩溃后屏上还是锁屏内容 / 纯黑，桌面像素 0（没自动解锁）"
	else
		fail "崩溃后桌面漏回来了（自动解锁了？）"
	fi
else
	fail "崩溃后 grim 抓帧失败（$(tail -1 "$RUNDIR/grim-dead.log")）"
fi
# 键位仍然不生效：**真键盘**那条路（wtype 造虚拟键盘 → input.c 的 keyboard_key
# → keybind 的锁定白名单）按已绑定的键 → 工作区 / 焦点不变，日志里仍有「不生效」。
# 这里不用 vidowmsg send-key：注入那条路**平时本来就不触发键位表**（见 input.c 的
# inject_key），只有锁定期才被特意接到白名单上 —— 用它量「解锁后键位恢复」量不到。
wtype -M logo -k j >/dev/null 2>&1
wtype -M logo -k j >/dev/null 2>&1
sleep 0.4
WS_AFTER=$(active_ws)
FOC_AFTER=$(focused_win)
echo "    工作区：$WS_BEFORE  ->  $WS_AFTER"
echo "    焦点：  $FOC_BEFORE  ->  $FOC_AFTER"
if [ "$WS_BEFORE" = "$WS_AFTER" ] && [ "$FOC_BEFORE" = "$FOC_AFTER" ]; then
	pass "崩溃后真键盘按已绑定的键仍不生效（工作区 / 焦点没变）"
else
	fail "崩溃后真键盘按已绑定的键生效了（锁定态没守住）"
fi
grep -a "锁定期间不生效" "$LOG" | grep -aq "Mod+J" &&
	pass "日志里仍有「绑定 Mod+J … 锁定期间不生效」（键照旧被白名单挡）" ||
	fail "崩溃后日志里没有「锁定期间不生效」—— 键可能漏给了别的路径"
if grep -a 'sessionlock:' "$LOG" | grep -aqE 'ERROR' ; then
	# 崩溃这条路**故意**打一条 ERROR（那就是判据），只看有没有别的异常
	:
fi

# ============================================================ 第四遍：H 解锁恢复

mark "第四遍：H 解锁恢复（locktest stdin 写 u → 桌面回来 + 键位恢复）"
stop_clients
stop_vidow
start_vidow ""
start_bluewin
sleep 0.5
grim -o HEADLESS-1 "$RUNDIR/h-free1.png" 2>/dev/null &&
	grim -o HEADLESS-2 "$RUNDIR/h-free2.png" 2>/dev/null &&
	pass "解锁前的桌面抓到了（h-free1.png / h-free2.png）" ||
	fail "解锁前 grim 抓帧失败"
WS_BEFORE=$(active_ws)
FOC_BEFORE=$(focused_win)
LOCKLOG="$RUNDIR/lock4.log"
start_locktest "$RUNDIR/lock4.in" "$LOCKLOG"
if grep -aq LOCKED "$LOCKLOG"; then
	pass "第四遍：锁上了（LOCKED）"
else
	fail "第四遍没等到 LOCKED"
	tail -5 "$LOCKLOG" | sed 's/^/        /'
fi
sleep 0.5
grim -o HEADLESS-1 "$RUNDIR/h-locked1.png" 2>/dev/null
# 锁定期先用**真键盘**按一次那个已绑定的键：工作区不许动，而且日志里要留下
# 「锁定期间不生效」—— 解锁后同一个动作必须反过来（这样 before/after 才是同一把尺）。
BLOCKED_BEFORE=$(grep -ac "锁定期间不生效" "$LOG")
wtype -M logo -k j >/dev/null 2>&1
sleep 0.4
BLOCKED_AFTER=$(grep -ac "锁定期间不生效" "$LOG")
if [ "$(active_ws)" = "$WS_BEFORE" ] && [ "$BLOCKED_AFTER" -gt "$BLOCKED_BEFORE" ]; then
	pass "锁定期真键盘按 Mod+J：工作区没动，日志多了「锁定期间不生效」（$BLOCKED_BEFORE -> $BLOCKED_AFTER）"
else
	fail "锁定期真键盘按 Mod+J 的结果不对（工作区 $(active_ws)，不生效行 $BLOCKED_BEFORE -> $BLOCKED_AFTER）"
fi
# 往 locktest 的 stdin（fifo，脚本这边是 fd 9）写 u = unlock_and_destroy
printf 'u\n' >&9
for _ in $(seq 1 50); do grep -aq "unlock-sent" "$LOCKLOG" && break; sleep 0.1; done
grep -aq "unlock-sent" "$LOCKLOG" &&
	pass "locktest 发了 unlock_and_destroy" ||
	fail "locktest 没打出 unlock-sent"
for _ in $(seq 1 50); do grep -aq "sessionlock: 解锁了" "$LOG" && break; sleep 0.1; done
if grep -a "sessionlock: 解锁了" "$LOG" >/dev/null; then
	pass "日志：$(grep -a 'sessionlock: 解锁了' "$LOG" | tail -1 | sed 's/.*sessionlock: //')"
else
	fail "日志里没有「解锁了，桌面树恢复」"
	tail -8 "$LOG" | sed 's/^/        /'
fi
sleep 0.6
if grim -o HEADLESS-1 "$RUNDIR/h-after1.png" 2>"$RUNDIR/grim-h.log" &&
		grim -o HEADLESS-2 "$RUNDIR/h-after2.png" 2>>"$RUNDIR/grim-h.log"; then
	if compare_desktop "$RUNDIR/h-free1.png" "$RUNDIR/h-after1.png" "HEADLESS-1 解锁后 vs 锁前" &&
			compare_desktop "$RUNDIR/h-free2.png" "$RUNDIR/h-after2.png" "HEADLESS-2 解锁后 vs 锁前"; then
		pass "两块屏的桌面都回来了（逐像素相同 ≥99%）"
	else
		fail "解锁后画面和锁前对不上（见上面数字）"
	fi
	# 锁中那张得和锁前不一样，否则上面那条「对上了」没有意义
	if python3 - "$RUNDIR/h-free1.png" "$RUNDIR/h-locked1.png" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
print("    锁中 vs 锁前最大差 %d" % int(np.abs(a - b).max()))
sys.exit(0 if int(np.abs(a - b).max()) > 0 else 1)
PY
	then
		pass "锁定期间那一帧确实不是桌面（和锁前不同）"
	else
		fail "锁中那张和锁前一样（锁没生效？H 的「桌面回来了」就没证据）"
	fi
else
	fail "解锁后 grim 抓帧失败（$(tail -1 "$RUNDIR/grim-h.log")）"
fi
# 键位恢复：**真键盘**按同一个已绑定的键 → 工作区真的切了（锁定期那次是「没动」）
BLOCKED_UNLOCK=$(grep -ac "锁定期间不生效" "$LOG")
wtype -M logo -k j >/dev/null 2>&1
sleep 0.5
WS_AFTER=$(active_ws)
FOC_AFTER=$(focused_win)
echo "    工作区：$WS_BEFORE  ->  $WS_AFTER"
echo "    焦点：  $FOC_BEFORE  ->  $FOC_AFTER"
if [ "$WS_AFTER" != "$WS_BEFORE" ]; then
	pass "解锁后真键盘按同一个已绑定的键生效了（工作区真的切了：$WS_BEFORE -> $WS_AFTER）"
else
	fail "解锁后真键盘按同一个已绑定的键没生效（白名单还拦着？键位没恢复？）"
fi
if [ "$(grep -ac '锁定期间不生效' "$LOG")" = "$BLOCKED_UNLOCK" ]; then
	pass "解锁后不再有新的「锁定期间不生效」（白名单不再拦，共 $BLOCKED_UNLOCK 行都是锁定期留下的）"
else
	fail "解锁后还新增了「锁定期间不生效」：$(grep -a '锁定期间不生效' "$LOG" | tail -1 | sed 's/.*sessionlock: //')"
fi
if kill -0 "$VPID" 2>/dev/null; then
	pass "解锁全程 vidow 没崩"
else
	fail "解锁把 vidow 搞崩了"
fi
# 解锁后 lock 那两棵树必须关掉，不然锁屏内容会盖在桌面上
if python3 - "$RUNDIR/h-after1.png" <<'PY'
import sys
import numpy as np
from PIL import Image
LOCK = np.array((0x22, 0x66, 0x99), dtype=np.int16)
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
total = a.shape[0] * a.shape[1]
lock = int((np.abs(a - LOCK).max(axis=2) <= 2).sum())
print("    解锁后锁屏色占比 %.1f%%" % (lock * 100.0 / total))
sys.exit(0 if lock < total * 0.5 else 1)
PY
then
	pass "解锁后 layer_lock 那两棵树的节点不再画出来（锁屏色 <50%）"
else
	fail "解锁后屏上还有大块锁屏色（lock 树没关掉？）"
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
	echo "test-sessionlock.sh 全部通过（S1：能锁、能黑屏、能握手；S2：lock surface 生命周期 + 输出热插拔；S3：输入路由 + 键位白名单；S4：能解锁 / 崩溃保持锁定 / 抓帧不泄桌面）"
else
	echo "test-sessionlock.sh 有失败项"
fi
exit "$FAILED"
