#!/bin/bash
# 挂在 layer surface 上的弹出物再弹二级：子菜单必须贴着**父弹出物**摆。
#
# 背景：窗口那条路 7768fe7 修过（见 test-popup-nested.sh），layer 这条路当时没修。
# 靶子：tools/menuprobe-layer.py —— 一块 top 层的状态栏（layer surface）+ 一级菜单
# （第一项带子菜单）+ 二级子菜单（GTK3 用嵌套 xdg_popup 实现）。
#
# 这条路上有两个坑，修之前都踩了（都靠下面的判据卡住）：
#   1) 场景树挂错：wlroots 把弹出物节点坐标（popup->current.geometry）当「相对
#      **父表面**」的偏移，二级弹出物的父表面是**一级弹出物**，挂到状态栏那棵树
#      （或 layer_toplevels）上就等于把「一级菜单在状态栏里的位置」整段丢掉。
#      而且挂在 layer 表面上的弹出物是**两步**建的（wlroots 的 wlr_layer_shell_v1.c：
#      先 xdg_wm_base.get_xdg_popup(parent=NULL) 只发 xdg_shell 那条 new_popup，
#      再 zwlr_layer_surface_v1.get_popup 只发 layer 那条）—— 一级弹出物归 layer.c
#      管，**二级弹出物只有 xdg_shell 那条信号**，于是归 view.c 的 server_new_popup
#      管，它当时的 fallback 是 layer_toplevels。
#   2) 可用区（unconstrain）算成 0x0：view.c 那条路只在「父面是窗口」时算可用区，
#      二级弹出物（根是 layer 表面）走到那里 view == NULL，box 是 {0,0,0,0} ——
#      客户端拿到「可用区是个 0x0 的盒子」，就把子菜单摆到那个点上：屏幕左上角。
#
# 判据（每轮）：
#   1) 前提：状态栏（layer surface）上屏了、位置尺寸对
#   2) 一级菜单弹出来了（日志里有 mapped=1 的弹出物）
#   3) 二级菜单真的弹了：日志里出现**两个不同尺寸**的弹出物（一级 8 项 / 二级 2 项）
#   4) 二级贴着父菜单：一个的右边缘 ≈ 另一个的左边缘（容差 24px，GTK 的子菜单
#      跟父菜单有 ~14px 重叠），且两者的纵向范围相交
#   5) 两个弹出物都落在屏里
#   6) 像素：菜单真的画出来了（屏幕上多出像素）
#   7) vidow 全程活着；结尾进程组收干净
#
# 两轮：输出 scale 1 和 scale 2（缩放只改像素密度，节点坐标是逻辑坐标，判据一样）。
#
# 用法：./test-popup-layer.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-popup-layer-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
CLIENT_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

for tool in grim python3; do
	command -v "$tool" >/dev/null || { echo "test-popup-layer.sh: 需要 $tool"; exit 1; }
done
python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-popup-layer.sh: 需要 python3-gobject (GTK3) + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 不要壁纸：背景纯黑，屏幕上多出来的像素只可能是状态栏和菜单
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
gaps = 10
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1920x1080" }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export WLR_RENDERER_ALLOW_SOFTWARE=1
export HOME="$CFGDIR" VIDOW_LOG=debug DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.3}"; }

# 状态栏（layer surface）的盒子："x y w h"，没上屏就是 "? ? ? ?"
bar_geom() {
	./build/vidowmsg layers 2>/dev/null | python3 -c '
import sys, json
l = next((l for l in json.load(sys.stdin)
          if l["namespace"] == "menuprobe-layer" and l["mapped"]), None)
if l is None:
    print("? ? ? ?")
else:
    g = l["geometry"]
    print("%d %d %d %d" % (g["x"], g["y"], g["width"], g["height"]))'
}

wait_log() { # <关键字> <超时秒>
	for _ in $(seq 1 $((${2:-4} * 10))); do
		grep -aq "$1" "$LOG" && return 0
		sleep 0.1
	done
	return 1
}

# 日志里每个尺寸的弹出物取最后一条位置（同一个弹出物会 commit 好几次）：
# 输出每行「W H X Y」。尺寸不同 = 不同的弹出物（一级 8 项 / 二级 2 项）。
popup_boxes() { # <日志片段文件>
	python3 - "$1" <<'PY'
import re, sys
boxes = {}
for line in open(sys.argv[1], errors="replace"):
    m = re.search(r"popup: commit 缓冲 \d+x\d+ 逻辑 (\d+)x(\d+) 节点开着 在 "
                  r"(-?\d+),(-?\d+)（表面 mapped=1", line)
    if m:
        w, h, x, y = (int(v) for v in m.groups())
        boxes[(w, h)] = (x, y)
for (w, h), (x, y) in sorted(boxes.items()):
    print(w, h, x, y)
PY
}

changed_px() { # <a.png> <b.png> -> 变化像素数
	python3 - "$RUNDIR/$1" "$RUNDIR/$2" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert("RGB")).astype(np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert("RGB")).astype(np.int16)
print(int((np.abs(a - b).max(axis=2) > 8).sum()))
PY
}

start_probe() {
	setsid nohup python3 ./tools/menuprobe-layer.py >> "$RUNDIR/probe.log" 2>&1 \
		</dev/null &
	CLIENT_PID=$!
	for _ in $(seq 1 40); do
		[ "$(bar_geom)" != "? ? ? ?" ] && break
		sleep 0.25
	done
	sleep 0.8
}
stop_probe() {
	[ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
	CLIENT_PID=""
	for _ in $(seq 1 20); do
		[ "$(bar_geom)" = "? ? ? ?" ] && break
		sleep 0.1
	done
}

# 一轮：点状态栏弹一级菜单，指针落进第一项（带子菜单那项）把二级菜单勾出来，
# 然后量两个弹出物在场景里的位置。
run_round() { # <说明> <逻辑屏宽> <逻辑屏高>
	local tag=$1 ow=$2 oh=$3
	mark "$tag：状态栏（layer surface）+ 一级菜单 + 二级子菜单（逻辑屏 ${ow}x${oh}）"

	stop_probe
	start_probe
	read -r BX BY BW BH <<< "$(bar_geom)"
	if [ "$BX" = "?" ]; then
		fail "$tag: 状态栏没上屏（menuprobe-layer.py）"
		return
	fi
	echo "    状态栏盒 $BX,$BY ${BW}x${BH}"
	# 状态栏得在屏里，而且点击点（下面按 300,16 算）必须落在它里面 —— 不然菜单
	# 根本不会弹，后面的判据等于没测。
	if [ "$BX" -ge 0 ] && [ "$BY" -ge 0 ] &&
			[ $((BX + BW)) -le "$ow" ] && [ $((BY + BH)) -le "$oh" ]; then
		pass "状态栏在屏里（$BX,$BY ${BW}x${BH} ∈ ${ow}x${oh}）"
	else
		fail "状态栏不在屏里：$BX,$BY ${BW}x${BH}（屏 ${ow}x${oh}）"
	fi

	local before="$tag-before.png"
	local after="$tag-after.png"
	grim -o HEADLESS-1 "$RUNDIR/$before" 2>/dev/null

	local n1=$(wc -l < "$LOG")
	# 一级菜单按指针弹：弹出来的菜单左上角就在指针那儿，指针因此正落在第一项
	# （带子菜单那项）上；再往项里挪一点，GTK 才起延时把二级菜单勾出来。
	local cx=$((BX + 300)) cy=$((BY + 16))
	act "pointer-motion $cx $cy" 0.4
	act "pointer-button left press" 0.3
	act "pointer-button left release" 0.6
	if ! wait_log "popup: commit" 5; then
		fail "$tag: 弹出物一条 commit 都没有 —— 客户端压根没弹"
		return
	fi
	sleep 0.5
	act "pointer-motion $((cx + 20)) $((cy + 15))" 0.4
	sleep 1.6
	grim -o HEADLESS-1 "$RUNDIR/$after" 2>/dev/null

	local seg="$RUNDIR/$tag-segment.log"
	sed -n "$((n1 + 1)),\$p" "$LOG" > "$seg"
	local boxes=$(popup_boxes "$seg")
	local count=$(echo "$boxes" | grep -c .)
	echo "    弹出物（每行：逻辑 W H 节点X 节点Y）"
	echo "$boxes" | sed 's/^/      /'
	if [ "$count" -lt 2 ]; then
		fail "$tag: 只看到 $count 个弹出物 —— 二级菜单没弹出来，量不了"
	else
		# 判据交给 python 算（几何比较），它按 pass()/fail() 的格式打印，最后一行
		# VERDICT 0/1 是给这里记账用的。boxes 的每一行是「W H X Y」。
		local verdict=$(python3 - "$tag" $boxes <<'PY'
import sys
tag = sys.argv[1]
vals = [int(v) for v in sys.argv[2:]]
# (w, h, x, y) -> (x, y, w, h)
boxes = [(vals[i+2], vals[i+3], vals[i], vals[i+1])
         for i in range(0, len(vals), 4)]
TOL = 24  # GTK 的子菜单跟父菜单重叠 ~14px，留点余量
if len(boxes) != 2:
    print("  FAIL  %s: 弹出物不是两个（%d 个），判不了父子" % (tag, len(boxes)))
    print("VERDICT 1")
    sys.exit(0)
a, b = boxes
# 相邻：一个的右边缘贴着另一个的左边缘（左右哪边都行：贴边时 GTK 会把子菜单翻到另一侧）
adj = abs((a[0] + a[2]) - b[0]) <= TOL or abs((b[0] + b[2]) - a[0]) <= TOL
# 纵向相交：子菜单对着父菜单的某一项（不是跑到屏幕角上去）
overlap = max(a[1], b[1]) < min(a[1] + a[3], b[1] + b[3])
if adj:
    print("  PASS  %s: 二级弹出物贴着父弹出物（边缘差 %dpx ≤ %d）"
          % (tag, min(abs(a[0] + a[2] - b[0]), abs(b[0] + b[2] - a[0])), TOL))
else:
    print("  FAIL  %s: 二级弹出物没贴着父弹出物：%d,%d %dx%d 与 %d,%d %dx%d"
          "（边缘差 %d / %d px）"
          % (tag, a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3],
             abs(a[0] + a[2] - b[0]), abs(b[0] + b[2] - a[0])))
if overlap:
    print("  PASS  %s: 二级弹出物纵向对着父弹出物（%d..%d 与 %d..%d 相交）"
          % (tag, a[1], a[1] + a[3], b[1], b[1] + b[3]))
else:
    print("  FAIL  %s: 二级弹出物跑到别处了（纵向 %d..%d 与 %d..%d 不相交）"
          % (tag, a[1], a[1] + a[3], b[1], b[1] + b[3]))
print("VERDICT %d" % (0 if (adj and overlap) else 1))
PY
)
		echo "$verdict" | grep -v '^VERDICT'
		echo "$verdict" | grep -q '^VERDICT 0$' || FAILED=1
	fi

	# 两个弹出物都得在屏里（逻辑坐标，跟 scale 无关）
	if [ "$count" -ge 2 ]; then
		local off=$(echo "$boxes" | awk -v w="$ow" -v h="$oh" \
			'{ if ($3 < 0 || $4 < 0 || $3 >= w || $4 >= h) print "y" }')
		if [ -z "$off" ]; then
			pass "两个弹出物都在屏里（0..$ow × 0..$oh）"
		else
			fail "有弹出物被摆到屏外了（见上面的坐标）"
		fi
	fi

	local d=$(changed_px "$before" "$after")
	echo "    屏幕上变化像素 $d"
	if [ "$d" -ge 500 ]; then
		pass "菜单真的画出来了（$d 像素变化）"
	else
		fail "屏幕上几乎没变（$d 像素）—— 菜单没画出来"
	fi
}

run_round scale1 1920 1080
./build/vidowmsg output-scale HEADLESS-1 2 >/dev/null 2>&1
sleep 1.0
run_round scale2 960 540

exec 9>&-
stop_probe
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "vidow 全程活着"
else
	fail "vidow 挂了"
fi

# 收尾：harness 起的进程一个都不许留（只认我们自己的进程组，别碰用户那个 vidow）
cleanup
trap - EXIT
left=""
for p in $(pgrep -x vidow 2>/dev/null); do
	[ "$(ps -o pgid= -p "$p" 2>/dev/null | tr -d ' ')" = "$VIDOW_PGID" ] &&
		left="$left $p"
done
if [ -n "$left" ]; then
	fail "收尾没干净：我们起的 vidow 还留着$left"
else
	pass "收尾干净（进程组 $VIDOW_PGID 没有残留；机器上现存的 vidow 见下）"
fi
pgrep -a vidow | sed 's/^/    现存的 vidow: /' || echo "    现存的 vidow: 无"

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-popup-layer.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
