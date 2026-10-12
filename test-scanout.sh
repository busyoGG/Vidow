#!/bin/bash
# 全屏直通（direct scanout）：全屏窗口只要**不需要装饰**，就别烘那趟离屏渲染 ——
# 场景里只剩客户端表面这一个节点，wlroots 才有机会把客户端的缓冲直接扫上屏
# （wlr_scene.c 的 scene_entry_try_direct_scanout 要求这一帧的 render list 正好一项）。
# 省掉的是每帧「一次整屏离屏渲染 + 一次输出合成」：全屏看视频 / 打游戏时是白拿的
# 帧率，也少一趟 4K 全屏的 GPU 带宽。
#
# 判据在 fx.c 的 vidow_view_scanout_ok，切换在 view.c 的 view_scanout_switch。
# **进出直通都必须看不出变化**（用户要求：「回退的时候屏幕不要黑一下」）：
#   * 进：先把真实表面露出来（内容一样），同一帧把装饰节点收掉；
#   * 退：先把装饰那条路接回来，烘成了才把表面藏起来（view_anim_bake 尾那句）。
# 判据成立时「合成出来的像素」和「客户端缓冲本身」是同一个东西，所以两条路的截图
# 该逐像素一样（唯一例外见第 4 条）。
#
# 判据：
#   1) 全屏 + 装饰开着（默认配置，blur_all 也开着）→ 日志出现
#      `scanout: … 交给场景直画`，紧跟着 wlroots 的 `Direct scan-out enabled`
#   2) 退全屏 → `scanout: … 回到合成` + `Direct scan-out disabled`（回得来）
#   3) 非全屏窗口一次都不进直通（别把装饰顺手关了）
#   4) 两条路的像素：**内圈逐像素一致**。最外 1px 允许有差 —— 装饰着色器的边缘
#      抗锯齿（radius 0 时 smoothstep(-1,1,d) 让最外那圈像素落在 0.84 上），全屏
#      窗口贴到屏幕边，这圈就在屏幕最外沿；两个方向都是静态的 1px，不是闪一下。
#   5) 切换过程中窗口不消失：退全屏前后连拍，窗口里那一点一直是窗口内容，不是兜底色
#   6) VIDOW_NO_SCANOUT=1 真的能关掉（探针）：日志里没有 scanout、wlroots 也不直通
#
# 用法：./test-scanout.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试**并行**跑 —— 收尾按 cmdline 杀 pattern.py / grim。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-scanout-test
CFGDIR="$RUNDIR/home"
LOG=""
FAILED=0
VIDOW_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*grim*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

stop_vidow() {
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	VIDOW_PID=""
}

cleanup() {
	stop_clients
	stop_vidow
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi" 2>/dev/null || { echo "test-scanout.sh: 需要 python3-gobject"; exit 1; }
python3 -c "from PIL import Image" 2>/dev/null || { echo "test-scanout.sh: 需要 python3-pil"; exit 1; }
command -v grim >/dev/null || { echo "test-scanout.sh: 需要 grim"; exit 1; }

rm -rf "$RUNDIR"
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 默认配置（装饰开着、blur_all 默认开）—— 直通要在默认配置下就成立
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

msg() { ./build/vidowmsg "$@" >/dev/null 2>&1; }

start_vidow() { # <标签> [额外 env]
	LOG="$RUNDIR/$1.log"
	# shellcheck disable=SC2086
	env ${2:-} setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
	grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来（$1）"; tail -5 "$LOG"; exit 1; }
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
	sleep 0.5
	msg output-mode HEADLESS-1 1280 720 60
	sleep 0.6
}

# 那一小块里最多的颜色（pattern.py 四象限：左上红 / 右上绿 / 左下蓝 / 右下白）
dominant() { # <png> <x> <y> [边长]
	python3 - "$1" "$2" "$3" "${4:-20}" <<'PY'
import collections, sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
x, y, n = int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
c = collections.Counter()
for px in range(x - n // 2, x + n // 2):
    for py in range(y - n // 2, y + n // 2):
        c[im.getpixel((px, py))] += 1
r, g, b = c.most_common(1)[0][0]
if r < 80 and g < 80 and b > 180:
    print("蓝")
elif r > 180 and g < 80 and b < 80:
    print("红")
elif g > 180 and r < 80 and b < 80:
    print("绿")
elif r > 200 and g > 200 and b > 200:
    print("白")
else:
    print("其它(%d,%d,%d)" % (r, g, b))
PY
}

count() { local n; n=$(grep -ac "$1" "$LOG" 2>/dev/null); echo "${n:-0}"; }

echo "=================== 第一趟：默认配置（该直通） ==================="
start_vidow scanout
python3 ./tools/pattern.py w1 > "$RUNDIR/w1.log" 2>&1 &
sleep 2

mark "1) 非全屏的时候不许进直通"
if [ "$(count 'scanout:')" = "0" ]; then
	pass "平铺窗口没进直通（scanout: 0 次）"
else
	fail "平铺窗口就进直通了（$(count 'scanout:') 次）"
fi

mark "2) 全屏 → 交给场景直画 + wlroots 真的直通"
msg toggle-fullscreen --title w1
sleep 1.5
if grep -aq "scanout: w1 交给场景直画" "$LOG"; then
	pass "日志：scanout: w1 交给场景直画"
else
	fail "日志里没有「交给场景直画」"
fi
if grep -aq "Direct scan-out enabled" "$LOG"; then
	pass "wlroots：Direct scan-out enabled（真的直通了）"
else
	fail "wlroots 没有直通（render list 还是多于一项？）"
fi

mark "3) 全屏截图（这一张要拿去和合成那条路比）"
grim "$RUNDIR/full-scanout.png" >/dev/null 2>&1
got=$(dominant "$RUNDIR/full-scanout.png" 300 300)
if [ "$got" = "红" ]; then
	pass "全屏窗口铺满整块屏（300,300 是左上象限的红）"
else
	fail "全屏窗口没铺满（300,300 = $got）"
fi

mark "4) 退全屏：切换过程中窗口不许消失（连拍 6 张看那一点）"
msg toggle-fullscreen --title w1
bad=0
for i in 1 2 3 4 5 6; do
	grim "$RUNDIR/exit-$i.png" >/dev/null 2>&1
	c=$(dominant "$RUNDIR/exit-$i.png" 300 300 8)
	[ "$c" = "红" ] || { bad=$i; echo "     第 $i 张：$c"; }
done
if [ "$bad" = "0" ]; then
	pass "退全屏全程那一点都是窗口内容（没有黑一下 / 没有露兜底色）"
else
	fail "退全屏第 $bad 张采样点不是窗口内容（切换时画面空了）"
fi
sleep 1
if grep -aq "scanout: w1 回到合成" "$LOG"; then
	pass "日志：scanout: w1 回到合成"
else
	fail "日志里没有「回到合成」"
fi
if grep -aq "Direct scan-out disabled" "$LOG"; then
	pass "wlroots：Direct scan-out disabled（退回来了）"
else
	fail "wlroots 没报 disabled"
fi
if [ "$(count 'scanout:')" = "2" ]; then
	pass "进出各一次（scanout: 2 次）"
else
	fail "scanout: 出现 $(count 'scanout:') 次，期望 2 次"
fi

stop_clients
stop_vidow

echo
echo "=================== 第二趟：VIDOW_NO_SCANOUT=1（不许直通） ==================="
start_vidow compose "VIDOW_NO_SCANOUT=1"
python3 ./tools/pattern.py w1 > "$RUNDIR/w1b.log" 2>&1 &
sleep 2
msg toggle-fullscreen --title w1
sleep 1.5
if [ "$(count 'scanout:')" = "0" ]; then
	pass "探针把直通关掉了（scanout: 0 次）"
else
	fail "VIDOW_NO_SCANOUT=1 没关掉（scanout: $(count 'scanout:') 次）"
fi
if grep -aq "Direct scan-out enabled" "$LOG"; then
	fail "wlroots 还是直通了"
else
	pass "wlroots 没直通（走的合成）"
fi
grim "$RUNDIR/full-compose.png" >/dev/null 2>&1
stop_clients
stop_vidow

mark "5) 两条路的像素：内圈该逐像素一样"
python3 - <<'PY'
from PIL import Image
a = Image.open('/tmp/vidow-scanout-test/full-scanout.png').convert('RGB')
b = Image.open('/tmp/vidow-scanout-test/full-compose.png').convert('RGB')
w, h = a.size
pa, pb = a.load(), b.load()
inner = 0
ring = 0
worst = 0
for y in range(h):
    for x in range(w):
        if pa[x, y] != pb[x, y]:
            d = max(abs(pa[x, y][i] - pb[x, y][i]) for i in range(3))
            worst = max(worst, d)
            if x < 2 or y < 2 or x >= w - 2 or y >= h - 2:
                ring += 1
            else:
                inner += 1
print('  尺寸 %dx%d' % (w, h))
print('  内圈差异像素：%d' % inner)
print('  最外 2px 差异像素：%d（最大差 %d/255）' % (ring, worst))
raise SystemExit(1 if inner else 0)
PY
if [ $? = "0" ]; then
	pass "直通和合成画出来的像素在内圈完全一样"
else
	fail "内圈就有差异 —— 直通那条路画的东西和合成不一样"
fi

echo
if [ "$FAILED" = "0" ]; then
	echo "全部通过（日志：$RUNDIR）"
else
	echo "有失败项（日志：$RUNDIR）"
fi
exit $FAILED
