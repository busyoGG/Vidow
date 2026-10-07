#!/bin/bash
# xdg_popup 的 unconstrain 盒子：必须是「根 toplevel 表面自己的坐标系」里的可用区。
#
# 修之前：server_new_popup 直接把 wlr_output_layout_get_box() 的输出盒（**布局
# 坐标**）交给了 wlr_xdg_popup_unconstrain_from_box()，而 wlroots 要的是根 toplevel
# 表面自己的坐标系（它内部会减掉 popup 的 toplevel 坐标，见 wlr_xdg_popup.c）。
# 窗口不在布局原点 (0,0) 上时两者差着一整个窗口原点：客户端于是以为「可见区」在
# 它右边一整块屏的地方，把菜单摆到屏外 —— 微信主窗口在 DP-2（x=2880）上，右键菜单
# 的场景节点坐标是 4800,115（= 2880 + 1920，1920 就是那块屏的 x），而屏幕只有
# 0..3840，看着就是「弹窗不显示」。窗口贴在布局原点附近时这项差值为 0，所以
# 单屏 + 窗口在左上角测不出来（Hyprland 正常是因为它减了 t1ParentCoords）。
#
# 两轮跑同一套流程：窗口放在第二块屏（x=1920 那块）上、以及放在原点那块屏上。
# 后者是反向对照（修之前也过），前者是主判据（修之前菜单会跑到另一块屏上）。
#
# 判据（每轮）：
#   1) 前提：窗口真的落在目标那块屏上
#   2) 弹出物交了帧（popup: commit ... mapped=1）
#   3) unconstrain 用的盒子是**表面局部坐标**：box.x == 输出.x - 窗口.x
#   4) 弹出物的场景节点落在窗口那块屏的盒子里
#   5) 像素：窗口那块屏上真的多出菜单那块像素，另一块屏一个像素都没变
#   6) vidow 全程活着
#
# 用法：./test-popup2out.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-popup2out-test
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
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

for tool in grim python3; do
	command -v "$tool" >/dev/null || { echo "test-popup2out.sh: 需要 $tool"; exit 1; }
done
python3 -c "import gi" 2>/dev/null || { echo "test-popup2out.sh: 需要 python3-gobject (GTK3)"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 不要壁纸：背景纯黑，屏幕上多出来的像素只可能是菜单
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
gaps = 10
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1920x1080" }
vidow.output{ name = "HEADLESS-2", scale = 1, mode = "1920x1080" }
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_TEST_ACTION="file:$RUNDIR/actions"
mkfifo "$RUNDIR/actions"

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
	tail -1 | cut -d= -f2)
sleep 0.5

# 布局钉死：HEADLESS-2 在原点，HEADLESS-1 在它右边（headless 后端默认的先后顺序
# 不保证，位置写死后面才好算「盒子该是多少」）
./build/vidowmsg output-position HEADLESS-2 0 0 >/dev/null 2>&1
./build/vidowmsg output-position HEADLESS-1 1920 0 >/dev/null 2>&1
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
start_probe() {
	setsid nohup python3 ./tools/menuprobe.py >> "$RUNDIR/probe.log" 2>&1 </dev/null &
	CLIENT_PIDS="$CLIENT_PIDS $!"
	for _ in $(seq 1 40); do
		[ "$(geom_of menuprobe)" != "? ? ? ?" ] && break
		sleep 0.25
	done
	sleep 0.8
}
stop_probe() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	CLIENT_PIDS=""
	for _ in $(seq 1 20); do
		[ "$(geom_of menuprobe)" = "? ? ? ?" ] && break
		sleep 0.1
	done
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

# 一轮：把干净的客户端摆到 (wx,wy)，点一下弹菜单，量盒子 / 节点 / 像素
run_round() { # <说明> <窗口x> <窗口y> <目标输出名> <目标输出x> <另一块输出名>
	local tag=$1 wx=$2 wy=$3 oname=$4 ox=$5 other=$6
	mark "$tag：窗口放 $oname（$ox,0）上的 $wx,$wy"

	stop_probe
	start_probe
	./build/vidowmsg toggle-floating --title menuprobe >/dev/null 2>&1
	sleep 0.4
	./build/vidowmsg set-window-position --title menuprobe "$wx" "$wy" >/dev/null 2>&1
	sleep 0.8
	read -r GX GY GW GH <<< "$(geom_of menuprobe)"
	if [ "$GX" = "?" ]; then
		fail "$tag: 探针窗口没起来（menuprobe.py）"
		return
	fi
	echo "    窗口盒 $GX,$GY ${GW}x${GH}"
	if [ "$GX" = "$wx" ] && [ "$GY" = "$wy" ]; then
		pass "窗口摆到了 $wx,$wy（$oname）"
	else
		fail "窗口没摆到位：$GX,$GY（期望 $wx,$wy）"
	fi

	local before="$RUNDIR/$tag-$oname-before.png"
	local after="$RUNDIR/$tag-$oname-after.png"
	local otherb="$RUNDIR/$tag-$other-before.png"
	local otherc="$RUNDIR/$tag-$other-after.png"
	grim -o "$oname" "$before" 2>/dev/null
	grim -o "$other" "$otherb" 2>/dev/null

	local n1=$(wc -l < "$LOG")
	act "pointer-motion $((GX + GW / 2)) $((GY + GH / 2))" 0.4
	act "pointer-button left press" 0.3
	act "pointer-button left release" 0.4
	if wait_log "popup: commit" 5; then
		pass "弹出物交了帧（popup: commit）"
	else
		fail "弹出物一条 commit 都没有 —— 客户端压根没弹"
		return
	fi
	sleep 0.6
	grim -o "$oname" "$after" 2>/dev/null
	grim -o "$other" "$otherc" 2>/dev/null

	local seg="$RUNDIR/$tag-segment.log"
	sed -n "$((n1 + 1)),\$p" "$LOG" > "$seg"
	local pop=$(grep -a "popup: commit" "$seg" | grep -a "mapped=1" | tail -1)
	local box=$(grep -a "popup: unconstrain 完成" "$seg" | tail -1)
	echo "    $box"
	echo "    $pop"

	if [ -n "$box" ]; then
		read -r BX BY <<< "$(echo "$box" | sed 's/.*可用区 //' | sed 's/ [0-9]*x[0-9]*）.*//' |
			tr ',' ' ')"
		if [ "$BX" = "$((ox - GX))" ] && [ "$BY" = "$((0 - GY))" ]; then
			pass "unconstrain 盒子换算成了表面局部坐标（$BX,$BY = 输出 $ox,0 减窗口 $GX,$GY）"
		else
			fail "unconstrain 盒子不是表面局部坐标：$BX,$BY（期望 $((ox - GX)),$((0 - GY))）"
		fi
	else
		fail "日志里没有 unconstrain 那一行"
	fi

	if [ -n "$pop" ]; then
		read -r PX PY <<< "$(echo "$pop" | sed 's/.*在 //' | sed 's/（.*//' | tr ',' ' ')"
		echo "    弹出物节点在 $PX,$PY"
		if [ "$PX" -ge "$ox" ] && [ "$PX" -lt $((ox + 1920)) ] &&
				[ "$PY" -ge 0 ] && [ "$PY" -lt 1080 ]; then
			pass "弹出物落在窗口那块屏的盒子里（$PX,$PY ∈ $ox..$((ox + 1920)) × 0..1080）"
		else
			fail "弹出物被摆到屏外了：$PX,$PY（窗口在 $GX,$GY，$oname 是 $ox..$((ox + 1920))）"
		fi
	else
		fail "日志里没有弹出物的 commit 行"
	fi

	local d_own=$(changed_px "$(basename "$before")" "$(basename "$after")")
	local d_other=$(changed_px "$(basename "$otherb")" "$(basename "$otherc")")
	echo "    $oname 变化像素 $d_own，$other 变化像素 $d_other"
	if [ "$d_own" -ge 500 ]; then
		pass "菜单真的画在 $oname 上了（$d_own 像素变化）"
	else
		fail "$oname 几乎没变（$d_own 像素）—— 菜单没画出来"
	fi
	if [ "$d_other" -lt 500 ]; then
		pass "$other 没被牵连（$d_other 像素变化）"
	else
		fail "$other 也变了 $d_other 像素 —— 菜单跑到别的屏上了"
	fi
}

run_round 主判据 2200 300 HEADLESS-1 1920 HEADLESS-2
run_round 对照 200 200 HEADLESS-2 0 HEADLESS-1

exec 9>&-
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "vidow 全程活着"
else
	fail "vidow 挂了"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-popup2out.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
