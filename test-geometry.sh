#!/bin/bash
# 「窗口几何」和表面尺寸对不上的那个中间态：窗口框不动，内容铺满框。
#
# 用户报的「窗口会先渲染小一点然后才按照缩放显示」（chromium 系）：客户端改大小
# 的一两秒里，**表面**已经换成新尺寸、声明的窗口几何（xdg_surface.set_window_geometry）
# 还是原来那个大尺寸 —— wlroots 只在**声明那一刻**把几何夹进当时的表面，之后表面
# 缩小它不管。vidow 要是照旧按声明框采内容，UV 会跑到 1 外面被 clamp 成「窗口框
# 到位了、里面的画面小一圈、右边一条糊边」。
#
# 靶子：tools/geowin（左红中绿右蓝 + 正中间一条白线；白线位置看得出内容有没有被挤）。
#   --shrink=200x150：先 300x250 上屏（几何声明 300x250），再换成 200x150 的缓冲，
#   **不动**几何 —— 这就是那个中间态。
#
# 判据：
#   1) 日志点名「几何落在表面外面」（说明真的复现了那个中间态）
#   2) 窗口框还在声明尺寸上（float_geometry 300x250），不是缩到表面那么大
#   3) 缩小之后那一帧（最后一份装饰缓冲 dump）里白线在内容正中（~0.5；
#      被 clamp 的老行为是 0.333 —— 内容挤在左边 2/3、右边一条糊边）
#   4) 哨兵：缩小之前那一帧（dump 0）白线也在正中（这条判据本身立得住）
#   5) 日志里没有 assert / abort
#
# 用法：./test-geometry.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-geometry-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
GEO_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$GEO_PID" ] && kill "$GEO_PID" 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT
trap 'exit 143' TERM INT

[ -x ./build/geowin ] || { echo "test-geometry.sh: 先构建（build/geowin）"; exit 1; }

rm -rf "$RUNDIR"
rm -f /tmp/vidow-fx-*.raw /tmp/vidow-fx-*.txt
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
gaps = 0
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
blur_enabled = false
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
export VIDOW_FX_DUMP=64

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
sleep 0.4

# dump 里白线的相对位置（0.5 = 内容正中间）
line_pos() { # <dump 序号> -> 相对位置
	python3 - "$1" <<'PY'
import sys
import numpy as np
i = sys.argv[1]
try:
    w, h, fmt = open("/tmp/vidow-fx-%s.txt" % i).read().split()
except OSError:
    print("none")
    sys.exit()
w, h = int(w), int(h)
a = np.frombuffer(open("/tmp/vidow-fx-%s.raw" % i, "rb").read(),
                  dtype=np.uint8).reshape(h, w, 4)
R, G, B = a[:, :, 2].astype(int), a[:, :, 1].astype(int), a[:, :, 0].astype(int)
content = (R > 120) | (G > 120) | (B > 120)
xs = np.where(content.any(axis=0))[0]
white = (R > 180) & (G > 180) & (B > 180)
wx = np.where(white.any(axis=0))[0]
if not xs.size or not wx.size:
    print("none")
else:
    mid = (wx[0] + wx[-1]) / 2.0
    print("%.3f %d" % ((mid - xs[0]) / max(xs[-1] - xs[0] + 1, 1),
                       xs[-1] - xs[0] + 1))
PY
}

mark "1) 上屏（300x250，几何也是 300x250），然后表面缩成 200x150"
./build/geowin shrink 300 250 300 250 --fixed --shrink=200x150 --linger=4 \
	> "$RUNDIR/geo.log" 2>&1 &
GEO_PID=$!
sleep 2.6
read -r BX BY BW BH <<<"$(./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    if w["title"] != "shrink":
        continue
    g = w["float_geometry"] if w["floating"] else w["geometry"]
    print(g["x"], g["y"], g["width"], g["height"])')"
NDUMPS=$(ls /tmp/vidow-fx-*.txt 2>/dev/null | wc -l)
kill "$GEO_PID" 2>/dev/null; GEO_PID=""
sleep 0.4

if [ "$NDUMPS" -lt 2 ]; then
	fail "只有 $NDUMPS 份装饰缓冲 dump：这条测不到东西（客户端上屏了吗？）"
else
	read -r FPOS FCONTENT <<<"$(line_pos $((NDUMPS - 1)))"
	# 哨兵取缩小之前**第一份有内容的** dump（第一帧还没上屏，是空的）
	SPOS=none
	for i in $(seq 0 $((NDUMPS - 2))); do
		read -r P C <<<"$(line_pos "$i")"
		if [ "$P" != "none" ]; then
			SPOS=$P
			break
		fi
	done

	if [ "$(grep -ac "窗口几何落在表面外面" "$LOG")" -ge 1 ]; then
		pass "这个中间态真的复现了：$(grep -a "窗口几何落在表面外面" "$LOG" | head -1 | sed 's/.*] //')"
	else
		fail "日志里没有「几何落在表面外面」—— 中间态没复现"
	fi

	if [ "$BW" = "300" ] && [ "$BH" = "250" ]; then
		pass "窗口框留在声明尺寸上：$BW x $BH（没缩到表面的 200x150）"
	else
		fail "窗口框变成 $BW x $BH（该留在 300x250）"
	fi

	if [ "$SPOS" != "none" ] && python3 -c "import sys; sys.exit(0 if abs($SPOS - 0.5) < 0.12 else 1)"; then
		pass "哨兵：缩小之前白线在内容正中（$SPOS）"
	else
		fail "哨兵不成立：缩小之前白线在 $SPOS"
	fi

	if [ "$FPOS" != "none" ] && python3 -c "import sys; sys.exit(0 if abs($FPOS - 0.5) < 0.12 else 1)"; then
		pass "缩小之后内容仍铺满这块框（白线相对位置 $FPOS，内容宽 $FCONTENT）"
	else
		fail "缩小之后内容被挤到一边（白线 $FPOS，内容宽 $FCONTENT）—— 采样被 clamp 了"
	fi
fi

if grep -aqE "assert|abort|Assertion" "$LOG"; then
	fail "日志里有 assert / abort：$(grep -aE 'assert|abort|Assertion' "$LOG" | head -2)"
else
	pass "日志里没有 assert / abort"
fi

echo
[ "$FAILED" = "0" ] && echo "test-geometry.sh: 全部通过" || echo "test-geometry.sh: 有失败"
exit "$FAILED"
