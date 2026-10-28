#!/bin/bash
# blur_xray：重叠时也用整屏那份底衬（壁纸 + layer 那层），忽略别的窗口 ——
# 观感上「糊的是壁纸，不是压着的那个窗口」，换来的是**不建私有底衬**（重叠场景
# 每帧从 N×(1+2·级数−1) 趟掉到 ~3 趟，私有底衬那块显存全免）。
#
# 场景：壁纸 + 一块**纯红**浮窗 + 一块压着它的半透明白浮窗（0.35）。白窗背后的
# 模糊里到底是谁，就是这条开关的全部区别。
#
# 判据（同一场景跑两遍，只差 blur_xray）：
#   1) 关（默认）：重叠的浮窗**建私有底衬**（整段日志的 own 次数 > 0）
#   2) 开：同一个场景 own == 0（一块私有底衬都不建），而且 vidow 活着、模糊照旧在画
#   3) 语义真的变了：白窗压着红窗的那块区域，两次截图里 mean(R−B) 差 ≥ 15
#      （关：红色渗进模糊；开：只有壁纸）
#
# 用法：./test-blurxray.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurxray-test
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
		case "$cmd" in *sheet.py*|*wall.py*) kill "$pid" 2>/dev/null ;; esac
	done 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v grim >/dev/null || { echo "test-blurxray.sh: 需要 grim"; exit 1; }
python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-blurxray.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }
python3 -c "import numpy, PIL" 2>/dev/null ||
	{ echo "test-blurxray.sh: 需要 python3-numpy + python3-pil"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug VIDOW_RENDER_STATS=1
export DISPLAY= WAYLAND_DISPLAY=

float_at() { # <标题> <x> <y> <宽> <高>
	local t="$1" x="$2" y="$3" w="$4" h="$5"
	for _ in $(seq 1 30); do
		./build/vidowmsg windows 2>/dev/null | grep -q "\"$t\"" && break
		sleep 0.3
	done
	for _ in $(seq 1 12); do
		./build/vidowmsg toggle-floating --title "$t" >/dev/null 2>&1
		local f=$(./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
print(next((w["floating"] for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), "?"))' "$t")
		[ "$f" = "True" ] && break
		sleep 0.3
	done
	./build/vidowmsg set-window-size --title "$t" "$w" "$h" >/dev/null 2>&1
	sleep 0.3
	./build/vidowmsg set-window-position --title "$t" "$x" "$y" >/dev/null 2>&1
	sleep 0.6
}

# 跑一遍场景。<xray: true|false> <标签> -> 截图存 $RUNDIR/<标签>.png，own 次数存 $RUNDIR/<标签>.own
run_phase() {
	local xray="$1" tag="$2"
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
vidow.output{ name = "HEADLESS-1", scale = 2, mode = "2560x1440" }
blur_enabled = true
blur_all = true
blur_levels = 3
blur_offset = 1.5
blur_xray = $xray
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	grep -aq "vidow ready" "$LOG" || {
		echo "vidow 没起来（xray=$xray）"; tail -5 "$LOG"; exit 1; }
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	sleep 0.5

	setsid nohup python3 ./tools/wall.py > "$RUNDIR/wall-$tag.log" 2>&1 </dev/null &
	CLIENTS="$CLIENTS $!"
	sleep 1.5
	# 底下那块：纯红、不透明
	setsid nohup python3 ./tools/sheet.py under 1 0 0 1 \
		> "$RUNDIR/under-$tag.log" 2>&1 </dev/null &
	CLIENTS="$CLIENTS $!"
	sleep 1.5
	float_at under 100 100 400 300
	# 压在上面那块：白的半透明，背后就是红窗
	setsid nohup python3 ./tools/sheet.py top 1 1 1 0.35 \
		> "$RUNDIR/top-$tag.log" 2>&1 </dev/null &
	CLIENTS="$CLIENTS $!"
	sleep 1.5
	float_at top 250 200 400 300
	# 让它动一下：私有底衬是「有损伤碰到我才重抓」，静止屏幕不会重抓
	for dx in 10 -10; do
		./build/vidowmsg set-window-position --title top $((250 + dx)) 200 \
			>/dev/null 2>&1
		sleep 0.4
	done
	./build/vidowmsg set-window-position --title top 250 200 >/dev/null 2>&1
	sleep 1.2

	grep -o 'own [0-9]*' "$LOG" | awk '{s+=$2} END {print s+0}' > "$RUNDIR/$tag.own"
	grep -o 'chain [0-9]*' "$LOG" | awk '{s+=$2} END {print s+0}' > "$RUNDIR/$tag.chain"
	grep -c "渲染统计" "$LOG" > "$RUNDIR/$tag.stats"
	grim -o HEADLESS-1 "$RUNDIR/$tag.png" 2>/dev/null
	kill -0 "$VIDOW_PGID" 2>/dev/null && echo alive > "$RUNDIR/$tag.alive" ||
		echo dead > "$RUNDIR/$tag.alive"

	for p in $CLIENTS; do kill "$p" 2>/dev/null; done
	CLIENTS=""
	kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.5
	kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	VIDOW_PGID=""
}

mark "blur_xray：重叠时用整屏底衬（忽略别的窗口），不建私有底衬"

run_phase false off
run_phase true on

OWN_OFF=$(cat "$RUNDIR/off.own")
OWN_ON=$(cat "$RUNDIR/on.own")
CHAIN_OFF=$(cat "$RUNDIR/off.chain")
CHAIN_ON=$(cat "$RUNDIR/on.chain")
echo "    关：own 累计 $OWN_OFF / chain 累计 $CHAIN_OFF / 统计 $(( $(cat "$RUNDIR/off.stats") )) 行"
echo "    开：own 累计 $OWN_ON / chain 累计 $CHAIN_ON / 统计 $(( $(cat "$RUNDIR/on.stats") )) 行"

if [ "$OWN_OFF" -gt 0 ]; then
	pass "关着的时候重叠浮窗确实建了私有底衬（own 累计 $OWN_OFF）"
else
	fail "关着的时候 own 累计是 0 —— 场景没复现「重叠」，判据 2 就没有意义"
fi
if [ "$OWN_ON" = "0" ]; then
	pass "开着的时候一块私有底衬都不建（own 累计 0）"
else
	fail "开着的时候还建了 $OWN_ON 次私有底衬"
fi
if [ "$CHAIN_ON" -gt 0 ]; then
	pass "开着的时候模糊照旧在跑（chain 累计 $CHAIN_ON，走的是整屏那条链）"
else
	fail "开着的时候一条链都没跑（模糊没了？）"
fi
[ "$(cat "$RUNDIR/off.alive")" = alive ] && [ "$(cat "$RUNDIR/on.alive")" = alive ] &&
	pass "两遍 vidow 都活着" || fail "有一遍 vidow 死了"

# 语义：白窗压红窗那块区域，mean(R-B) 两次差多少
if python3 - "$RUNDIR/off.png" "$RUNDIR/on.png" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
# 逻辑坐标 (320,250)-(420,350) -> 物理像素 ×2；避开白网格的边
box = (slice(510, 690), slice(650, 830))
ra = float((a[box][:, :, 0] - a[box][:, :, 2]).mean())
rb = float((b[box][:, :, 0] - b[box][:, :, 2]).mean())
print("    同一块区域 mean(R-B)：关 %.1f / 开 %.1f（差 %.1f）" % (ra, rb, ra - rb))
ok = (ra - rb) >= 15.0 and int(a[box].max()) > 0 and int(b[box].max()) > 0
sys.exit(0 if ok else 1)
PY
then
	pass "语义确实变了：关的时候红色渗进模糊，开的时候只有壁纸"
else
	fail "两遍截图的红蓝差不够 / 有一张是黑的 —— blur_xray 没真的换掉模糊源"
fi

echo
if [ "$FAILED" -eq 0 ]; then
	echo "test-blurxray.sh 全部通过"
else
	echo "test-blurxray.sh 有失败项"
fi
exit "$FAILED"
