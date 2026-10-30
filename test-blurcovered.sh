#!/bin/bash
# 看不见的模糊不算：**内容整块不透明**的窗口不抓模糊底衬、不跑 kawase、不重烘。
#
# 装饰着色器最后两句是 out_px = content + out_px * (1 - content.a)，模糊那一项还乘着
# inside 和 step(u_mask_min, elem_a)：内容不透明的地方（content.a == 1）模糊被乘成 0，
# 内容全透明的地方又被剔掉。所以模糊只在**内容半透明**的窗口上看得见 —— 不透明窗口
# 那趟（整屏 kawase 金字塔 + 私有底衬抓取 + 「底衬版本变了就重烘这个窗口」）全是白算的。
# 判据在 fx.c 的 view_blur_visible（保守：拿不准就当看得见）。
#
# 场景：wbeat（每帧换色，出帧 ~30fps）当「一直在变的底衬」，上面压一个**静态**窗口：
#   1) 不透明那个（pattern.py，GTK 满窗不透明 → 格式/opaque region 整块不透明）：
#      模糊看不见 → 不抓底衬（own = 0）、不跑链条（chain = 0）、不重烘 ——
#      重烘次数/帧 该是 1.0（只有底下那个每帧在变的窗口重烘）
#   2) 半透明那个（sheet.py，整窗 alpha 0.5）：模糊看得见 → 照旧抓私有底衬、
#      跑链条、每帧重烘 —— 重烘/帧 回到 2.0。这一条是「判据不是一律不算模糊」的守门
#
#   3) 半透明那个被一个**不透明**窗口整块盖住：它一个像素都看不见了 → 私有底衬
#      也停抓（own = 0）。这是遮挡剔除的守门：以前被盖住的窗口照样每帧抓，
#      重叠的 N 个窗口各抓一份就是 O(n^2)。
#
# 实测（headless 1280x720@60，blur_all = true，VIDOW_NO_SCANOUT=1 逼走合成那条路）：
#   不透明：重烘 2.00 次/帧、own 372 次/6s、chain 372 遍、渲染趟数 3348
#     ↓ 改完（同一个场景）
#   不透明：重烘 1.00 次/帧、own 0、chain 0、渲染趟数 372（每帧少 8 趟）
#   半透明：own 照旧每帧一次、重烘 2.00 次/帧（模糊该算的还得算）
#
# 用法：./test-blurcovered.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
# 注意：别和别的测试并行跑 —— 收尾按 cmdline 杀 pattern.py / sheet.py / heartbeat.py。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-blurcovered-test
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
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*pattern.py*|*sheet.py*|*heartbeat.py*) kill "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

python3 -c "import gi; gi.require_version('Gtk','3.0')" 2>/dev/null ||
	{ echo "test-blurcovered.sh: 需要 python3-gobject + GTK3"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
scale = "1"
blur_enabled = true
blur_all = true
blur_levels = 4
blur_offset = 1.5
window_open_ms = 0
window_close_ms = 0
window_move_ms = 0
CFG

export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug VIDOW_RENDER_STATS=1 VIDOW_NO_SCANOUT=1
export DISPLAY= WAYLAND_DISPLAY=

setsid ./build/vidow > "$LOG" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 80); do grep -aq "vidow ready" "$LOG" && break; sleep 0.25; done
grep -aq "vidow ready" "$LOG" || { echo "vidow 没起来"; tail -5 "$LOG"; exit 1; }
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 | cut -d= -f2)
sleep 0.5
./build/vidowmsg output-mode HEADLESS-1 1280 720 60 >/dev/null 2>&1
sleep 0.8

msg() { ./build/vidowmsg "$@" >/dev/null 2>&1; }

# 上面压的那个窗口：浮动到 wbeat 上面，一半重叠
overlay_at() { # <标题>
	local t="$1"
	for _ in $(seq 1 12); do
		msg toggle-floating --title "$t"
		local f=$(./build/vidowmsg windows 2>/dev/null | python3 -c '
import sys, json
print(next((w["floating"] for w in json.load(sys.stdin) if w["title"] == sys.argv[1]), "?"))' "$t")
		[ "$f" = "True" ] && break
		sleep 0.3
	done
	msg set-window-size --title "$t" 500 400
	sleep 0.4
	msg set-window-position --title "$t" 300 250
	sleep 1.2
}

# 量这一段日志：重烘次数/帧、own、chain、渲染趟数
measure() { # <输出文件>
	tail -c +$((OFF + 1)) "$LOG" | grep -a "渲染统计" > "$1"
	OFF=$(wc -c < "$LOG")
}
OFF=0

mark "不透明的静态窗口压在每帧在变的窗口上"
setsid nohup python3 ./tools/heartbeat.py wbeat > "$RUNDIR/beat.log" 2>&1 </dev/null &
CLIENTS="$CLIENTS $!"
sleep 2.5
setsid nohup python3 ./tools/pattern.py wstatic > "$RUNDIR/pat.log" 2>&1 </dev/null &
PAT_PID=$!
CLIENTS="$CLIENTS $PAT_PID"
sleep 3
overlay_at wstatic
./build/vidowmsg windows 2>/dev/null | python3 -c '
import json, sys
ws = {w["title"]: w for w in json.load(sys.stdin)}
a, b = ws["wstatic"]["geometry"], ws["wbeat"]["geometry"]
ov = (a["x"] < b["x"] + b["width"] and b["x"] < a["x"] + a["width"] and
      a["y"] < b["y"] + b["height"] and b["y"] < a["y"] + a["height"])
print("  重叠=%s（wstatic %d,%d %dx%d / wbeat %d,%d %dx%d）" % (
    ov, a["x"], a["y"], a["width"], a["height"], b["x"], b["y"], b["width"], b["height"]))'
OFF=$(wc -c < "$LOG")
sleep 6
measure "$RUNDIR/stats-opaque.txt"
FRAMES=$(grep -ac ^frame "$RUNDIR/beat.log")
if [ "$FRAMES" -ge 60 ]; then
	pass "底下那个窗口一直在出帧（$FRAMES 帧，底衬真的在变）"
else
	fail "底下那个窗口只出了 $FRAMES 帧 —— 底衬没在变，下面两条判据不算数"
fi

read -r BAKES OWN CHAIN PASSES VFRAMES < <(python3 - "$RUNDIR/stats-opaque.txt" <<'PY'
import re, sys
rows = [l for l in open(sys.argv[1]) if l.strip()]
f = bakes = own = chain = passes = 0
for l in rows:
    m = re.search(r'渲染统计: (\d+) 帧/帧窗', l)
    if m: f += int(m.group(1))
    m = re.search(r'\(shared \d+ own (\d+) chain (\d+)\)', l)
    if m:
        own += int(m.group(1)); chain += int(m.group(2))
    m = re.search(r'装饰 [\d.]+ms/帧\((\d+) 次重烘\)', l)
    if m: bakes += int(m.group(1))
    m = re.search(r'趟 (\d+)', l)
    if m: passes += int(m.group(1))
print(bakes, own, chain, passes, f)
PY
)
echo "  不透明：vidow 画了 $VFRAMES 帧，重烘 $BAKES 次（$(
	python3 -c "print('%.2f' % ($BAKES / max($VFRAMES, 1)))") 次/帧）own $OWN chain $CHAIN 趟 $PASSES"
if python3 -c "import sys; sys.exit(0 if $BAKES <= 1.3 * max($VFRAMES, 1) else 1)"; then
	pass "不透明窗口没有每帧重烘（重烘 $(python3 -c "print('%.2f' % ($BAKES / max($VFRAMES, 1)))") 次/帧 ≤ 1.3；只该有底下那个在变的窗口）"
else
	fail "不透明窗口还在每帧重烘（$BAKES 次 / $VFRAMES 帧 ≈ $(python3 -c "print('%.2f' % ($BAKES / max($VFRAMES, 1)))") 次/帧）—— 看不见的模糊没去掉"
fi
if [ "$OWN" = "0" ] && [ "$CHAIN" = "0" ]; then
	pass "不透明窗口一次私有底衬都没抓（own 0 / chain 0）"
else
	fail "不透明窗口还在抓私有底衬（own $OWN / chain $CHAIN）—— 那笔模糊一个像素都透不出来"
fi

mark "半透明的静态窗口压在同一个位置（模糊该算的还得算）"
kill "$PAT_PID" 2>/dev/null
sleep 1.5
setsid nohup python3 ./tools/sheet.py wstatic 0.2 0.25 0.3 0.5 > "$RUNDIR/sheet.log" 2>&1 </dev/null &
SHEET_PID=$!
CLIENTS="$CLIENTS $SHEET_PID"
sleep 3
overlay_at wstatic
OFF=$(wc -c < "$LOG")
sleep 6
measure "$RUNDIR/stats-translucent.txt"
read -r BAKES2 OWN2 CHAIN2 PASSES2 VFRAMES2 < <(python3 - "$RUNDIR/stats-translucent.txt" <<'PY'
import re, sys
rows = [l for l in open(sys.argv[1]) if l.strip()]
f = bakes = own = chain = passes = 0
for l in rows:
    m = re.search(r'渲染统计: (\d+) 帧/帧窗', l)
    if m: f += int(m.group(1))
    m = re.search(r'\(shared \d+ own (\d+) chain (\d+)\)', l)
    if m:
        own += int(m.group(1)); chain += int(m.group(2))
    m = re.search(r'装饰 [\d.]+ms/帧\((\d+) 次重烘\)', l)
    if m: bakes += int(m.group(1))
    m = re.search(r'趟 (\d+)', l)
    if m: passes += int(m.group(1))
print(bakes, own, chain, passes, f)
PY
)
echo "  半透明：重烘 $BAKES2 次 own $OWN2 chain $CHAIN2 趟 $PASSES2"
# 半透明窗口的底衬有两个来源：自己的私有抓取（own），或者「layer 之下」的共享
# 抓取（under，见 test-layershare.sh）。只要有一边在抓，它的模糊就是新鲜的。
SPENT2=$(grep -a "渲染统计" "$LOG" | tail -3 | python3 -c '
import re, sys
u = o = 0
for l in sys.stdin:
    m = re.search(r"own (\d+) under (\d+)", l)
    if m: o += int(m.group(1)); u += int(m.group(2))
print(o + u)')
if [ "$SPENT2" -ge 10 ] 2>/dev/null; then
	pass "半透明窗口照旧每帧有底衬可采（own $OWN2 + under 合计 $SPENT2）—— 判据没把该模糊的一起砍掉"
else
	fail "半透明窗口的底衬只剩 $SPENT2 次（own $OWN2）—— 判据砍多了，半透明窗口的模糊也没了"
fi
if python3 -c "import sys; sys.exit(0 if $BAKES2 >= 1.5 * max($VFRAMES2, 1) else 1)"; then
	pass "半透明窗口自己也每帧重烘（重烘 $(python3 -c "print('%.2f' % ($BAKES2 / max($VFRAMES2, 1)))") 次/帧 ≥ 1.5）"
else
	fail "半透明窗口没有每帧重烘（$BAKES2 次 / $VFRAMES2 帧）—— 它的模糊底衬该跟着底衬变"
fi

mark "半透明窗口被不透明窗口整块盖住（看不见了就不该再抓）"
kill "$SHEET_PID" 2>/dev/null
sleep 1.2
# 换成不透明的 pattern.py：整块盖住它，而且后开 = z 序在上
setsid nohup python3 ./tools/pattern.py wstatic > "$RUNDIR/pat2.log" 2>&1 </dev/null &
PAT2_PID=$!
CLIENTS="$CLIENTS $PAT2_PID"
sleep 2.5
overlay_at wstatic
msg set-window-size --title wstatic 900 620
sleep 0.6
msg set-window-position --title wstatic 0 0
sleep 1.2
OFF=$(wc -c < "$LOG")
sleep 6
measure "$RUNDIR/stats-covered.txt"
read -r BAKES3 OWN3 CHAIN3 VFRAMES3 < <(python3 - "$RUNDIR/stats-covered.txt" <<'PY'
import re, sys
f = bakes = own = chain = 0
for l in open(sys.argv[1]):
    m = re.search(r'渲染统计: (\d+) 帧/帧窗', l)
    if m: f += int(m.group(1))
    m = re.search(r'\(shared \d+ own (\d+) chain (\d+)\)', l)
    if m:
        own += int(m.group(1)); chain += int(m.group(2))
    m = re.search(r'装饰 [\d.]+ms/帧\((\d+) 次重烘\)', l)
    if m: bakes += int(m.group(1))
print(bakes, own, chain, f)
PY
)
echo "  被盖住之后：重烘 $BAKES3 次 own $OWN3 chain $CHAIN3（$VFRAMES3 帧）"
if [ "$OWN3" = "0" ] && [ "$CHAIN3" = "0" ]; then
	pass "被整块盖住的窗口不再抓私有底衬（own 0 / chain 0）"
else
	fail "被整块盖住的窗口还在抓（own $OWN3 / chain $CHAIN3）—— 遮挡剔除没生效"
fi

# 只看**测试自己起的那个**：以前这里是 `pgrep -x vidow`，而用户桌面会话也叫
# 这个名字 —— 测试自己的 vidow 早死了，这条照样 PASS（永远测不出来的假绿）。
if { [ -n "${VIDOW_PID:-}" ] && kill -0 "$VIDOW_PID" 2>/dev/null; } ||
{ [ -n "${VIDOW_PGID:-}" ] && kill -0 -- "-$VIDOW_PGID" 2>/dev/null; }; then
	pass "vidow 还活着"
else
	fail "vidow 死了（$(tail -3 "$LOG" | tr '\n' '|')）"
fi

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-blurcovered.sh: OK"
else
	echo "test-blurcovered.sh: 有失败"
fi
exit "$FAILED"
