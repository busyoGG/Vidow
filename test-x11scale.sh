#!/bin/bash
# X11 逐窗口缩放桥：`vidow.rule{ title = "…", x11_scale = 1 }`
#
# 靶子是微信登录窗那类**固定尺寸、不认 GDK_SCALE / QT_SCALE_FACTOR** 的老 X11 程序：
# 全局那套（xwayland_scale=auto=2）假设「X 客户端按 2 倍画」，把它的缓冲区除以 2，
# 于是窗口只剩该有尺寸的一半 —— 面积正好 1/4（用户看到的就是「只有面板的 1/4」）。
# 逐窗口写 x11_scale = 1 就是「这个客户端没按 2 倍画，别除」。
#
# 判据（同一个 300x300 的固定尺寸 X11 窗口，跑两遍：不带规则 / 带规则）：
#   1) 不带规则：缓冲 300x300 按 2 倍声明 → 场景/布局 150x150（面积是该有的 1/4）
#   2) 带规则：缓冲 300x300 按 1 倍声明 → 场景/布局 300x300（和 Wayland 窗口一样大）
#   3) 命中坐标各自和渲染一致（不带规则 ×2、带规则 ×1）—— 「看着小、点不着」不该有
#   4) vidowmsg rules 里查得回这条规则（值就是 1）
#
# 用法：./test-x11scale.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-x11scale-test
CFGDIR="$RUNDIR/home"
FAILED=0
VIDOW_PID=""
XP_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

stop_all() {
	[ -n "$XP_PID" ] && kill "$XP_PID" 2>/dev/null
	XP_PID=""
	if [ -n "$VIDOW_PID" ]; then
		kill -TERM -- "-$VIDOW_PID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PID" 2>/dev/null
		VIDOW_PID=""
	fi
}
cleanup() {
	stop_all
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

[ -x ./build/xpopup ] || { echo "test-x11scale.sh: 先构建（./build/xpopup 不在）"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

# 光标落点：故意选在两遍里都落在窗口里面的位置（窗口是居中的，位置会变）
CX=250
CY=180
# vdrag 的绝对坐标是「布局的 0..10000 分之一」，不是像素：换算一下
to_extent() { # <像素> <布局尺寸>
	python3 -c "import sys; print(int(round(float(sys.argv[1]) * 10000 / float(sys.argv[2]))))" "$1" "$2"
}

run_case() { # <标签> <额外配置>
	local name="$1" extra="$2"
	local log="$RUNDIR/log-$name"
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
-- 一块 2 倍屏：X 的全局缩放（auto）会取 ceil(2) = 2
vidow.output{ name = "HEADLESS-1", scale = 2 }
scale = "auto"
xwayland = true
$extra
CFG
	setsid ./build/vidow > "$log" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$log" && break; sleep 0.25; done
	if ! grep -aq "vidow ready" "$log"; then
		fail "$name：vidow 没起来"
		grep -a "ERROR" "$log" | head -5
		stop_all
		return 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$log" |
		tail -1 | cut -d= -f2)
	export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$log" | tail -1 | cut -d= -f2)

	# 固定尺寸（WM_NORMAL_HINTS min == max）的普通 X11 窗口 = 微信登录窗那种。
	# 客户端偶尔会卡在「窗口建了但没 map」上（Xwayland 懒启动那一下的时序），
	# 起两次就好 —— 这个测试验的是缩放桥，不是客户端自己靠不靠谱。
	for attempt in 1 2; do
		setsid nohup ./build/xpopup popup normal 100 100 300 300 fixed \
			> "$RUNDIR/xp-$name.log" 2>&1 &
		XP_PID=$!
		for _ in $(seq 1 30); do
			grep -aq "place xp-popup -> " "$log" && break
			sleep 0.2
		done
		grep -aq "place xp-popup -> " "$log" && break
		if [ "$attempt" = 1 ]; then
			echo "  （第 1 次客户端没 map 上，重起一次）"
			kill "$XP_PID" 2>/dev/null
			XP_PID=""
			sleep 0.5
		fi
	done
	sleep 0.8

	# 指针移进窗口（vdrag 的两次坐标一样 = 落在那儿）
	local lw lh
	read -r lw lh < <(./build/vidowmsg outputs | python3 -c '
import json, sys
outs = json.load(sys.stdin)
if isinstance(outs, dict):
    outs = outs.get("outputs", [])
print(max(o["layout"]["x"] + o["layout"]["width"] for o in outs),
      max(o["layout"]["y"] + o["layout"]["height"] for o in outs))
')
	./build/vdrag "$(to_extent $CX $lw)" "$(to_extent $CY $lh)" \
		"$(to_extent $CX $lw)" "$(to_extent $CY $lh)" >/dev/null 2>&1
	sleep 0.6
	./build/vidowmsg rules > "$RUNDIR/rules-$name.json" 2>/dev/null
	./build/vidowmsg windows > "$RUNDIR/windows-$name.json" 2>/dev/null
	stop_all
}

mark "1) 不带规则（现在的默认行为：X 按 2 倍画 → 缓冲区除以 2）"
run_case plain ""
grep -a "x11: xp-popup 缓冲" "$RUNDIR/log-plain" | tail -1
grep -a "place xp-popup -> " "$RUNDIR/log-plain" | tail -1

mark "2) 带规则 vidow.rule{ title = \"xp-popup\", x11_scale = 1 }"
run_case rule 'vidow.rule{ title = "xp-popup", x11_scale = 1 }'
grep -a "rule: xp-popup 的 X11 缩放桥" "$RUNDIR/log-rule" | tail -1
grep -a "x11: xp-popup 缓冲" "$RUNDIR/log-rule" | tail -1
grep -a "place xp-popup -> " "$RUNDIR/log-rule" | tail -1

mark "3) 断言"
python3 - "$RUNDIR" "$CX" "$CY" <<'PY'
import json, os, re, sys

rundir, cx, cy = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
failed = []

def check(cond, msg):
    print(("  PASS  " if cond else "  FAIL  ") + msg)
    if not cond:
        failed.append(msg)

def log(name):
    return open(os.path.join(rundir, "log-%s" % name), "rb").read().decode(
        "utf-8", "replace")

def place(txt, title):
    m = None
    for m in re.finditer(r"place %s -> (-?\d+),(-?\d+) (\d+)x(\d+)"
                         % re.escape(title), txt):
        pass
    return tuple(int(g) for g in m.groups()) if m else None

def buf(txt):
    """缓冲尺寸、vidow 这边的缩放桥、场景尺寸、以及 Xwayland 在表面上声明的
    buffer scale（写了 _XWAYLAND_GLOBAL_OUTPUT_SCALE 之后它是 2）。"""
    m = None
    for m in re.finditer(r"x11: xp-popup 缓冲 (\d+)x(\d+) 按 (\d+) 倍声明 -> "
                         r"场景 (\d+)x(\d+)（表面 (\d+) 倍", txt):
        pass
    return tuple(int(g) for g in m.groups()) if m else None

def hit(txt):
    m = None
    for m in re.finditer(r"x11: 指针 ([\d.]+),([\d.]+) -> xp-popup 局部 "
                         r"([\d.]+),([\d.]+)", txt):
        pass
    return tuple(float(g) for g in m.groups()) if m else None

plain, rule = log("plain"), log("rule")

# 1) 不带规则：150x150（面积 1/4）
pb = buf(plain)
check(pb is not None, "不带规则：日志里有「缓冲 → 场景」那行")
if pb:
    bw, bh, scale, dw, dh, _sface = pb
    check(scale == 2, "不带规则：缓冲区按全局缩放 2 倍声明（%d）" % scale)
    check((dw, dh) == (bw // 2, bh // 2),
          "不带规则：缓冲 %dx%d 被除 2 → 场景 %dx%d" % (bw, bh, dw, dh))
    check((dw, dh) == (150, 150),
          "不带规则：300x300 的窗口只剩 150x150 —— 面积是该有的 1/4（复现用户看到的）"
          % ())
p = place(plain, "xp-popup")
check(p is not None and (p[2], p[3]) == (150, 150),
      "不带规则：布局里它就是 150x150（%s）" % (p,))

# 2) 带规则：300x300
rb = buf(rule)
check(rb is not None, "带规则：日志里有「缓冲 → 场景」那行")
if rb:
    bw, bh, scale, dw, dh, _sface = rb
    check(scale == 1, "带规则：缓冲区按 1 倍声明（不除，%d）" % scale)
    check((dw, dh) == (bw, bh),
          "带规则：缓冲 %dx%d 原样 → 场景 %dx%d" % (bw, bh, dw, dh))
    check((dw, dh) == (300, 300),
          "带规则：300x300 的窗口就是 300x300（和同尺寸的 Wayland 窗口一样大）")
p2 = place(rule, "xp-popup")
check(p2 is not None and (p2[2], p2[3]) == (300, 300),
      "带规则：布局里它是 300x300（%s）" % (p2,))
check("rule: xp-popup 的 X11 缩放桥" in rule,
      "规则真的作用到这个窗口上了（日志有「X11 缩放桥」那行）")

# 3) 命中坐标：局部坐标在「表面自己的逻辑坐标系」里，客户端再乘它声明的
#    buffer scale 就是 X 坐标。所以恒等式是
#        局部 × 表面缩放 == (布局 - 原点) × 缩放桥
#   两边都从日志 / windows dump 里量，不写死数字。
for name, txt, p in (("不带规则", plain, p), ("带规则", rule, p2)):
    h = hit(txt)
    b = buf(txt)
    if h is None or p is None or b is None:
        check(False, "%s：没拿到命中坐标 / 缓冲行（%s）" % (name, h))
        continue
    bridge = float(b[2])
    sscale = float(b[5])
    lx, ly, sx, sy = h
    ex, ey = (lx - p[0]) * bridge / sscale, (ly - p[1]) * bridge / sscale
    check(abs(sx - ex) <= 1.5 and abs(sy - ey) <= 1.5,
          "%s：光标 %g,%g 在窗口原点 %d,%d 上 → 局部 %g,%g（预期 %g,%g = "
          "×%g/表面%d）" % (name, lx, ly, p[0], p[1], sx, sy, ex, ey, bridge,
                            int(sscale)))
    # 客户端那侧：局部 × 表面缩放 应该等于「布局偏移 × 桥」——点哪儿就是哪儿
    cx, cy = sx * sscale, sy * sscale
    wx, wy = (lx - p[0]) * bridge, (ly - p[1]) * bridge
    check(abs(cx - wx) <= 1.5 and abs(cy - wy) <= 1.5,
          "%s：X 客户端拿到的坐标 %g,%g = 布局偏移 %d,%d × %g（一致）"
          % (name, cx, cy, int(lx - p[0]), int(ly - p[1]), bridge))

# 4) IPC 里查得回规则
try:
    rules = json.load(open(os.path.join(rundir, "rules-rule.json")))
    if isinstance(rules, dict):
        rules = rules.get("rules", [])
    got = [r.get("x11_scale") for r in rules]
    check(1.0 in [float(g) for g in got if g is not None],
          "vidowmsg rules 里查得回 x11_scale = 1（%s）" % got)
except (OSError, ValueError) as e:
    check(False, "读 rules json 失败：%s" % e)

# 5) 没有崩溃 / 断言
for name, txt in (("不带规则", plain), ("带规则", rule)):
    check("Assertion" not in txt and "abort" not in txt.lower(),
          "%s：日志里没有 assert / abort" % name)

print()
if failed:
    print("test-x11scale.sh: %d 项没通过" % len(failed))
    sys.exit(1)
print("全部通过 / test-x11scale.sh: OK")
PY
[ $? -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "test-x11scale.sh: OK"
else
	echo "test-x11scale.sh: 有失败项"
fi
exit $FAILED
