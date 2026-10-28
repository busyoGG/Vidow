#!/bin/bash
# 「窗口只有一点在视口里」的时候，屏幕上那一条必须是**裁**出来的，不是拉出来的。
#
# 用户报的：「如果一个窗口在视口外（但有一点在视口内）则会窗口表面横向拉伸变扁扁的，
# 且会随着窗口在视口内的范围大小出现不同程度的黑边」。
#
# 根因：越界裁剪那一刀（view.c 的 vidow_view_apply_clip）走的是 wlroots 的
# wlr_scene_subsurface_tree_set_clip()，它是靠改场景节点的 src_box（+ 显示尺寸）
# 实现的；而装饰管线（fx）采源缓冲的哪一块，正是从 src_box 算出来的
# （view.c 的 fx_geo_tex_map 把它当成「客户端声明的有效区」）。于是裁完之后，装饰
# 把「裁剩下的那一小条」当成整块有效内容，把那一小条**拉伸铺满整块窗口** ——
# 屏幕上就是：可见的那一条里内容被横向拉扁，剩下的是那圈没画过的留白（黑）。
# 可见部分越窄，采样到的条越窄、黑边越大。
#
# 修法：装饰管线在画这个窗口的时候（view->fx.active），表面那一份是隐藏的
# （0 透明度），**不能**再调那个 set_clip —— 装饰那份离屏缓冲由 fx.c 自己按同一个
# 框裁（crop_src）。装饰关掉的时候（表面自己画）才走 set_clip 那条路。
#
# 判据（headless 一块 1280x720 的屏，用 tools/pattern.py 的四象限窗口当标尺）：
#   1) 复现条件在：有窗口只有一部分在屏里（3 列，中间那列拉宽到 0.9 → 必然有列越界）
#   2) 可见的那一条里**一个黑像素都没有**（修之前：一条 50+ 像素宽的黑带）
#   3) 四象限的分界落在它该在的位置（窗口中心 ±4px）—— 裁出来才对，缩放会挪位置
#   4) 上下两半的颜色是「可见的那一半窗口」该有的那两个（左半 = 红/蓝，右半 = 绿/白）
#   5) 装饰关掉（表面自己画，走 set_clip 那条路）时也一样：黑边没有、分界对
#   6) vidow 全程活着
#
# 用法：./test-clip-stretch.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-clipstretch-test
FAILED=0
VIDOW_PID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PID" ] && kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	# 别按进程名全杀：用户的桌面会话也叫 vidow，跑一次测试就把桌面端掉
	# （2026-10-28 在 test-screencast.sh 上实测踩到）。上面按 $VIDOW_PID
	# 杀自己那个就够了。
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v python3 >/dev/null || { echo "test-clip-stretch.sh: 需要 python3"; exit 1; }
python3 -c "import gi; gi.require_version('Gtk','3.0')" 2>/dev/null ||
	{ echo "test-clip-stretch.sh: 需要 python3-gobject (GTK3)"; exit 1; }
command -v grim >/dev/null || { echo "test-clip-stretch.sh: 需要 grim"; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "test-clip-stretch.sh: 需要 python3-PIL"; exit 1; }

SCREEN_W=1280
SCREEN_H=720

# 一轮：起一块屏的 vidow（decor 由参数定）→ 三个四象限窗口 → 让中间那列越界 →
# 量屏幕上那一条。
run_case() {
	local name="$1" decor="$2"
	local dir="$RUNDIR/$name"
	mark "$name（decor_enabled = $decor）"
	VIDOW_PID=""
	CLIENT_PIDS=""
	rm -rf "$dir"; mkdir -p "$dir/home/.config/vidow" "$dir/run"
	chmod 700 "$dir/run"
	cat > "$dir/home/.config/vidow/config.lua" <<CFG
vidow.output{ name = "HEADLESS-1", scale = 1, mode = "1280x720" }
gaps = 0
decor_enabled = $decor
CFG

	export XDG_RUNTIME_DIR="$dir/run"
	export HOME="$dir/home" VIDOW_LOG=info
	export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
	export DISPLAY= WAYLAND_DISPLAY=
	export VIDOW_CONFIG_LUA="$dir/home/.config/vidow/config.lua"

	setsid ./build/vidow > "$dir/log" 2>&1 &
	VIDOW_PID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$dir/log" && break; sleep 0.25; done
	if ! grep -aq "vidow ready" "$dir/log"; then
		fail "vidow 没起来（$(tail -3 "$dir/log" | tr '\n' '|')）"
		return
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$dir/log" |
		tail -1 | cut -d= -f2)

	for i in 1 2 3; do
		setsid nohup python3 tools/pattern.py "p$i" > "$dir/p$i.log" 2>&1 </dev/null &
		CLIENT_PIDS="$CLIENT_PIDS $!"
	done
	local n=0
	for _ in $(seq 1 40); do
		n=$(./build/vidowmsg windows --timeout 5 2>/dev/null |
			grep -c '"frame_width"')
		[ "$n" -ge 3 ] && break
		sleep 0.5
	done
	if [ "$n" -lt 3 ]; then
		fail "三个窗口没都上来（只看到 $n 个）"
		return
	fi
	# 把两列拉宽：0.9 / 0.7 / 0.5 = 1152 / 896 / 640，总共 2688 > 1280。
	# 为什么是这个配比：视口被布局末尾夹住时，偏移量 = 2688 - 1280 = 1408，
	# 而 1152 < 1408 < 1536（最小的两列之和）—— **不管这三列怎么排**，1408
	# 都落在第二列里面，也就是第二列必然「左边被切掉、右边还在屏里」。
	#
	# 要的就是这一种越界：只有「裁的框比表面窄」才会真的把表面树裁掉（这条
	# bug 的触发条件）。列的顺序不保证（跟窗口创建顺序有关），所以焦点逐列试
	# 一遍，直到看见它。
	./build/vidowmsg set-column-width 0.9 --title p2 --timeout 5 >/dev/null 2>&1
	./build/vidowmsg set-column-width 0.7 --title p3 --timeout 5 >/dev/null 2>&1
	local part=""
	for t in p1 p2 p3; do
		./build/vidowmsg focus-window --title "$t" --timeout 5 >/dev/null 2>&1
		sleep 1.0
		./build/vidowmsg windows --timeout 5 > "$dir/win.json" 2>/dev/null
		part=$(python3 - "$dir/win.json" <<'PYPART'
import json, sys
for w in json.load(open(sys.argv[1])):
    g = w.get("geometry") or {}
    if not g.get("width"):
        continue
    x0, x1 = g["x"], g["x"] + g["width"]
    # 左边被切掉、右边还在屏里，而且露出来的那一条够宽（能量）
    if x0 < 0 < x1 and x1 >= 40:
        print("%s(id %s)" % (w.get("title"), w.get("id")))
        break
PYPART
)
		[ -n "$part" ] && break
	done
	if [ -z "$part" ]; then
		echo "      布局：$(python3 -c "
import json
for w in json.load(open('$dir/win.json')):
    g = w.get('geometry') or {}
    print('%s@%s+%s' % (w.get('title'), g.get('x'), g.get('width')), end=' ')
")"
	fi
	grim -o HEADLESS-1 "$dir/shot.png" >/dev/null 2>&1
	if [ ! -s "$dir/shot.png" ]; then
		fail "grim 没截到图"
		return
	fi

	local res
	res=$(python3 - "$dir/win.json" "$dir/shot.png" "$SCREEN_W" "$SCREEN_H" <<'PY'
import json, sys, warnings
warnings.filterwarnings("ignore")
import numpy as np
from PIL import Image

win_json, shot, SW, SH = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
ws = json.load(open(win_json))
im = np.asarray(Image.open(shot).convert("RGB")).astype(int)

def cls(px):
    r, g, b = px
    if r > 150 and g < 100 and b < 100: return "R"
    if g > 150 and r < 100 and b < 100: return "G"
    if b > 150 and r < 100 and g < 100: return "B"
    if r > 150 and g > 150 and b > 150: return "W"
    if r < 40 and g < 40 and b < 40: return "."
    return "?"

# 左边被切掉的那一列（窗口横跨屏的左边界）：只有这一种会把表面树真的裁窄
part = None
for w in ws:
    g = w.get("geometry") or {}
    if not g.get("width"):
        continue
    x0, x1 = g["x"], g["x"] + g["width"]
    if x0 < 0 < x1:
        part = w
        break
if part is None:
    print("NOPART")
    raise SystemExit
g = part["geometry"]
x0, x1 = max(0, g["x"]), min(SW, g["x"] + g["width"])
if x1 - x0 < 40:
    print("TOOSMALL %d" % (x1 - x0))
    raise SystemExit
# 上下两条采样带（避开客户端自己画的标题栏：取内容区 1/4 和 3/4 处各一条）
sx0, sx1 = x0 + 4, x1 - 4
y_top = g["y"] + g["height"] // 4
y_bot = g["y"] + 3 * g["height"] // 4
top = [cls(im[y_top, x]) for x in range(sx0, sx1)]
bot = [cls(im[y_bot, x]) for x in range(sx0, sx1)]
black = top.count(".") + bot.count(".")
# 可见的那一条对应窗口局部的哪一段 —— 期望的颜色由它算出来（不是写死的）
vl, vr = x0 - g["x"], x1 - g["x"]
half = g["width"] // 2
exp_top = "G" if vl >= half else ("R" if vr <= half else "GR")
exp_bot = "W" if vl >= half else ("B" if vr <= half else "BW")
exp_edge = g["x"] + half if exp_top == "GR" else None
# 分界：上半那条里 R->G 的跳变位置
edge = None
for i in range(1, len(top)):
    if top[i - 1] != top[i] and {top[i - 1], top[i]} == {"R", "G"}:
        edge = sx0 + i
        break
kinds_top = "".join(sorted(set(c for c in top if c in "RGBW")))
kinds_bot = "".join(sorted(set(c for c in bot if c in "RGBW")))
print("PART %d %d %d %d %d %d" % (g["x"], g["y"], g["width"], g["height"], x0, x1))
print("BLACK %d %d" % (black, (sx1 - sx0) * 2))
print("EDGE %s %s" % ("none" if edge is None else edge,
                      "none" if exp_edge is None else exp_edge))
print("KINDS %s %s %s %s" % (kinds_top, kinds_bot, exp_top, exp_bot))
PY
)
	echo "$res" | sed 's/^/      /'
	if echo "$res" | grep -q NOPART; then
		fail "没有「只有一部分在屏里」的窗口 —— 复现条件没到（布局没变？）"
		return
	fi
	if echo "$res" | grep -q TOOSMALL; then
		fail "可见的那一条太窄（$(echo "$res" | grep TOOSMALL | cut -d' ' -f2)px），量不了"
		return
	fi
	pass "复现条件在：$(echo "$res" | grep '^PART' | sed 's/^PART /窗口 /')"

	local black total
	black=$(echo "$res" | grep '^BLACK' | cut -d' ' -f2)
	total=$(echo "$res" | grep '^BLACK' | cut -d' ' -f3)
	if [ "$black" = "0" ]; then
		pass "可见的那一条里没有黑边（采样 $total 点，0 个黑）"
	else
		fail "可见的那一条里有黑边（$black/$total 个黑像素）—— 装饰把裁剩的一小条拉伸铺满了整块窗口"
	fi

	local edge exp_edge
	edge=$(echo "$res" | grep '^EDGE' | cut -d' ' -f2)
	exp_edge=$(echo "$res" | grep '^EDGE' | cut -d' ' -f3)
	if [ "$exp_edge" = "none" ]; then
		# 可见的那一条整个落在窗口的某一半里：本来就不该有分界
		if [ "$edge" = "none" ]; then
			pass "可见那一条整个落在窗口的一半里（没有分界，符合预期）"
		else
			fail "可见那一条里出现了不该有的分界（$edge）—— 整块窗口被塞进来了（缩放）"
		fi
	elif [ "$edge" = "none" ]; then
		fail "可见的那一条里找不到四象限的分界（画面被拉糊 / 拉偏了）"
	elif [ "$((edge - exp_edge))" -ge -4 ] && [ "$((edge - exp_edge))" -le 4 ]; then
		pass "四象限分界在窗口中心（$edge ≈ $exp_edge）—— 是裁出来的，不是缩放"
	else
		fail "四象限分界跑偏了（$edge，应当在 $exp_edge）—— 画面被横向拉伸了"
	fi

	local kt kb et eb
	kt=$(echo "$res" | grep '^KINDS' | cut -d' ' -f2)
	kb=$(echo "$res" | grep '^KINDS' | cut -d' ' -f3)
	et=$(echo "$res" | grep '^KINDS' | cut -d' ' -f4)
	eb=$(echo "$res" | grep '^KINDS' | cut -d' ' -f5)
	if [ "$kt" = "$et" ] && [ "$kb" = "$eb" ]; then
		pass "上下两半的颜色就是「可见的那一段窗口」该有的（上 $kt / 下 $kb）"
	else
		fail "可见那一条的颜色不对（上 $kt / 下 $kb，应当是上 $et / 下 $eb）"
	fi

	# 只看**测试自己起的那个**：以前这里是 `pgrep -x vidow`，而用户桌面会话也叫
	# 这个名字 —— 测试自己的 vidow 早死了，这条照样 PASS（永远测不出来的假绿）。
	if { [ -n "${VIDOW_PID:-}" ] && kill -0 "$VIDOW_PID" 2>/dev/null; } ||
	{ [ -n "${VIDOW_PGID:-}" ] && kill -0 -- "-$VIDOW_PGID" 2>/dev/null; }; then
		pass "vidow 还活着"
	else
		fail "vidow 死了（$(tail -3 "$dir/log" | tr '\n' '|')）"
	fi

	kill -TERM -- "-$VIDOW_PID" 2>/dev/null
	sleep 0.5
	kill -KILL -- "-$VIDOW_PID" 2>/dev/null
	VIDOW_PID=""
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	sleep 0.3
	for p in $CLIENT_PIDS; do kill -KILL "$p" 2>/dev/null; done
	CLIENT_PIDS=""
}

mkdir -p "$RUNDIR"
run_case "装饰开着" "true"
run_case "装饰关掉" "false"

echo
if [ "$FAILED" = 0 ]; then
	echo "结果：全过"
else
	echo "结果：有失败"
fi
exit "$FAILED"
