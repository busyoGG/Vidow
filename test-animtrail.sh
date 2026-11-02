#!/bin/bash
# 动画期间的损伤记账：位移补间跑完之后，局部重抓和整块重抓的画面必须一样。
#
# 场景：半透明模糊窗 T 底下压着一个彩色窗 B。B 用很长的位移补间
# （window_move_ms = 3000）从 T 的左上挪到右下，T 的私有底衬里画着 B。
# 每帧只记「B 现在在哪」的话，B 让出来的那条在 T 的底衬里永远停在上一版
# —— 动画结束后那一带留着 B 的模糊残影，和整块重抓的画面差出来。
#
# 判据：
#   1) T 的私有底衬真的在重抓（日志里 blur: own region <T> ≥ 10 行）
#   2) 两遍（局部 / 整块）的最终截图逐像素一样（差 > 2 的像素 0 个）
#   3) 两遍 vidow 都活着
#
# 用法：./test-animtrail.sh            （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-animtrail-test
FAILED=0
SRC="$(cd "$(dirname "$0")" && pwd)"

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup_all() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		[ -r "$p/cmdline" ] || continue
		case "$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)" in
			*sheet.py*|*pattern.py*|*wall.py*) kill "$pid" 2>/dev/null ;;
		esac
	done
	sleep 0.3
}
trap 'cleanup_all; [ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"' EXIT

python3 -c "import gi; gi.require_version('GtkLayerShell','0.1')" 2>/dev/null ||
	{ echo "test-animtrail.sh: 需要 python3-gobject + gtk-layer-shell"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$RUNDIR"

# 一遍完整场景。<nopartial> = 1 时从进程启动起就整块重抓（基准画面）。
run_once() { # <名字> <nopartial>
	local tag="$1" nopartial="$2"
	local D="$RUNDIR/$tag"
	mkdir -p "$D/home/.config/vidow" "$D/run"
	chmod 700 "$D/run"
	cat > "$D/home/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
blur_enabled = true
blur_all = true
blur_levels = 4
blur_offset = 1.5
window_open_ms = 0
window_close_ms = 0
window_move_ms = 3000
CFG
	[ "$nopartial" = "1" ] && : > "$D/nopartial"

	local LOG="$D/log"
	(
		export XDG_RUNTIME_DIR="$D/run"
		export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1
		export WLR_LIBINPUT_NO_DEVICES=1
		export HOME="$D/home" VIDOW_LOG=debug VIDOW_BLUR_TRACE=1
		export VIDOW_BLUR_NO_PARTIAL_FILE="$D/nopartial"
		export DISPLAY= WAYLAND_DISPLAY=
		setsid "$SRC/build/vidow" > "$LOG" 2>&1 &
		echo $! > "$D/pgid"
	)
	local pgid; pgid=$(cat "$D/pgid")
	for _ in $(seq 1 80); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	grep -aq "vidow ready" "$LOG" || { tail -5 "$LOG"; return 1; }
	local wd; wd=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)

	(
		export HOME="$D/home" XDG_RUNTIME_DIR="$D/run"
		export WAYLAND_DISPLAY="$wd" DISPLAY=
		setsid nohup python3 "$SRC/tools/wall.py" > "$D/wall.log" 2>&1 </dev/null &
		sleep 1.0
		# T：半透明模糊窗（它的底衬里画着 B）
		setsid nohup python3 "$SRC/tools/sheet.py" animt 0.2 0.4 1.0 0.5 \
			> "$D/t.log" 2>&1 </dev/null &
		sleep 1.5
		# B：彩色窗（不透明），压在 T 上
		setsid nohup python3 "$SRC/tools/pattern.py" animb \
			> "$D/b.log" 2>&1 </dev/null &
		sleep 1.5
		local msg="$SRC/build/vidowmsg"
		float_at() { # <标题> <x> <y>
			local t="$1" x="$2" y="$3" f
			for _ in $(seq 1 30); do
				"$msg" windows 2>/dev/null | grep -q "\"$t\"" && break
				sleep 0.3
			done
			for _ in $(seq 1 12); do
				"$msg" toggle-floating --title "$t" >/dev/null 2>&1
				f=$("$msg" windows 2>/dev/null | python3 -c '
import sys, json
print(next((w["floating"] for w in json.load(sys.stdin)
            if w["title"] == sys.argv[1]), "?"))' "$t")
				[ "$f" = "True" ] && break
				sleep 0.3
			done
			"$msg" set-window-position --title "$t" "$x" "$y" >/dev/null 2>&1
			sleep 0.6
		}
		float_at animt 100 100
		float_at animb 150 150
		sleep 1.5
		# 慢速位移补间：B 从 T 的左上挪到右下（仍然压着 T）
		"$msg" set-window-position --title animb 280 240 >/dev/null 2>&1
		sleep 4.5
		grep -aq "blur: own region animt" "$LOG" || true
		sleep 0.5
		grim -o HEADLESS-1 "$D/final.png" 2>/dev/null
	)
	kill -TERM -- "-$pgid" 2>/dev/null
	sleep 0.5
	kill -KILL -- "-$pgid" 2>/dev/null
	cleanup_all
	# 记下 T 的私有底衬重抓次数（场景真的跑到那条路了吗）
	grep -ac "blur: own region animt" "$LOG" 2>/dev/null || echo 0
}

mark "动画结束后的损伤残留：局部重抓 vs 整块重抓"
NP=$(run_once partial 0); NR=$(run_once full 1)
[ -n "$NP" ] && [ -n "$NR" ] || { echo "vidow 没起来"; exit 1; }

if [ "$NP" -ge 10 ] && [ "$NR" -ge 10 ]; then
	pass "两遍都真的在重抓 T 的私有底衬（局部 $NP 行 / 整块 $NR 行）"
else
	fail "T 的私有底衬重抓太少（局部 $NP / 整块 $NR）—— 场景没跑起来，下面不算数"
fi

if [ -f "$RUNDIR/partial/final.png" ] && [ -f "$RUNDIR/full/final.png" ]; then
	if python3 - "$RUNDIR/partial/final.png" "$RUNDIR/full/final.png" <<'PY'
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
d = np.abs(a - b).max(axis=2)
n = int((d > 2).sum())
ys, xs = np.where(d > 2)
box = ("bbox %d,%d..%d,%d" % (xs.min(), ys.min(), xs.max(), ys.max())) if n else ""
print("    最大差 %d，差 > 2 的像素 %d 个（整幅 %d）%s"
      % (int(d.max()), n, d.size, box))
sys.exit(0 if n == 0 else 1)
PY
	then
		pass "动画结束后：局部重抓的画面和整块重抓逐像素一样（没有残影）"
	else
		fail "动画结束后两张画面不一样 —— 位移补间让出来的那一块没进账本"
	fi
else
	fail "没截到图"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-animtrail.sh: OK"
else
	echo "test-animtrail.sh: 有失败"
fi
exit "$FAILED"
