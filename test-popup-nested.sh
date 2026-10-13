#!/bin/bash
# 嵌套（二级）弹出物的摆位：它必须贴着**父弹出物**，不能相对顶层窗口摆。
#
# 修之前：server_new_popup 把每个弹出物的场景树都挂在「父面所属的**顶层窗口**」
# 树上（view_from_surface 一路沿 popup->parent 爬到 toplevel）。一级弹出物这样
# 挂是对的 —— wlroots 给的节点坐标（popup->current.geometry，见
# types/scene/xdg_shell.c 的 scene_xdg_surface_update_position）本来就是相对
# **父表面**算的，而一级弹出物的父表面就是顶层窗口。二级弹出物的父表面是**一级
# 弹出物**，坐标相对一级弹出物算，挂到顶层窗口的树上就等于把「一级菜单在窗口里
# 的位置」整个丢掉：二级菜单整体平移一段 (父菜单.x - 窗口.x, 父菜单.y - 窗口.y)。
# 实测（1920x1080、窗口 300,200、点 360,260）：一级菜单在 773,731（275x268），
# 二级菜单被摆到 561,200 —— 跑到窗口左上角，差 473 像素；正确位置是 1034,731
# （= 一级菜单右边缘 1048 减去 GTK 子菜单跟父菜单的 14px 重叠）。
#
# 判据（每轮）：
#   1) 前提：探针窗口起来了、摆到位
#   2) 一级菜单弹出来了（日志里有 mapped=1 的弹出物）
#   3) 二级菜单真的弹了：日志里出现**两个不同尺寸**的弹出物（一级 8 项 / 二级 2 项）
#   4) 二级贴着父菜单：一个的右边缘 ≈ 另一个的左边缘（容差 24px，GTK 的子菜单
#      跟父菜单有 ~14px 重叠），且两者的纵向范围相交（子菜单对着父菜单的某一项）
#   5) 两个弹出物都落在窗口那块屏里（没被摆到屏外）
#   6) 像素：菜单真的画出来了（屏幕上多出像素）
#   7) vidow 全程活着
#
# 两轮：输出 scale 1 和 scale 2（缩放只改像素密度，节点坐标是逻辑坐标，判据一样）。
#
# 用法：./test-popup-nested.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-popup-nested-test
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
	command -v "$tool" >/dev/null || { echo "test-popup-nested.sh: 需要 $tool"; exit 1; }
done
python3 -c "import gi" 2>/dev/null || { echo "test-popup-nested.sh: 需要 python3-gobject (GTK3)"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 不要壁纸：背景纯黑，屏幕上多出来的像素只可能是菜单
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

geom_of() { # <标题> -> "x y w h"
	./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
w = next((w for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), None)
if w is None:
    print("? ? ? ?")
else:
    g = w.get("float_geometry") or w.get("geometry")
    print("%d %d %d %d" % (g["x"], g["y"], g["width"], g["height"]))' "$1"
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
	setsid nohup python3 ./tools/menuprobe2.py >> "$RUNDIR/probe.log" 2>&1 </dev/null &
	CLIENT_PID=$!
	for _ in $(seq 1 40); do
		[ "$(geom_of menuprobe2)" != "? ? ? ?" ] && break
		sleep 0.25
	done
	sleep 0.8
}
stop_probe() {
	[ -n "$CLIENT_PID" ] && kill "$CLIENT_PID" 2>/dev/null
	CLIENT_PID=""
	for _ in $(seq 1 20); do
		[ "$(geom_of menuprobe2)" = "? ? ? ?" ] && break
		sleep 0.1
	done
}

# 一轮：把探针摆到 (wx,wy)，点一下弹一级菜单，指针落进第一项（带子菜单那项）
# 把二级菜单勾出来，然后量两个弹出物在场景里的位置。
run_round() { # <说明> <窗口x> <窗口y> <逻辑屏宽> <逻辑屏高>
	local tag=$1 wx=$2 wy=$3 ow=$4 oh=$5
	mark "$tag：探针窗口 $wx,$wy（逻辑屏 ${ow}x${oh}），点开一级菜单再勾出二级菜单"

	stop_probe
	start_probe
	./build/vidowmsg set-window-position --title menuprobe2 "$wx" "$wy" >/dev/null 2>&1
	sleep 0.8
	read -r GX GY GW GH <<< "$(geom_of menuprobe2)"
	if [ "$GX" = "?" ]; then
		fail "$tag: 探针窗口没起来（menuprobe2.py）"
		return
	fi
	echo "    窗口盒 $GX,$GY ${GW}x${GH}"
	# 摆没摆到 wx,wy 不断言：平铺窗口的位置由布局说了算（set-window-position 对
	# 平铺窗口不生效），点哪儿跟着**实际**盒子算，断言才有意义。
	if [ "$GX" -ge 0 ] && [ "$GY" -ge 0 ] &&
			[ $((GX + GW)) -le "$ow" ] && [ $((GY + GH)) -le "$oh" ]; then
		pass "探针窗口在屏里（$GX,$GY ${GW}x${GH} ∈ ${ow}x${oh}）"
	else
		fail "探针窗口不在屏里：$GX,$GY ${GW}x${GH}（屏 ${ow}x${oh}）"
	fi

	local before="$tag-before.png"
	local after="$tag-after.png"
	grim -o HEADLESS-1 "$RUNDIR/$before" 2>/dev/null

	local n1=$(wc -l < "$LOG")
	# 一级菜单按指针弹：弹出来的菜单左上角就在指针那儿，指针因此正落在第一项
	# （带子菜单那项）上；再往项里挪一点，GTK 才起延时把二级菜单勾出来。
	local cx=$((GX + 60)) cy=$((GY + 60))
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
# 纵向相交：子菜单对着父菜单的某一项（不是跑到窗口角上去）
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

run_round scale1 10 10 1920 1080
./build/vidowmsg output-scale HEADLESS-1 2 >/dev/null 2>&1
sleep 1.0
run_round scale2 10 10 960 540

exec 9>&-
stop_probe
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "vidow 全程活着"
else
	fail "vidow 挂了"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-popup-nested.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
