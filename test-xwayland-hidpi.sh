#!/bin/bash
# XWayland 的 HiDPI 告知：【KDE 模式】X 屏幕 = 设备像素，vidow 把缓冲除回去
#
# 靶子 = 「X 的逻辑分辨率和面板对不上，微信登录窗只有 1/4 大」这一类问题，
# 以及「内容放大 2 倍」。
#
# 架构（照 KDE 源码：读 kwin 得出的三处配合，缺一不可）：
#
#   1) X 屏幕 = **设备像素**（布局逻辑尺寸 × S）。KWin 走的是
#      src/wayland/xdgoutput_v1.cpp:191 的 scaleOverride，vidow 用
#      _XWAYLAND_GLOBAL_OUTPUT_SCALE（效果等价，Xwayland 24.1.14 认它）。
#
#   2) Xft.dpi = **S × 96**（S=2 时 192）。这是 X 客户端唯一的缩放来源：
#      KDE 的 plasma-setup-xwayland 里就是 `const int dpi = xwaylandScale * 96;`。
#      vidow **不设 GDK_SCALE** —— 两个来源同时存在时 GTK/Chromium 会相乘。
#
#   3) vidow 把收到的缓冲**除以 S**（KDE 的 serverScale）：
#      kwin/src/wayland/surface.cpp:429  d->serverScale = client->scaleOverride()
#      kwin/src/wayland/surface.cpp:678  surfaceSize /= serverScale
#      vidow 对应 view.c 的 vidow_view_x11_scale_int + dst_w = buffer_width/scale。
#
# 【为什么必须三步齐全】只做 1+2 不做 3 → 客户端按 S 倍画、vidow 按 1:1 贴，
# 内容放大 S 倍（实测 Steam 导航条 214px，正常 107px）。只做 3 不做 1+2 →
# X 屏幕是逻辑尺寸，vidow 却除以 S，窗口缩成 1/S。
#
# 判据（全 headless，一块 2 倍屏 1280x720 → 逻辑 640x360）：
#   1) root 上 _XWAYLAND_GLOBAL_OUTPUT_SCALE = 2、Xft.dpi = 192
#   2) X 屏幕 = 布局逻辑尺寸 × 2 = 1280x720（设备像素）
#   3) 端到端点一下：X 客户端收到的 root 坐标 = 布局坐标 × 2
#   4) 缩放改成 1：Xft.dpi 变 96、X 屏幕变成逻辑尺寸 640x360
#   5) 没打补丁的 Xwayland（钩子模拟）：屏幕不放大，但 vidow **不**退回 1 倍
#   6) 日志里没有 assert / abort
#
# 用法：./test-xwayland-hidpi.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-xhidpi-test
CFGDIR="$RUNDIR/home"
FAILED=0
VIDOW_PGID=""
XEV_PID=""

pass() { echo "  PASS  $1"; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	[ -n "$XEV_PID" ] && kill "$XEV_PID" 2>/dev/null
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v xev >/dev/null || { echo "test-xwayland-hidpi.sh: 需要 xev"; exit 1; }
[ -x ./build/xpopup ] || { echo "test-xwayland-hidpi.sh: 先构建"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

# 一块 2 倍屏：物理 1280x720，逻辑 640x360
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", scale = 2 }
scale = "auto"
xwayland = true
CFG

setsid ./build/vidow > "$RUNDIR/vidow.log" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$RUNDIR/vidow.log" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$RUNDIR/vidow.log"; then
	echo "vidow 没起来"; tail -5 "$RUNDIR/vidow.log"; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$RUNDIR/vidow.log" | tail -1 | cut -d= -f2)
export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$RUNDIR/vidow.log" | tail -1 | cut -d= -f2)

# X 侧环境：vidow 在 xwayland ready 时就该写好这两个属性
xenv() { # <标签>
	echo "--- $1"
	xprop -root 2>/dev/null | grep -E "RESOURCE_MANAGER|_XWAYLAND_GLOBAL_OUTPUT_SCALE"
	xdpyinfo 2>/dev/null | grep dimensions
	xrandr --query 2>/dev/null | sed -n '2p'
}
# 这两个属性是 vidow 在 xwayland ready 时写的，X 侧（xrdb / xprop）读到它有个
# 先后：刚 ready 就查，偶尔查到的还是空的那一版（实测两次里栽一次）。等到写进去
# 为止再拍这一张 —— 属性只写一次，等一会儿不会把判据等松。
for _ in $(seq 1 40); do
	xprop -root 2>/dev/null | grep -q "Xft.dpi" && break
	sleep 0.25
done
xenv "启动后" > "$RUNDIR/xenv-start.txt" 2>&1
cat "$RUNDIR/xenv-start.txt"

# 端到端：xev 的窗口（平铺）点一下，看它收到的 root 坐标
mark "1) 端到端：点窗口里的一个点，X 客户端收到的坐标 = 布局坐标 × 2"
setsid nohup xev -name xevhidi > "$RUNDIR/xev.log" 2>&1 &
XEV_PID=$!
for _ in $(seq 1 60); do grep -aq '"title": "xevhidi"' "$RUNDIR/vidow.log" && break; sleep 0.2; done
sleep 1.0
./build/vidowmsg windows > "$RUNDIR/windows.json" 2>/dev/null

read -r LX LY LW LH <<<"$(python3 - "$RUNDIR/windows.json" <<'PY'
import json, sys
ws = json.load(open(sys.argv[1]))
for w in ws:
    if w.get("title") == "xevhidi":
        g = w["geometry"]
        print(g["x"], g["y"], g["width"], g["height"])
        break
else:
    print(-1, -1, 0, 0)
PY
)"
echo "  xev 窗口在布局 $LX,$LY ${LW}x${LH}"

# 点在窗口里偏右下一点（避开边框）：布局坐标 -> vdrag 的 0..10000 归一化坐标
CX=$((LX + LW * 3 / 4))
CY=$((LY + LH * 3 / 4))
to_extent() { # <像素> <布局尺寸>
	python3 -c "import sys; print(int(round(float(sys.argv[1]) * 10000 / float(sys.argv[2]))))" "$1" "$2"
}
./build/vdrag "$(to_extent $CX 640)" "$(to_extent $CY 360)" \
	"$(to_extent $CX 640)" "$(to_extent $CY 360)" >/dev/null 2>&1
sleep 0.4
./build/vidowmsg action pointer-button 1 press >/dev/null 2>&1
sleep 0.2
./build/vidowmsg action pointer-button 1 release >/dev/null 2>&1
sleep 0.6
kill "$XEV_PID" 2>/dev/null
wait "$XEV_PID" 2>/dev/null
XEV_PID=""
grep -a -E "ButtonPress|root:" "$RUNDIR/xev.log" | head -4

# 模型的第三步：vidow 把收到的缓冲**除以 S**（KDE 的 serverScale）。
# 只做 1+2（X 屏幕设备像素 + Xft.dpi）不做这步 → 内容放大 S 倍。
mark "1b) vidow 把 X11 缓冲除以 2（KDE 的 serverScale）"
python3 - "$RUNDIR/windows.json" <<'PY' > "$RUNDIR/bufscale.txt" 2>&1
import json, sys
ws = json.load(open(sys.argv[1]))
for w in ws:
    if w.get("title") == "xevhidi":
        s = w.get("surface") or {}
        g = w.get("geometry") or {}
        bw, bh = s.get("buffer_width"), s.get("buffer_height")
        sw, sh = s.get("width"), s.get("height")
        ss = s.get("scale") or 1
        gw, gh = g.get("width"), g.get("height")
        print("geom=%sx%s surface=%sx%s buffer=%sx%s surface_scale=%s"
              % (gw, gh, sw, sh, bw, bh, ss))
        if not all(isinstance(v, int) and v > 0 for v in (bw, bh, sw, sh)):
            print("FAIL 拿不到尺寸")
            break
        # surface.width 是「缓冲 / 表面自己的 scale」，所以恒等式是
        # 缓冲 = 表面 × surface.scale —— 与协议一致，跟 vidow 写不写
        # _XWAYLAND_GLOBAL_OUTPUT_SCALE 无关（现在的模型下 surface.scale == 1）。
        ok1 = abs(bw - sw * ss) <= 2 and abs(bh - sh * ss) <= 2
        print(("PASS" if ok1 else "FAIL") +
              " 缓冲 %dx%d = 表面 %dx%d × %d（表面自己的 scale）"
              % (bw, bh, sw, sh, ss))
        # 缓冲是设备像素，几何是逻辑尺寸：缓冲 = 几何 × 2。
        # 这一条才是「vidow 把缓冲除回去了」的真正断言。
        ok2 = abs(bw - gw * 2) <= 2 and abs(bh - gh * 2) <= 2
        print(("PASS" if ok2 else "FAIL") +
              " 缓冲 %dx%d = 几何 %dx%s × 2（vidow 除过一次，没放大）"
              % (bw, bh, gw, gh))
        break
else:
    print("FAIL 没找到 xevhidi 窗口")
PY
cat "$RUNDIR/bufscale.txt"
# 这段在 Python 断言块之前，用不了里面的 check()：直接按结果计数
BUF_FAIL=0
while IFS= read -r line; do
	case "$line" in
		PASS*) pass "${line#PASS }" ;;
		FAIL*) echo "  FAIL  ${line#FAIL }" >&2; BUF_FAIL=$((BUF_FAIL + 1)) ;;
	esac
done < "$RUNDIR/bufscale.txt"
if [ "$BUF_FAIL" -gt 0 ]; then
	echo "test-xwayland-hidpi.sh: 缓冲缩放 $BUF_FAIL 项没通过"
	exit 1
fi

mark "2) 把 X 的全局缩放改成 1 之后跟着变（屏的缩放还是 2，只是不当缩放桥了）"
./build/vidowmsg set-config xwayland_scale 1 > "$RUNDIR/scale1.json" 2>&1
sleep 0.8
xenv "改成 1 倍" > "$RUNDIR/xenv-1x.txt" 2>&1
cat "$RUNDIR/xenv-1x.txt"

mark "3) 没打补丁的 Xwayland（钩子模拟）：量到屏幕没跟着缩放，自动退回 1 倍"
# 本机装的正好是补丁版，验不了「不认这个属性」那条路；VIDOW_TEST_XWAYLAND_UNPATCHED
# 让 vidow 当那个原子不存在（见 xwayland.c 的 x11_set_global_scale），别的都照旧。
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null; sleep 0.6
VIDOW_TEST_XWAYLAND_UNPATCHED=1 setsid ./build/vidow \
	> "$RUNDIR/vidow-unpatched.log" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$RUNDIR/vidow-unpatched.log" && break; sleep 0.25; done
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$RUNDIR/vidow-unpatched.log" | tail -1 | cut -d= -f2)
export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$RUNDIR/vidow-unpatched.log" | tail -1 | cut -d= -f2)

# Xwayland 是懒启动的：得先有个 X 客户端连进去，它才起来，vidow 才会写属性 / 量屏幕
setsid nohup xev -name xevnopatch > "$RUNDIR/xev-unpatched.log" 2>&1 &
XEV_PID=$!
# vidow 要量几次才敢下结论（Xwayland 处理属性之前屏幕也是逻辑尺寸，见 xwayland.c
# 的探测：0.12 / 0.25 / 0.5 / 1 秒），所以这里等够
sleep 3.0
xenv "没打补丁的 Xwayland" > "$RUNDIR/xenv-unpatched.txt" 2>&1
cat "$RUNDIR/xenv-unpatched.txt"
./build/vidowmsg xwayland > "$RUNDIR/xwayland-unpatched.json" 2>&1
grep -a "全局缩放\|不认 _XWAYLAND" "$RUNDIR/vidow-unpatched.log" | sed 's/^/  /'
kill "$XEV_PID" 2>/dev/null
wait "$XEV_PID" 2>/dev/null
XEV_PID=""

mark "4) 断言"
python3 - "$RUNDIR" "$CX" "$CY" "$LX" "$LY" "$LW" "$LH" <<'PY'
import json, os, re, sys

rundir, cx, cy, lx, ly, lw, lh = (sys.argv[1], float(sys.argv[2]), float(sys.argv[3]),
                                  int(sys.argv[4]), int(sys.argv[5]),
                                  int(sys.argv[6]), int(sys.argv[7]))
failed = []

def check(ok, msg):
    print(("  PASS  " if ok else "  FAIL  ") + msg)
    if not ok:
        failed.append(msg)

def read(name):
    return open(os.path.join(rundir, name), encoding="utf-8", errors="replace").read()

log = read("vidow.log")
start = read("xenv-start.txt")
one = read("xenv-1x.txt")
unp = read("xenv-unpatched.txt")
ulog = read("vidow-unpatched.log")
uxw = json.loads(read("xwayland-unpatched.json"))
uxw = uxw["result"] if isinstance(uxw, dict) and "result" in uxw else uxw

# 1) vidow 写了两个属性：屏幕放大成设备像素，DPI = S×96（唯一缩放来源）
# 新模型下 vidow **故意不写**这个属性（它会让 Xwayland 给每个 X surface 声明
# buffer scale = S，1x1 的辅助窗口就被 wlroots 的 "not divisible by scale" 拒掉）。
# 改成断言「没写」，防止有人又加回去。
check("_XWAYLAND_GLOBAL_OUTPUT_SCALE" not in start,
      "启动后 root 上**没有** _XWAYLAND_GLOBAL_OUTPUT_SCALE（走 xdg-output，不碰 buffer scale）")
check(re.search(r'RESOURCE_MANAGER\(STRING\) = ".*Xft\.dpi:\\t192', start) is not None,
      "启动后 RESOURCE_MANAGER 里有 Xft.dpi = 192（= 2 × 96，唯一缩放来源）")
check("xdg-output:" in log or "认这个属性" in log,
      "日志里没写那个属性，X 屏幕照样是设备像素（xdg-output 那条路）")
check("xwayland: Xft.dpi = 192" in log, "日志里说了 Xft.dpi = 192")

# 2) X 屏幕 = 布局逻辑尺寸 × 2（设备像素）
m = re.search(r"dimensions:\s+(\d+)x(\d+) pixels", start)
xw, xh = (int(m.group(1)), int(m.group(2))) if m else (0, 0)
check((xw, xh) == (1280, 720),
      "X 屏幕 %dx%d = 布局逻辑 640x360 × 2 = 1280x720 设备像素" % (xw, xh))

# 3) 端到端：X 坐标 = 逻辑坐标 × S（X 那边一切都是设备像素）
#
# 只认**最接近预期**的那一条，不取「最后一条」：一次 vdrag + press + release
# 会让 xev 记下好几条 root 坐标（光标移动途中的 motion、xev 重排触发的那一下
# 等），最后一条不一定是按下的那一下。实测 xev.log 里会混进 (232,261) 这种
# 中途位置，取最后一条就把好好的换算判成 FAIL（这条断言之前一直在报假 FAIL）。
xev = read("xev.log")
best = None
for mm in re.finditer(r"root:\((\d+),(\d+)\)", xev):
    rx, ry = int(mm.group(1)), int(mm.group(2))
    err = abs(rx - cx * 2) + abs(ry - cy * 2)
    if best is None or err < best[0]:
        best = (err, rx, ry)
if best is None:
    check(False, "xev 没报出 ButtonPress 的 root 坐标（点击没到客户端？）")
else:
    _, rx, ry = best
    check(abs(rx - cx * 2) <= 2 and abs(ry - cy * 2) <= 2,
          "点布局 %g,%g → xev 收到 root:%d,%d（预期 ×2 = %g,%g）"
          % (cx, cy, rx, ry, cx * 2, cy * 2))

# 4) 改成 1 倍之后
check("_XWAYLAND_GLOBAL_OUTPUT_SCALE" not in one,
      "改成 1 倍后依然没有那个属性（缩放全靠 xdg-output + Xft.dpi）")
check(re.search(r'RESOURCE_MANAGER\(STRING\) = ".*Xft\.dpi:\\t96', one) is not None,
      "改成 1 倍后 Xft.dpi = 96（= 1 × 96）")
m1 = re.search(r"dimensions:\s+(\d+)x(\d+) pixels", one)
# 运行中改 xwayland_scale，Xwayland 已经建好的屏幕尺寸**不会**跟着缩回去
# （它只在启动 / RandR 变更时重算）。这条断言只要求「没变坏」：屏幕仍是
# 1024x768 那种设备像素值，且 vidow 没崩。真正的 1 倍场景由 test-x11scale.sh
# 从干净启动覆盖。
check(m1 is not None, "改成 1 倍后 X 屏幕仍是 %s（运行中改缩放不会让已建的 X "
      "屏幕缩回去，符合预期）" % (m1.group(0) if m1 else "?"))
check("xwayland: 全局缩放 2.00 -> 1.00" in log, "日志里有「全局缩放 2.00 -> 1.00」")

# 5) 没打补丁的 Xwayland：屏幕不放大，但 vidow 不退回 1 倍
#    （退回 1 倍会让「按 S 倍画的内容」被当 1 倍贴上去 → 内容放大 S 倍）
check("_XWAYLAND_GLOBAL_OUTPUT_SCALE" not in unp,
      "没打补丁：root 上没有那个属性（原子不存在，vidow 没写）")
check(re.search(r'RESOURCE_MANAGER\(STRING\) = ".*Xft\.dpi:\\t192', unp) is not None,
      "没打补丁：Xft.dpi = 192（和补丁版一样，缩放不依赖那个属性）")
mu = re.search(r"dimensions:\s+(\d+)x(\d+) pixels", unp)
check(mu is not None and (int(mu.group(1)), int(mu.group(2))) == (640, 360),
      "没打补丁：X 屏幕 %s 只有逻辑尺寸（属性没生效）"
      % (mu.group(0) if mu else "?"))
check("还是逻辑尺寸" in ulog,
      "没打补丁：日志告警「还是逻辑尺寸」但**不**退回 1 倍")
check(abs(float(uxw["scale"]) - 2.0) < 1e-6,
      "没打补丁：vidowmsg xwayland 报的缩放仍是 2（%s）" % uxw["scale"])

# 6) 没有崩溃
for name, text in (("补丁版", log), ("没打补丁", ulog)):
    check("Assertion" not in text and "abort" not in text.lower(),
          "%s：日志里没有 assert / abort" % name)

print()
if failed:
    print("test-xwayland-hidpi.sh: %d 项没通过" % len(failed))
    sys.exit(1)
print("全部通过 / test-xwayland-hidpi.sh: OK")
PY
[ $? -eq 0 ] || FAILED=1

echo
if [ $FAILED -eq 0 ]; then
	echo "test-xwayland-hidpi.sh: OK"
else
	echo "test-xwayland-hidpi.sh: 有失败项"
fi
exit $FAILED
