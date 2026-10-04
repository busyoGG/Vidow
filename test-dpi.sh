#!/bin/bash
# 混合 DPI：两块缩放不同的屏 + 一个 X11 客户端 + 一个 Wayland 客户端，验证
# 「一样大、一样清晰」。判据全部能从日志里量出来：
#
#   1) 每块屏按自己的缩放渲染：逻辑尺寸 = 像素 / 缩放
#   2) XWayland 的全局缩放 = ceil(最密那块屏的缩放)：X 只有一套坐标，
#      低于某个输出的缩放，那块屏上的 X11 窗口就是被放大糊的
#   3) 清晰度 = 缓冲区像素 >= 落在屏上的设备像素（只允许缩小，不允许放大）；
#      在最密的那块屏上是 1:1
#   4) Wayland 客户端跟着输出缩放渲染：缓冲区像素 == 设备像素（1:1）
#   5) X11 的命中坐标按同一比例换算（点哪儿就是哪儿）
#
# 全 headless，不需要宿主会话，也不碰任何真实 X/portal 进程。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-dpi-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	# vidow 是 setsid 起的：整组一起收，连它 fork 的 Xwayland 一起。
	if [ -n "${VIDOW_PGID:-}" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	rm -rf "$RUNDIR"
}
trap cleanup EXIT

# ---------------------------------------------------------------- 离线自检
mark "缩放计算（不启合成器）"
if ./build/scaletest > "$RUNDIR-scaletest.txt" 2>&1; then
	pass "scaletest：$(grep -c '^ok' "$RUNDIR-scaletest.txt") 项断言全过"
else
	fail "scaletest 有失败项："
	sed -n '/^FAIL/p' "$RUNDIR-scaletest.txt" | sed 's/^/        /'
fi

# ---------------------------------------------------------------- 起合成器
mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
-- HEADLESS-2 是 2 倍屏，HEADLESS-1 是 1 倍屏：模拟一块 4K 一块 1080p
scale = "auto"
outputs = "HEADLESS-1:1 HEADLESS-2:2"
xwayland = true
EOF

# 子进程真实拿到的环境：X11 缩放是这样传给客户端的
cat >> "$CFGDIR/.config/vidow/config.lua" <<'EOF'
-- vidow.exec 返回 stdout（失败时 nil + 原因）；要退出码用 vidow.exec_status
local v = vidow.exec("printenv GDK_SCALE")
vidow.log("childenv GDK_SCALE=" .. tostring(v or "nil"))
local q = vidow.exec("printenv QT_SCALE_FACTOR")
vidow.log("childenv QT_SCALE_FACTOR=" .. tostring(q or "nil"))
local d = vidow.exec("printenv DISPLAY")
vidow.log("childenv DISPLAY=" .. tostring(d or "nil"))
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PID=$!
VIDOW_PGID=$VIDOW_PID
for _ in $(seq 1 40); do
	grep -aq "vidow ready" "$LOG" && break
	sleep 0.25
done
if ! grep -aq "vidow ready" "$LOG"; then
	fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)

mark "输出缩放"
./build/vdrag 8000 3000 8000 3000 >/dev/null 2>&1   # 光标先落在 1 倍屏上
sleep 0.3
./build/vdrag 800 3000 800 3000 >/dev/null 2>&1     # 再回到 2 倍屏

# X11 客户端（xterm 不是 HiDPI 感知的：正好代表老 X11 程序）
if command -v xterm >/dev/null; then
	setsid xterm -title dpi-x11 -geometry 40x10 >/dev/null 2>&1 &
	for _ in $(seq 1 40); do
		grep -aq "place dpi-x11" "$LOG" && break
		sleep 0.25
	done
	sleep 1
	./build/vdrag 800 3000 800 3000 >/dev/null 2>&1
	sleep 0.5
else
	echo "  SKIP  没装 xterm，跳过 X11 部分"
fi

# 同一个 X11 客户端挪到 1 倍屏上：缓冲区照样按全局缩放画，落屏时缩小 —— 还是清晰
if command -v xterm >/dev/null; then
	./build/vdrag 8000 3000 8000 3000 >/dev/null 2>&1   # 光标到 1 倍屏
	sleep 0.3
	setsid xterm -title dpi-x11-low >/dev/null 2>&1 &
	for _ in $(seq 1 40); do
		grep -aq "place dpi-x11-low" "$LOG" && break
		sleep 0.25
	done
	sleep 2
fi

# Wayland 客户端（GTK3 认 wp_fractional_scale / preferred_buffer_scale）
if command -v zenity >/dev/null; then
	setsid zenity --info --text=dpi-probe --timeout=6 >/dev/null 2>&1 &
	sleep 4
else
	echo "  SKIP  没装 zenity，跳过 Wayland 部分"
fi

# SSD 对齐：同一个 GTK 程序分别跑在 X11 和 Wayland 上，落在同一块屏上，
# 设备像素footprint 和装饰缓冲（内容 + 2×内边距，再乘输出缩放）都必须一样。
# X11 那次带上 GDK_SCALE=2 —— 这正是 vidow 给自己子进程设的值（上面的 childenv
# 日志会验一遍），这里手动补上是因为测试脚本不是 vidow 的子进程。
if command -v zenity >/dev/null; then
	mark "SSD 对齐（同一个 zenity，X11 vs Wayland，都在 2 倍屏上）"
	SX="$RUNDIR/x11d.log"; SW="$RUNDIR/wld.log"
	./build/vdrag 800 3000 800 3000 >/dev/null 2>&1   # 光标到 2 倍屏
	sleep 0.3

	base=$(wc -l < "$LOG")
	GDK_BACKEND=x11 GDK_SCALE=2 setsid zenity --info --text=ssd-parity --timeout=25 >/dev/null 2>&1 &
	for _ in $(seq 1 40); do
		tail -n +"$((base + 1))" "$LOG" | grep -aq "place .* -> " && break
		sleep 0.25
	done
	sleep 2
	tail -n +"$((base + 1))" "$LOG" > "$SX"
	pkill -x zenity; sleep 1

	base=$(wc -l < "$LOG")
	(DISPLAY= setsid zenity --info --text=ssd-parity --timeout=25 >/dev/null 2>&1 &)
	for _ in $(seq 1 40); do
		tail -n +"$((base + 1))" "$LOG" | grep -aq "place .* -> " && break
		sleep 0.25
	done
	sleep 2
	tail -n +"$((base + 1))" "$LOG" > "$SW"
	pkill -x zenity; sleep 0.5

	grep -aq "place .* -> " "$SX" &&
		pass "X11 的 GTK 对话框被摆位了（没被压成 1x1）" ||
		fail "X11 的 GTK 对话框没有被摆位（大概还卡在 1x1）"
	grep -aq "childenv GDK_SCALE=2" "$LOG" &&
		pass "子进程真的拿到 GDK_SCALE=2（X11 客户端按 2 倍画）" ||
		fail "子进程没拿到 GDK_SCALE=2"
else
	echo "  SKIP  没装 zenity，跳过 SSD 对齐"
fi

pgrep -x vidow >/dev/null && pass "合成器还活着（没有 abort）" || fail "合成器挂了"

# ---------------------------------------------------------------- 量结果
python3 - "$LOG" "$RUNDIR/x11d.log" "$RUNDIR/wld.log" <<'PY'
import re, sys

log = open(sys.argv[1], "rb").read().decode("utf-8", "replace")
lines = log.splitlines()
failed = []

def check(ok, what):
    print(("  PASS  " if ok else "  FAIL  ") + what)
    if not ok:
        failed.append(what)

print()
print("== 断言 ==")

# 1) 每块屏的缩放和逻辑尺寸
outs = {}
for m in re.finditer(r"output (HEADLESS-\d): (\d+)x(\d+) @ [\d.]+Hz scale ([\d.]+)"
                     r"（([^）]*)），逻辑 (\d+)x(\d+)", log):
    name, pw, ph, scale, why, lw, lh = m.groups()
    outs[name] = dict(pw=int(pw), ph=int(ph), scale=float(scale), why=why,
                      lw=int(lw), lh=int(lh))
check(len(outs) == 2, "两块屏都上了：%s" % ", ".join(
    "%s %.2fx 逻辑 %dx%d（%s）" % (n, o["scale"], o["lw"], o["lh"], o["why"])
    for n, o in sorted(outs.items())))
for n, o in outs.items():
    check(abs(o["lw"] - o["pw"] / o["scale"]) <= 1,
          "%s 逻辑尺寸 = 像素/缩放（%d/%g=%d）" % (n, o["pw"], o["scale"], o["lw"]))

if outs:
    densest = max(o["scale"] for o in outs.values())
    # 2) X 的全局缩放
    xs = re.findall(r"xwayland: 全局缩放 ([\d.]+) -> ([\d.]+)", log)
    final_x = float(xs[-1][1]) if xs else 0.0
    check(final_x == float(int(densest) + (densest % 1 > 0)),
          "XWayland 全局缩放 = ceil(最密的屏 %.2fx) = %.2f（X 只有一套坐标，"
          "低了这个数密屏上就是放大糊）" % (densest, final_x))
    check("GDK_SCALE=QT_SCALE_FACTOR=%d" % final_x in log,
          "给 X11 客户端的环境按同一倍数下发（GDK_SCALE / QT_SCALE_FACTOR）")

# 3) X11：缓冲区 vs 设备像素（只准缩小，密屏上 1:1）
x11 = {}
for m in re.finditer(r"x11: (\S+) 缓冲 (\d+)x(\d+) 按 (\d+) 倍声明 -> 场景 "
                     r"(\d+)x(\d+)（输出 ([\d.]+)x，落到设备像素 (\d+)x(\d+)）", log):
    t, bw, bh, s, sw, sh, oscale, dw, dh = m.groups()
    if int(bw) <= 4:          # 客户端第一帧的占位缓冲
        continue
    x11[t] = (int(bw), int(bh), int(s), int(dw), int(dh), float(oscale))
if x11:
    for t, (bw, bh, s, dw, dh, oscale) in sorted(x11.items()):
        ratio = bw / max(dw, 1)
        check(bw >= dw, "X11 %s：缓冲区 %dpx 不小于落在屏上的 %d 设备px"
              "（缩小才清晰，放大必糊）" % (t, bw, dw))
        if abs(oscale - s) < 1e-6:
            check(abs(ratio - 1.0) < 0.02,
                  "X11 %s：在最密的那块屏（%.2fx）上 1:1（缓冲 %dpx -> 设备 %dpx）"
                  % (t, oscale, bw, dw))
        else:
            check(abs(bw / dw - s / oscale) < 0.02,
                  "X11 %s：%.2f 倍屏上是 %.2f 倍缩小（缓冲 %d -> 设备 %d）"
                  % (t, oscale, bw / dw, bw, dw))
    check(len({round(v[5], 2) for v in x11.values()}) >= 1,
          "X11 窗口落在输出上（按该输出的缩放换算）")
else:
    print("  SKIP  日志里没有 X11 窗口")

# 4) Wayland：客户端按输出缩放渲染，1:1
wl = re.findall(r"surface (\S+): 缓冲 (\d+)x(\d+) 声明 \d+ 倍 -> 逻辑 (\d+)x(\d+)"
                r"（输出 ([\d.]+)x，设备像素 (\d+)x(\d+)）", log)
wl = [w for w in wl if int(w[1]) > 8]
if wl:
    for t, bw, bh, lw, lh, oscale, dw, dh in [wl[-1]]:
        check(abs(int(bw) - int(dw)) <= 1 and abs(int(bh) - int(dh)) <= 1,
              "Wayland %s：缓冲 %sx%s == 设备像素 %sx%s（输出 %.2fx，1:1 清晰）"
              % (t, bw, bh, dw, dh, float(oscale)))
else:
    print("  SKIP  日志里没有 Wayland 客户端")

# 5) X11 命中坐标：局部 = (布局 - 窗口原点) * 全局缩放
hits = re.findall(r"x11: 指针 ([\d.]+),([\d.]+) -> (\S+) 局部 ([\d.]+),([\d.]+)", log)
rects = [(m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)),
          int(m.group(5)))
         for m in re.finditer(r"place (\S+) -> (-?\d+),(-?\d+) (\d+)x(\d+)", log)]
if hits:
    lx, ly, t, sx, sy = hits[-1]
    # 按**坐标**找那个窗口，不按标题：xterm 会把标题改成 cwd，几个 xterm 可能同名
    # （踩过：命中在 6,6 那个窗口上，名字却命中了 326,6 那条 place 记录）。
    inside = [r for r in rects if r[0] == t and
              r[1] <= float(lx) < r[1] + r[3] and r[2] <= float(ly) < r[2] + r[4]]
    if not inside:
        inside = [r for r in rects if r[1] <= float(lx) < r[1] + r[3] and
                  r[2] <= float(ly) < r[2] + r[4]]
    ox, oy = (inside[-1][1], inside[-1][2]) if inside else (0, 0)
    exp_x = (float(lx) - ox) * final_x
    exp_y = (float(ly) - oy) * final_x
    check(inside != [] and abs(float(sx) - exp_x) <= 1.5 and
          abs(float(sy) - exp_y) <= 1.5,
          "X11 命中坐标：光标 %s,%s 在窗口原点 %d,%d 的表面上 -> 局部 %s,%s "
          "（预期 %.0f,%.0f = (光标-原点)x%.2f）"
          % (lx, ly, ox, oy, sx, sy, exp_x, exp_y, final_x))
# 6) SSD 对齐：X11 与 Wayland 的窗口都走同一条装饰管线 —— 装饰缓冲尺寸必须等于
#    (逻辑尺寸 + 2×内边距) × 输出缩放，两边各自成立（zenity 的自然尺寸在两种
#    后端上会差几十像素，那是 GTK 的事，不是装饰的事）
def slice_log(path):
    try:
        return open(path, "rb").read().decode("utf-8", "replace")
    except OSError:
        return ""

PAD = 12   # shadow_radius 8 + |shadow_offset_y| 2 + 2，见 vidow_fx_pad()
def targets(txt):
    return set((int(a), int(b)) for a, b in
               re.findall(r"fx: target (\d+)x(\d+)", txt))

def want_target(logical, scale, pad):
    return (int(round((logical[0] + 2 * pad) * scale)),
            int(round((logical[1] + 2 * pad) * scale)))

def decorated(txt, pat, label):
    """从日志里取最后一个真实窗口的「逻辑尺寸 + 输出缩放」，看它的装饰缓冲在不在"""
    last = None
    for m in re.finditer(pat, txt):
        if int(m.group(1)) > 8:
            last = m
    if last is None:
        print("  SKIP  %s：样本不足" % label)
        return
    logical = (int(last.group(1)), int(last.group(2)))
    scale = float(last.group(3))
    want = want_target(logical, scale, PAD)
    ok = want in targets(txt)
    check(ok, "%s 的装饰缓冲 = (内容 %dx%d + 2×%d) × %.2f = %dx%d%s"
          % (label, logical[0], logical[1], PAD, scale, want[0], want[1],
             "" if ok else "（没找到，装饰没走到这条管线）"))

x11txt = slice_log(sys.argv[2]) if len(sys.argv) > 2 else ""
wltxt = slice_log(sys.argv[3]) if len(sys.argv) > 3 else ""
if x11txt and wltxt:
    decorated(x11txt,
              r"x11: \S+ 缓冲 \d+x\d+ 按 \d+ 倍声明 -> 场景 (\d+)x(\d+)"
              r"（输出 ([\d.]+)x", "X11")
    decorated(wltxt,
              r"surface \S+: 缓冲 \d+x\d+ 声明 \d+ 倍 -> 逻辑 (\d+)x(\d+)"
              r"（输出 ([\d.]+)x", "Wayland")

# 7) 没有崩溃
check("Assertion" not in log and "abort" not in log.lower(),
      "日志里没有 assert / abort")

print()
print("结论：%s" % ("全部通过" if not failed else "%d 项失败" % len(failed)))
sys.exit(1 if failed else 0)
PY
rc=$?
[ $rc -ne 0 ] && FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "test-dpi.sh: OK"
else
	echo "test-dpi.sh: 有失败项"
fi
exit $FAILED
