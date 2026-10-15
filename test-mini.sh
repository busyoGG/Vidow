#!/bin/bash
# 小窗模式（mini window，S1–S5 + S7）：把散在 /tmp 的那几个临时验证脚本收成一个。
#
# 「小窗」是浮动窗口上的一个**形态**（`none` / `free` / `mini` / `icon`），进小窗
# 不重配客户端（保留原生 buffer，由合成器缩放 + 指针坐标换算）。这个脚本按
# 「一层一个 mark」验：
#
#   S1  形态 IPC / 循环 / 清单 / 还原（`mini-window` / `set-mini-state` /
#       `cycle-mini-state` / `unmini-window` / `mini-windows`）
#   S2  跟屏（切工作区 geometry 逐字段不变；小窗不跟工作区）
#       焦点守卫（`focused` 不指向小窗；`focus_follows_mouse = true` 也不抢）
#       输入路由（迷你 / 图标态客户端收不到 motion / scroll / key；FREE 态收得到）
#   S3  原生 buffer 缩放（客户端未被重配：`surface` 尺寸不变，进小窗时新发的
#       configure 尺寸照旧是原生值）、图标贴边裁剪（相邻屏那条「本该溢出」的条带
#       非背景像素 = 0）、指针坐标换算（客户端收到的坐标 = 原生对应点 ±3px）
#   S3b `mini_aspect` 三值（全局 free / lock / auto + 运行时键位翻转压过全局）
#   S4  落点区（拖进右上角松手 = mini；拖到别处松手不误触）、内边缘 → icon、
#       跨屏边 → 换屏保持形态、滚轮降档、悬停工具条（点「还原成列」）
#   S5  `force_render` 分叉（关 = 图标与预览都静止且客户端 0 新帧；开 = 都活）、
#       过渡动画（相邻位移 ≤ 屏宽 10%、单调、第一帧在起点）、惯性甩出
#   S5b MRU（Alt+Tab）提交选中迷你 / 图标态小窗 → 展开成自由小窗并拿到焦点
#   S7  HiDPI（scale 2）：运行时把 HEADLESS-2 调到 scale 2、邻屏摆到它右边，验
#       输出逻辑尺寸（物理 / 2）/ 几何按**逻辑**工作区算（物理脚印 2 倍）/ 跨缩放
#       边界（scale 2 → scale 1）的图标裁剪（像素差分，非「非背景」）/ 指针坐标换算
#
# **这个脚本抓到过的三个真 bug（都已修，判据留在脚本里当回归）**：
#   1) 图标漏到相邻屏上（S3「相邻屏非背景像素 = 0」那条）：贴右边缘溢出的那 16×64
#      条带被隔壁那块屏照画，修前 832/1024 非背景像素（其中 172 个是客户端纯红），
#      修后 0/1024。根因两处，都在「缩放节点不能借 wlroots 那套 1:1 语义的裁剪」上：
#        · `src/view.c` 的 `view_scaled_crop()`：缩放态（图标 / 迷你 / 保留原生
#          buffer 的自由小窗、以及过渡动画那几帧）**不再走**
#          `wlr_scene_subsurface_tree_set_clip()`（它内部把 clip 当节点位置用、
#          src_box 只按表面尺寸比例缩、dst 不动 —— 缩放态下内容会被挪位 + 拉伸，
#          而且越界那半块照画），改成「源框取可见那一块、目标框也取可见那一块」；
#        · `src/view.c` 的 `mini_snapshot_place()`：图标快照那份节点挂在**全局**
#          layer_toplevels 上（不在窗口自己的场景树里），以前一个裁剪都没有 ——
#          按自己那块屏的框裁掉框外那部分（判据不能用 `view_clip_box()`：它对停到
#          停车位的窗口按设计就是「不裁」）。
#   2) 工具条点过一次按钮之后 `server->mini_toolbar_pressed` 留在 true
#      （`src/input.c` 的 `vidow_pointer_button()` 里，松手那一支以前写在
#      `if (server->mini_toolbar_view != NULL)` 里面，而按下那条路紧接着
#      `vidow_mini_toolbar_hide()` 就把 view 清成 NULL）—— 悬停着工具条的迷你窗口上
#      下一对按下/松开被吞：形态不变（按一下没反应），而且那次按下起的 mini_grab
#      再没人收尾 —— 紧接着一次**不带按键**的 pointer-motion 就把窗口拖走（修前
#      现场 `place w1 -> 464,506`）。修法：松手那一半无条件清标志，并且标志被吞过、
#      grab 还挂着时接着走小窗那条松手把它一起收掉。回归判据见 S4 那节最后三条
#      （单击放大 / 无按键 motion 不拖窗口）。
#
#   3) HiDPI（运行时 `output-scale` 改到 2）之后**重进**小窗，盒里的内容只有该有的
#      一半大（判据是 S7「盒里内容 = contain 几何的 2 倍物理像素」）：修前脚印
#      86x95 物理（该 172x190），修后 172x190。根因在 `src/view.c`：`view_dst_capture()`
#      记 1:1 基线时兜底读的是**缓冲像素**（缩放 2 的客户端缓冲是逻辑的两倍），而
#      `view_mini_native()` 优先读这份缓存、xdg 那条路又只在小窗退出那一刻记一次
#      （读到的时刻客户端还没按新逻辑尺寸交帧）→ 永久过期。修法与取证见 README 278。
#
# **怎么注入**：headless 里没有真鼠标 / 真键盘，用 `VIDOW_TEST_ACTION` 的 FIFO
# 形式发 `pointer-motion x y` / `pointer-button left press|release mod`。它走的是和
# 真实输入**完全同一条路**（`vidow_pointer_motion` / `vidow_pointer_button`）。
# 客户端探针（`$RUNDIR/probe.py`，现场生成）画纯红 + 每帧打一行 `frame`，并把它
# 收到的 motion / scroll / key / configure 都打出来 —— 输入路由和坐标换算都靠它。
#
# **不测（时序太敏感，宁缺勿假）**：
#   - 滑移 / 触摸板手势那条路（小窗只接指针）
#   - X11 `PAspect`：需要一个能设 aspect hints 的 X11 客户端；本脚本没有，
#     所以「全局 free + X11 PAspect 不再自动锁」「改回 auto 自动锁」这两条只在
#     代码里（`vidow_mini_aspect_effective` 的三级优先级 + `mini_aspect` 的
#     `auto` 分支），不在这儿断言。本机 Xwayland 跑起来时手工验过，见 README 274。
#
# 用法：./test-mini.sh    （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=${VIDOW_MINI_RUNDIR:-/tmp/vidow-mini-test}
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
PLOG="$RUNDIR/probe.log"
FAILED=0
VIDOW_PGID=""
CLIENT_PIDS=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	exec 9>&- 2>/dev/null
	for p in $CLIENT_PIDS; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PGID" ] && kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PGID" ] && kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

command -v grim >/dev/null || { echo "test-mini.sh: 需要 grim"; exit 1; }
python3 -c "import numpy, PIL" 2>/dev/null ||
	{ echo "test-mini.sh: 需要 python3-numpy + python3-pil"; exit 1; }
python3 -c "import gi; gi.require_version('Gtk','3.0')" 2>/dev/null ||
	{ echo "test-mini.sh: 需要 python3-gobject + gtk3"; exit 1; }
[ -x ./build/vidow ] || { echo "test-mini.sh: 先 ninja -C build"; exit 1; }

rm -rf "$RUNDIR"; mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
# 两块屏，谁在左边写死（headless 自动排是反的）。工作区 = 10,10 1260x700。
# focus_follows_mouse 开着是**故意的**：焦点守卫那条判据要在它开着的时候仍然成立。
# 规则那条用标题 "rulewin" 匹配（w2 一开出来就是图标态）。
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
vidow.output{ name = "HEADLESS-1", x = 0 }
vidow.output{ name = "HEADLESS-2", x = 1280 }
vidow.bind("Mod+mouse-left", "interactive-move")
vidow.bind("Mod+mouse-right", "interactive-resize")
scale = "1"
gaps = 10
window_move_ms = 0
mini_transition_ms = 600
mini_icon_overflow = 0.4
focus_follows_mouse = true
vidow.rule{ title = "rulewin", mini_state = "icon", mini_aspect = "lock",
            mini_preserve_buffer = false }
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
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" | tail -1 |
	cut -d= -f2)

exec 9> "$RUNDIR/actions"
act() { echo "$1" >&9; sleep "${2:-0.25}"; }

# ---------------------------------------------------------------- 客户端探针
cat > "$RUNDIR/probe.py" <<'PY'
#!/usr/bin/env python3
# 小窗测试的客户端探针：画纯红 + 每帧打一行 frame（量出帧），并把它收到的
# configure / motion / scroll / key 打出来（量输入路由与坐标换算）。
import sys
import time

import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, Gdk  # noqa: E402

T0 = time.monotonic()
N = [0]


def log(m):
    print("probe: %s" % m, flush=True)


class Probe(Gtk.Window):
    def __init__(self):
        super().__init__(title=sys.argv[1] if len(sys.argv) > 1 else 'probe')
        self.set_default_size(400, 300)
        self.add_events(Gdk.EventMask.POINTER_MOTION_MASK |
                        Gdk.EventMask.BUTTON_PRESS_MASK |
                        Gdk.EventMask.BUTTON_RELEASE_MASK |
                        Gdk.EventMask.SCROLL_MASK |
                        Gdk.EventMask.ENTER_NOTIFY_MASK |
                        Gdk.EventMask.KEY_PRESS_MASK)
        self.connect('draw', self.on_draw)
        self.connect('configure-event', lambda w, e: log("configure %dx%d" % (e.width, e.height)))
        self.connect('motion-notify-event', lambda w, e: log("motion %.1f,%.1f" % (e.x, e.y)))
        self.connect('scroll-event', lambda w, e: log("scroll %.1f,%.1f" % (e.delta_x, e.delta_y)))
        self.connect('key-press-event', lambda w, e: log("key"))
        self.connect('button-press-event', lambda w, e: log("press"))
        self.connect('button-release-event', lambda w, e: log("release"))
        self.connect('enter-notify-event', lambda w, e: log("enter"))
        self.connect('destroy', Gtk.main_quit)

    def on_draw(self, widget, cr):
        N[0] += 1
        w = widget.get_allocated_width()
        h = widget.get_allocated_height()
        # 底色纯红（像素判据用它）+ 一条**每帧都在动**的白条：两帧截图一不一样
        # 就是「它还在不在出帧」的判据（纯色窗口两帧永远相同，那条会假通过）。
        cr.set_source_rgb(1, 0, 0)
        cr.rectangle(0, 0, w, h)
        cr.fill()
        cr.set_source_rgb(1, 1, 1)
        cr.rectangle((N[0] % 20) * (max(w, 1) / 20.0), 0, max(w / 20.0, 2), h)
        cr.fill()
        print("frame %d %d" % (N[0], int((time.monotonic() - T0) * 1000)), flush=True)
        widget.queue_draw()      # 立刻再要一帧：出帧速度由合成器的帧回调决定
        return False


Probe().show_all()
Gtk.main()
PY

python3 ./tools/wall.py "#ff00ff" >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.2
python3 "$RUNDIR/probe.py" w1 > "$PLOG" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.8

# ---------------------------------------------------------------- 小工具
# 窗口字段一行一个：id / x y w h / output / mini / 可见 / 工作区 / 焦点 /
# 浮动 / surface 尺寸（原生逻辑尺寸）
winf() {  # $1 = 标题，$2 = 字段
	./build/vidowmsg windows | python3 -c '
import sys, json
t, f = sys.argv[1], sys.argv[2]
for w in json.load(sys.stdin):
    if w["title"] != t:
        continue
    g = w["float_geometry"] if w["floating"] else w["geometry"]
    s = w.get("surface") or {}
    v = {"id": w["id"], "x": g["x"], "y": g["y"], "w": g["width"],
         "h": g["height"], "out": w["output"], "mini": w["mini"],
         "vis": w["visible"], "ws": w["workspace"], "foc": w["focused"],
         "float": w["floating"], "sw": s.get("width"), "sh": s.get("height"),
         "edge": w.get("mini_edge")}[f]
    print(v)
    break
' "$1" "$2"
}
geom() { echo "$(winf w1 x),$(winf w1 y) $(winf w1 w)x$(winf w1 h)"; }
# 探针收到的事件条数
pev() { grep -ac "^probe: $1" "$PLOG" 2>/dev/null || true; }
frame() { grep -ac "^frame " "$PLOG" 2>/dev/null || true; }
mini_id() { winf w1 id; }
set_state() { ./build/vidowmsg set-mini-state --id "$(mini_id)" --state "$1" >/dev/null 2>&1; }
unmini() { ./build/vidowmsg unmini-window --id "$(mini_id)" >/dev/null 2>&1; }
# 非背景像素数（背景 = wall.py 的纯洋红 #ff00ff）
nonbg() {
	python3 - "$1" <<'PY'
import sys
from PIL import Image
import numpy as np
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
d = (np.abs(a - np.array([255, 0, 255], dtype=np.int16)).sum(axis=2) > 6)
print(int(d.sum()))
PY
}
# 非壁纸像素的**外接框**（物理 px，"w h"）：小窗盒里客户端内容到底铺了多大一块。
# 背景 = wall.py 的纯洋红 #ff00ff（和 nonbg 同一套判据）。
nonbgbox() {
	python3 - "$1" <<'PY'
import sys
from PIL import Image
import numpy as np
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
m = (np.abs(a - np.array([255, 0, 255], dtype=np.int16)).sum(axis=2) > 6)
ys, xs = np.nonzero(m)
if len(xs) == 0:
    print("0 0")
else:
    print(int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1))
PY
}
# 这块图里有多少「非底色」像素（底色 = 该区域出现最多的那个颜色）：
# 量工具条按钮上有没有**字形**（纯色按钮 = 0），以及整条有没有内容。
inkpix() {
	python3 - "$1" <<'PY'
import sys
from collections import Counter
from PIL import Image
import numpy as np
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
c = Counter(map(tuple, a.reshape(-1, 3)))
base = np.array(c.most_common(1)[0][0], dtype=np.int16)
d = (np.abs(a - base).sum(axis=2) > 24)
print("%d/%d" % (int(d.sum()), a.shape[0] * a.shape[1]))
PY
}
shot() {  # $1 输出文件，$2..$5 = x y w h（grim 的几何是 "x,y WxH"！）
	local sz
	if [ "${4:-0}" -le 0 ] || [ "${5:-0}" -le 0 ]; then
		fail "grim 区域是空的：$2,$3 ${4}x${5}"
		return 1
	fi
	grim -t ppm -g "$2,$3 ${4}x${5}" - > "$1" 2>/dev/null
	sz=$(stat -c%s "$1" 2>/dev/null || echo 0)
	if [ "$sz" -le 100 ]; then
		fail "grim 截图为空：$2,$3 ${4}x${5}（$sz 字节）"
		return 1
	fi
	return 0
}
same() { python3 -c '
import sys
sys.exit(0 if open(sys.argv[1],"rb").read() == open(sys.argv[2],"rb").read() else 1)' \
	"$1" "$2"; }
diffpct() { python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
import numpy as np
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
print("%.1f" % ((np.abs(a - b).sum(axis=2) > 6).mean() * 100.0))
PY
}
# 最早的「这一趟之后」的新日志行号（给「只看这一段」的断言用）
logmark() { wc -l < "$LOG"; }
since() { tail -n +"$1" "$LOG"; }
# 同上，但看的是**客户端探针**的日志（客户端自己收到的 configure / motion）
plogmark() { wc -l < "$PLOG"; }
psince() { tail -n +"$1" "$PLOG"; }

# ================================================================ S1：形态 IPC
mark "S1 形态 IPC：三态 / 循环 / 清单 / 还原"
ID=$(mini_id)
[ -n "$ID" ] && pass "拿到窗口 id = $ID" || fail "没拿到窗口 id"
# 默认值：mini_aspect = "free"（自由小窗是自由摆放的正常窗户，默认不锁比例 —— 用户
# 报的「自由小窗默认应该不限制比例」）。得在 S3b **之前**查：那一节会显式写三个取值。
DEF_ASPECT=$(./build/vidowmsg config mini_aspect |
	python3 -c 'import sys,json;print(json.load(sys.stdin)["value"])')
[ "$DEF_ASPECT" = "free" ] && pass "mini_aspect 默认 = free（vidowmsg config）" ||
	fail "mini_aspect 默认不是 free：$DEF_ASPECT"

./build/vidowmsg mini-window --id "$ID" >/dev/null
sleep 1.0
[ "$(winf w1 mini)" = "mini" ] && pass "mini-window → 形态 mini" ||
	fail "mini-window 没进迷你态：$(winf w1 mini)"
[ "$(winf w1 ws)" = "None" ] && pass "小窗不属于任何工作区（ws = null，屏级对象）" ||
	fail "小窗还挂在工作区上：$(winf w1 ws)"
[ "$(winf w1 vis)" = "True" ] && pass "小窗可见（可见性旁路生效）" ||
	fail "小窗被判成不可见：$(winf w1 vis)"
N=$(./build/vidowmsg mini-windows | python3 -c 'import sys,json;print(len(json.load(sys.stdin)))')
[ "$N" = "1" ] && pass "mini-windows 清单里有 1 个" || fail "mini-windows 清单数不对：$N"

set_state free; sleep 0.8
[ "$(winf w1 mini)" = "free" ] && pass "set-mini-state free" || fail "set-mini-state free 没生效"
./build/vidowmsg cycle-mini-state --id "$ID" >/dev/null; sleep 0.8
[ "$(winf w1 mini)" = "mini" ] && pass "三态循环 free → mini" || fail "循环 free → $(winf w1 mini)"
./build/vidowmsg cycle-mini-state --id "$ID" >/dev/null; sleep 0.8
[ "$(winf w1 mini)" = "icon" ] && pass "三态循环 mini → icon" || fail "循环 mini → $(winf w1 mini)"
./build/vidowmsg cycle-mini-state --id "$ID" >/dev/null; sleep 0.8
[ "$(winf w1 mini)" = "free" ] && pass "三态循环 icon → free" || fail "循环 icon → $(winf w1 mini)"
unmini; sleep 0.8
[ "$(winf w1 mini)" = "none" ] && pass "unmini-window → 还原成列（none）" ||
	fail "unmini 没还原：$(winf w1 mini)"
COL=$(./build/vidowmsg columns | python3 -c 'import sys,json;print(len(json.load(sys.stdin)))')
[ "$COL" -ge 1 ] && pass "还原后回到列里（columns = $COL）" || fail "还原后没有列"

# ================================================================ S2：跟屏
mark "S2 跟屏：切工作区 geometry 逐字段不变"
set_state mini; sleep 1.0
G0="$(geom) $(winf w1 out) $(winf w1 sw)x$(winf w1 sh)"
./build/vidowmsg focus-workspace 2 >/dev/null; sleep 0.9
G1="$(geom) $(winf w1 out) $(winf w1 sw)x$(winf w1 sh)"
[ "$G1" = "$G0" ] && pass "切到工作区 2：geometry / 归属 / surface 逐字段不变（$G1）" ||
	fail "切工作区动了几何：$G0 → $G1"
[ "$(winf w1 vis)" = "True" ] && pass "切走之后小窗照样在屏上（visible）" ||
	fail "切工作区把小窗藏了"
./build/vidowmsg focus-workspace 1 >/dev/null; sleep 0.9
G2="$(geom) $(winf w1 out) $(winf w1 sw)x$(winf w1 sh)"
[ "$G2" = "$G0" ] && pass "切回工作区 1：还是同一份 geometry（$G2）" ||
	fail "切回来变样了：$G0 → $G2"

mark "S2 焦点守卫：focused 不是小窗，FFM=true 也不抢"
FOC=$(./build/vidowmsg focused 2>/dev/null | grep -ac 'w1' || true)
[ "$FOC" = "0" ] && pass "迷你态 focused 不是小窗（进小窗时焦点交出去了）" || fail "小窗拿到了焦点"
[ "$(./build/vidowmsg config focus_follows_mouse | grep -cw true)" = "1" ] &&
	pass "focus_follows_mouse = true（下面这条判据就是在它开着时验的）" ||
	fail "focus_follows_mouse 不是 true，焦点守卫这条没验到"
# FFM 那条守卫：指针压到迷你小窗上，焦点也不许给它。
# （放在 focus-window 那条**之前** —— focus-window 的语义是「跳到这个窗口」，
#   它会把迷你先摊成自由小窗再聚焦，那之后 FFM 就有理由聚焦它了。）
read -r MX MY MW MH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
act "pointer-motion $((MX + MW / 2)) $((MY + MH / 2))" 0.4
FOC2=$(./build/vidowmsg focused 2>/dev/null | grep -ac 'w1' || true)
[ "$FOC2" = "0" ] && pass "指针压到小窗中心，焦点照旧不给它（FFM 被守卫拦下）" ||
	fail "悬停聚焦把小窗点亮了"
# focus-window 是「跳到这个窗口」：迷你 / 图标态会先摊成自由小窗再聚焦（IPC 语义）
./build/vidowmsg focus-window --id "$ID" >/dev/null 2>&1; sleep 1.0
[ "$(winf w1 mini)" = "free" ] && pass "focus-window 对迷你态：先摊成自由小窗（IPC 语义）" ||
	fail "focus-window 没有摊成自由小窗：$(winf w1 mini)"
[ "$(winf w1 foc)" = "True" ] && pass "摊开之后焦点给了它（focused = true）" ||
	fail "摊开之后没给焦点"
set_state mini; sleep 1.0

# ================================================================ S3：缩放 / 路由 / 坐标
mark "S3 原生 buffer：迷你 / 图标保留（不重配），FREE 按盒子重配客户端"
# `mini_preserve_buffer` 的语义**只管迷你 / 图标态**（缩略图：保留原生 buffer、由
# 合成器缩着画）；自由小窗不受它影响 —— 盒子多大就 configure 客户端多大，客户端
# 跟着重新排版。用户报的「自由小窗改大小被限制了比例」就是保留缓冲那条路：改盒子
# 只改留白，内容比例一动不动。
unmini; sleep 0.8
SW0=$(winf w1 sw); SH0=$(winf w1 sh); CF0=$(pev configure)
echo "  平铺态 surface = ${SW0}x${SH0}，客户端 configure 事件 $CF0 条"
MARK=$(logmark)
PMARK0=$(plogmark)
set_state mini; sleep 1.0
SWM=$(winf w1 sw); SHM=$(winf w1 sh)
[ "$SWM" = "$SW0" ] && [ "$SHM" = "$SH0" ] &&
	pass "进迷你态：客户端 surface 尺寸不变（${SWM}x${SHM} = 原生，保留 buffer 那档）" ||
	fail "迷你态客户端被重配了：${SW0}x${SH0} → ${SWM}x${SHM}"
# 进小窗会多出一条 configure（set_tiled 的状态位变化，niri 那条路一样）—— 判据
# 不是「一条都不许多」，而是**多出来的那些尺寸还是原生的**（尺寸才是重配）。
# 看的是**客户端探针的日志**（psince）：configure 是客户端自己收到的事件，合成器
# 日志里没有它 —— 以前这里读的是 vidow 日志，于是永远 0 条、判据空转。
BAD=$(psince "$PMARK0" | grep -a 'probe: configure' | grep -avc "configure ${SW0}x${SH0}" || true)
NEW=$(psince "$PMARK0" | grep -ac 'probe: configure' || true)
if [ "${BAD:-0}" = "0" ]; then
	pass "进迷你态后客户端收到的 configure（$NEW 条）尺寸全是原生 ${SW0}x${SH0}"
else
	fail "迷你态客户端被重配成别的尺寸了：$(psince "$PMARK0" | grep -a 'probe: configure' | tail -1)"
fi
MARK=$(logmark)
PMARK1=$(plogmark)
set_state free; sleep 1.4
SW1=$(winf w1 sw); SH1=$(winf w1 sh)
FB=$(geom)
[ "$SW1" != "$SW0" ] &&
	pass "进自由小窗：客户端**被重配成盒子尺寸**（${SW0}x${SH0} → ${SW1}x${SH1}，盒 $FB）" ||
	fail "自由小窗里客户端没被重配（还是 ${SW1}x${SH1}）—— FREE 该走 configure 那条路"
psince "$PMARK1" | grep -aq "probe: configure ${SW1}x${SH1}" &&
	pass "客户端自己收到 configure ${SW1}x${SH1}（probe 日志）" ||
	fail "客户端没收到 configure ${SW1}x${SH1}：$(psince "$PMARK1" | grep -a 'probe: configure' | tail -1)"
echo "  自由小窗盒 = $FB（1:1：盒里就是客户端自己的窗口几何，surface 比它大的是 CSD 阴影那圈）"

mark "S3 输入路由：迷你 / 图标态收不到，FREE 态收得到"
set_state mini; sleep 1.0
read -r MX MY MW MH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
M0=$(pev motion); S0=$(pev scroll); K0=$(pev key)
act "pointer-motion $((MX + MW / 2)) $((MY + MH / 2))" 0.2
act "pointer-motion $((MX + MW / 2 + 20)) $((MY + MH / 2 + 10))" 0.2
./build/vidowmsg pointer-scroll 0 1 >/dev/null 2>&1; sleep 0.3
./build/vidowmsg send-key tap Super+X >/dev/null 2>&1; sleep 0.3
M1=$(pev motion); S1=$(pev scroll); K1=$(pev key)
[ "$M1" = "$M0" ] && pass "迷你态：客户端收到 0 个 motion（$M0 → $M1）" ||
	fail "迷你态客户端收到了 motion：$M0 → $M1"
[ "$S1" = "$S0" ] && pass "迷你态：客户端收到 0 个 scroll" || fail "迷你态客户端收到了 scroll"
[ "$K1" = "$K0" ] && pass "迷你态：客户端收到 0 个 key" || fail "迷你态客户端收到了 key"

set_state free; sleep 1.0
# 自由小窗是可交互态，守卫只拦迷你 / 图标那两档（focus-window 走同一个入口）
./build/vidowmsg focus-window --id "$ID" >/dev/null 2>&1; sleep 0.4
[ "$(winf w1 foc)" = "True" ] &&
	pass "自由小窗可以聚焦（focus-window 生效；迷你 / 图标态那条被守卫拦下）" ||
	fail "自由小窗拿不到焦点：$(winf w1 foc)"
read -r FX FY FW FH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
M2=$(pev motion); S2=$(pev scroll); K2=$(pev key)
act "pointer-motion $((FX + FW / 2)) $((FY + FH / 2))" 0.3
act "pointer-motion $((FX + FW / 2 + 10)) $((FY + FH / 2 + 6))" 0.3
./build/vidowmsg send-key tap Super+X >/dev/null 2>&1; sleep 0.3
M3=$(pev motion); K3=$(pev key)
[ "$M3" -gt "$M2" ] && pass "自由小窗：客户端收到 motion（$M2 → $M3）" ||
	fail "自由小窗收不到 motion（输入路由吞过头了）"
[ "$K3" -gt "$K2" ] && pass "自由小窗：客户端收到 key" || fail "自由小窗收不到 key"
# FREE 态「上滚转发给客户端」（唯一不吞滚轮的情形）：下滚是降档，留给 S4 验
./build/vidowmsg pointer-scroll 0 -1 >/dev/null 2>&1; sleep 0.3
S3=$(pev scroll)
[ "$S3" -gt "$S2" ] && pass "自由小窗：上滚转发给客户端（$S2 → $S3）" ||
	fail "自由小窗的上滚被吞了"

mark "S3 指针坐标换算：客户端收到的坐标 = 盒内对应点 ±3px"
# FREE 是 **1:1** 的：盒子里就是客户端自己的窗口几何（盒子多大就 configure 多大），
# surface 比盒子大的那一圈是 CSD 阴影（居中）。换算 = 盒内相对位置 + 阴影偏移。
read -r EX EY <<< "$(python3 - "$FX" "$FY" "$FW" "$FH" "$SW1" "$SH1" <<'PY'
import sys
bx, by, bw, bh, sw, sh = (float(v) for v in sys.argv[1:7])
# 盒内取一点（离边 1/3、2/5），期望它落到客户端自己的哪一点
px = bx + bw * 0.33
py = by + bh * 0.40
print("%.1f %.1f" % (px - bx + (sw - bw) / 2.0, py - by + (sh - bh) / 2.0))
PY
)"
PX=$(python3 -c "print(int($FX + $FW * 0.33))")
PY_=$(python3 -c "print(int($FY + $FH * 0.40))")
act "pointer-motion $((PX - 6)) $((PY_ - 4))" 0.20
act "pointer-motion $PX $PY_" 0.35
GOT=$(grep -a '^probe: motion' "$PLOG" | tail -1 |
	sed -n 's/probe: motion \([0-9.-]*\),\([0-9.-]*\)/\1 \2/p')
echo "  注入 $PX,$PY_ → 期望客户端 $(echo "$EX $EY" | awk '{printf "%.1f,%.1f", $1, $2}')，实收 $GOT"
python3 - "$GOT" "$EX" "$EY" <<'PY'
import sys
gx, gy = (float(v) for v in sys.argv[1].split())
ex, ey = float(sys.argv[2]), float(sys.argv[3])
d = max(abs(gx - ex), abs(gy - ey))
print("  PASS  坐标换算对上了（差 %.1f px）" % d if d <= 3.0 else
      "  FAIL  坐标换算不对：实收 %.1f,%.1f 期望 %.1f,%.1f（差 %.1f px）" % (gx, gy, ex, ey, d))
sys.exit(0 if d <= 3.0 else 1)
PY
[ $? -eq 0 ] || FAILED=1

# ================================================================ S3：图标裁剪
mark "S3 迷你态命中：整个盒子都算窗口（左右留白也要抓得住）"
# 用户报的：「小窗左右有死区、拖不动、窗口直接卡在范围内」。根因：内容是按比例
# contain 摆在盒子**中间**的（345x318 的内容装进 353x196 的盒子 → 左右各留 ~70
# 像素留白），而命中测试只认内容节点 ⇒ 留白按下去不算窗口、拖不起来。修法：迷你态
# 命中按整个盒子算（view.c 的 mini_box_view_at，盒子取 float_geom + anim_off）。
box() {
	./build/vidowmsg windows | python3 -c '
import sys, json
for w in json.load(sys.stdin):
    if w["title"] == "w1":
        g = w["float_geometry"] or w["geometry"]
        print(g["x"], g["y"], g["width"], g["height"])
        break'
}
hit() { ./build/vidowmsg pick-window "$1" "$2" 2>/dev/null | grep -q "w1" && echo yes || echo no; }
set_state mini; sleep 1.2
read -r X Y W H <<< "$(box)"
echo "  迷你盒 $X,$Y ${W}x${H}（原生内容 $(winf w1 sw)x$(winf w1 sh) → 左右有留白）"
L=$(hit $((X + 3)) $((Y + H / 2)))
R=$(hit $((X + W - 3)) $((Y + H / 2)))
C=$(hit $((X + W / 2)) $((Y + H / 2)))
if [ "$L" = yes ] && [ "$R" = yes ] && [ "$C" = yes ]; then
	pass "贴左边缘 3px / 盒心 / 贴右边缘 3px 都命中它自己（留白也算窗口）"
else
	fail "盒子里有死区：左 3px=$L 盒心=$C 右 3px=$R（期望都是 yes）"
fi
# 真动作：按在**左留白**上拖 —— 以前这一步抓不住窗口（拖动完全不发生）
CX=$((X + 6)); CY=$((Y + H / 2))
act "pointer-motion $CX $CY" 0.25
act "pointer-button left press mod" 0.25
act "pointer-motion $((CX - 100)) $CY" 0.45
read -r NX NY NW NH <<< "$(box)"
act "pointer-button left release mod" 0.5
if [ "$NX" -lt "$((X - 50))" ]; then
	pass "按在左留白（盒 x+6）上能拖动：盒子跟手 $X → $NX"
else
	fail "按在左留白上拖不动：盒子 $X,$Y → $NX,$NY（期望 x 减 ≈100）"
fi

mark "S3 自由小窗能拖出屏边：屏幕左右那两片区域不再卡住它"
# 用户：「小窗在屏幕上拖拽的时候，左右屏幕区域会把小窗卡住不让过去」+「能拖出屏幕
# 边缘（允许一半在屏外）」。以前 FREE 的锚点夹在 0..1、resolve 的兜底又把盒子夹回
# 工作区 ⇒ 盒子左边缘最多停在 work.x + span，离屏边还差一整个盒子宽（实测光标顶到
# 1919，盒子还卡在 1151 = 1908 - 763）。现在同一块屏内拖动允许**出屏半个盒子**，
# 跨屏改归属那支照旧贴新屏边缘（上一轮用户要的「移动到临近边」）。
set_state free; sleep 1.2
read -r X Y W H <<< "$(box)"
echo "  自由小窗 $X,$Y ${W}x${H}（屏 $(winf w1 out)）；往右拖到光标能到的最右边 2559"
act "pointer-motion $((X + W / 2)) $((Y + H / 2))" 0.25
act "pointer-button left press mod" 0.25
act "pointer-motion 2559 $((Y + H / 2))" 0.4
read -r NX NY NW NH <<< "$(box)"
act "pointer-button left release mod" 0.5
if [ "$NX" -gt "$X" ] && [ $((NX + NW)) -gt 2550 ]; then
	pass "拖到屏右边：盒子跟到 $NX，右边缘 $((NX + NW)) 越过工作区右边界 2550（出屏 $((NX + NW - 2550)) px ≈ 半个盒子）"
else
	fail "还被卡在屏里：盒子 $X → $NX，右边缘 $((NX + NW))（工作区右边界 2550，期望越过它）"
fi

mark "S3 图标贴边裁剪：相邻屏上「本该溢出」的条带非背景像素 = 0"
unmini; sleep 0.8
# 先把窗口摆回屏 1：图标默认贴**右边**，屏 1 的右边外侧才是屏 2（spill 才落得上屏）
./build/vidowmsg move-window-to-output HEADLESS-1 --id "$ID" >/dev/null 2>&1
sleep 0.6
set_state icon; sleep 1.0
read -r IX IY IW IH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
OUT=$(winf w1 out)
echo "  图标 $IX,$IY ${IW}x${IH}（屏 $OUT，mini_icon_size=64、overflow=0.4、贴右边）"
[ "$IW" = "64" ] && [ "$IH" = "64" ] && pass "图标盒 = 64x64（mini_icon_size）" ||
	fail "图标盒不是 64x64：${IW}x${IH}"
if [ "$OUT" = "HEADLESS-1" ] && [ $((IX + IW)) -gt 1280 ]; then
	SPILL=$((IX + IW - 1280))
	pass "图标溢出右边缘 $SPILL px（贴右边 + overflow）"
	# 屏 1 自己的那一块：有内容（对照，证明截图不是空的）
	VIS=$((1280 - IX))
	if shot "$RUNDIR/icon-on.ppm" "$IX" "$IY" "$VIS" "$IH"; then
		N=$(nonbg "$RUNDIR/icon-on.ppm")
		[ "$N" -gt 500 ] && pass "屏 1 上图标那一块有内容（非背景像素 $N，对照）" ||
			fail "屏 1 上图标那块是空的（非背景像素 $N）"
	fi
	# 屏 2 上「本该溢出」的那一条：必须是干净的背景
	if shot "$RUNDIR/icon-spill.ppm" 1280 "$IY" "$SPILL" "$IH"; then
		N=$(nonbg "$RUNDIR/icon-spill.ppm")
		[ "$N" = "0" ] && pass "屏 2 上溢出的 ${SPILL}x${IH} 条带非背景像素 = 0（裁到自己那块屏）" ||
			fail "图标漏到相邻屏上了：非背景像素 $N"
	fi
else
	fail "图标没有贴右边缘溢出（输出 $OUT，盒 ${IX}+${IW}），裁剪这条验不了"
fi

# ================================================================ S3b：mini_aspect
mark "S3b mini_aspect 三值：全局 free / lock / auto + 运行时键位翻转"
aspect_eff() { since "$1" | grep -ao '宽高比 有效值=[a-z]*' | tail -1 | cut -d= -f2; }
# 改宽拖动（Mod+右键 = interactive-resize，宽度驱动）：返回「宽 x 高」。
# 锁比例那条路 h = w / ratio；不锁那条路 h = 原高 + dy（这里 dy = 0）。
# 注意：**进小窗时**盒子仍按 mini_free_w/h 摆（锁比例管的是拖拽改大小那条路），
# 所以判据是「拖完 h 跟不跟 w 走」，不是「进小窗时盒子就是内容比例」。
resize_w() {  # $1 = 往右拖多少像素
	local x y w h
	read -r x y w h <<< "$(echo "$(geom)" | tr 'x,' ' ')"
	act "pointer-motion $((x + w / 2)) $((y + h / 2))" 0.2
	act "pointer-button right press mod" 0.2
	act "pointer-motion $((x + w / 2 + $1)) $((y + h / 2))" 0.45
	act "pointer-button right release mod" 0.5
	echo "$(winf w1 w) $(winf w1 h)"
}
free_box() { # 从列里重新进自由小窗，打印「宽 高」
	unmini; sleep 0.7
	set_state free; sleep 1.0
	echo "$(winf w1 w) $(winf w1 h)"
}
# 内容比例（surface 逻辑尺寸 = 原生），锁比例时该是它
RATIO=$(python3 -c "print($SW1 / $SH1)")

read -r _ _ <<< "$(free_box)"
./build/vidowmsg set-config mini_aspect free >/dev/null; sleep 0.3
MARK=$(logmark); unmini; sleep 0.7; set_state free; sleep 1.0
read -r W0 H0 <<< "$(echo "$(winf w1 w) $(winf w1 h)")"
E=$(aspect_eff "$MARK")
[ "$E" = "free" ] && pass "全局 free：有效值 = free（日志点名）" || fail "有效值不是 free：$E"
[ "$W0" = "504" ] && [ "$H0" = "315" ] &&
	pass "全局 free：进态盒子 = mini_free_w/h 的 40%×45%（504x315）" ||
	fail "全局 free 的进态盒子不对：${W0}x${H0}"
read -r W1 H1 <<< "$(resize_w 100)"
[ "$W1" -ge 590 ] && [ "$H1" = "$H0" ] &&
	pass "全局 free：改宽 504 → $W1，高不动（$H1，不锁比例）" ||
	fail "free 态改宽动了高：${W0}x${H0} → ${W1}x${H1}"

read -r W0 H0 <<< "$(free_box)"
./build/vidowmsg set-config mini_aspect lock >/dev/null; sleep 0.3
MARK=$(logmark); read -r W0 H0 <<< "$(free_box)"
# 锁比例锁的是**客户端当前的内容比例**。FREE 里客户端跟着盒子 configure，表面比例
# 会随盒子变 —— 期望值必须在这一趟小窗里现量一次，不能沿用 S3 那一趟的旧值。
RATIO=$(python3 -c "print($(winf w1 sw) / $(winf w1 sh))")
E=$(aspect_eff "$MARK")
[ "$E" = "lock" ] && pass "全局 lock：有效值 = lock" || fail "有效值不是 lock：$E"
read -r W1 H1 <<< "$(resize_w -100)"
WANT=$(python3 -c "print(round($W1 / $RATIO))")
[ "$W1" -le 560 ] && [ "$H1" -ge $((WANT - 3)) ] && [ "$H1" -le $((WANT + 3)) ] &&
	pass "全局 lock：改窄 $W0 → $W1，高跟着走 → $H1（= 宽 / 内容比例 ≈$WANT）" ||
	fail "锁比例没生效：${W0}x${H0} → ${W1}x${H1}（期望高 ≈$WANT）"

./build/vidowmsg set-config mini_aspect auto >/dev/null; sleep 0.3
MARK=$(logmark); read -r W0 H0 <<< "$(free_box)"
E=$(aspect_eff "$MARK")
[ "$E" = "auto" ] && pass "改回 auto：有效值 = auto（GTK 客户端没有自动信号）" ||
	fail "有效值不是 auto：$E"
read -r W1 H1 <<< "$(resize_w -100)"
[ "$W1" -le 560 ] && [ "$H1" = "$H0" ] &&
	pass "auto（没有自动信号）：改窄 $W0 → $W1，高不动 —— 不锁" ||
	fail "auto 下改窄动了高：${W0}x${H0} → ${W1}x${H1}"

# 键位（运行时）压过全局：全局 lock 时翻一次 → 有效值 free、拖拽不再锁
./build/vidowmsg set-config mini_aspect lock >/dev/null; sleep 0.3
MARK=$(logmark); read -r W0 H0 <<< "$(free_box)"
./build/vidowmsg focus-window --id "$ID" >/dev/null 2>&1; sleep 0.3
./build/vidowmsg action toggle-mini-aspect >/dev/null 2>&1; sleep 0.4
FLIP=$(since "$MARK" | grep -a '键位翻转宽高比' | tail -1)
echo "  ${FLIP:-(没有翻转日志)}"
[ -n "$FLIP" ] && pass "toggle-mini-aspect 生效（运行时翻转，不写回配置）" ||
	fail "键位翻转没生效（$(since "$MARK" | grep -a 'toggle-mini-aspect' | tail -1)）"
echo "$FLIP" | grep -aq '→ free' && pass "翻转方向对：lock → free" || fail "翻转方向不对"
read -r W1 H1 <<< "$(resize_w -100)"
[ "$W1" -le 560 ] && [ "$H1" = "$H0" ] &&
	pass "键位翻转压过全局 lock：改窄 $W0 → $W1，高不动" ||
	fail "键位没压过全局：${W0}x${H0} → ${W1}x${H1}"
echo "  （未测：X11 PAspect 那两条 —— 需要一个能设 aspect hints 的 X11 客户端，见文件头）"

# ================================================================ S4：落点区
mark "S4 落点区：拖进右上角松手 = 迷你；拖到别处不误触"
unmini; sleep 0.6
./build/vidowmsg set-config mini_aspect auto >/dev/null; sleep 0.2
if [ "$(winf w1 float)" != "True" ]; then
	./build/vidowmsg toggle-floating --id "$ID" >/dev/null; sleep 0.5
fi
read -r X0 Y0 W0 H0 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
echo "  浮窗 $X0,$Y0 ${W0}x${H0}；落点区 = 右上角 15%×20%（1081,10 189x140）"
MARK=$(logmark)
act "pointer-motion $((X0 + W0 / 2)) $((Y0 + H0 / 2))" 0.2
act "pointer-button left press mod" 0.2
act "pointer-motion 1100 60" 0.25
act "pointer-motion 1175 80" 0.25
if since "$MARK" | grep -aq 'mini: 落点区亮'; then
	pass "光标进落点区 → 落点区亮了（日志）"
else
	fail "落点区没亮（$(since "$MARK" | grep -a 'mini:' | tail -1)）"
fi
act "pointer-button left release mod" 1.2
if since "$MARK" | grep -aq '拖拽落进落点区'; then
	pass "在落点区松手 → 走的是落点区那条路（日志）"
else
	fail "松手没走落点区"
fi
[ "$(winf w1 mini)" = "mini" ] && pass "松手后形态 = mini（拖到右上角 = 迷你小窗）" ||
	fail "落点区松手没变成迷你：$(winf w1 mini)"
# 负例：拖到屏中间松手 —— 不许误触
unmini; sleep 0.8
[ "$(winf w1 float)" != "True" ] && { ./build/vidowmsg toggle-floating --id "$ID" >/dev/null; sleep 0.5; }
read -r X0 Y0 W0 H0 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
MARK=$(logmark)
act "pointer-motion $((X0 + W0 / 2)) $((Y0 + H0 / 2))" 0.2
act "pointer-button left press mod" 0.2
act "pointer-motion 640 400" 0.25
act "pointer-motion 660 420" 0.25
act "pointer-button left release mod" 1.0
[ "$(winf w1 mini)" = "none" ] && pass "拖到屏中间松手：形态不变（没有误触成小窗）" ||
	fail "区外松手被误判成小窗：$(winf w1 mini)"
since "$MARK" | grep -aq '落点区亮' && fail "光标不在落点区里，落点区却亮了" ||
	pass "光标不在落点区里 → 落点区没亮（只在光标底下那块屏亮）"

# ================================================================ S4：内边缘 / 跨屏边
mark "S4 内边缘 → icon / 跨屏边 → 换屏保持形态"
unmini; sleep 0.8
set_state mini; sleep 1.0
read -r MX MY MW MH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
MARK=$(logmark)
act "pointer-motion $((MX + MW / 2)) $((MY + MH / 2))" 0.2
act "pointer-button left press" 0.1
act "pointer-motion $((MX + MW / 2 - 60)) $((MY + MH / 2))" 0.15
act "pointer-motion 3 $((MY + MH / 2))" 0.2
act "pointer-button left release" 1.2
LINE=$(since "$MARK" | grep -a '拖到内边缘' | tail -1)
echo "  ${LINE:-(没有内边缘日志)}"
[ "$(winf w1 mini)" = "icon" ] && pass "慢慢拖到左边（外侧没有相邻屏）松手 → 收成图标" ||
	fail "拖到内边缘没收成图标：$(winf w1 mini)"
[ -n "$LINE" ] && pass "方向判定是「左」（日志点名）" || fail "内边缘日志没有方向"

set_state mini; sleep 1.0
read -r MX MY MW MH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
OUT0=$(winf w1 out); MARK=$(logmark)
act "pointer-motion $((MX + MW / 2)) $((MY + MH / 2))" 0.2
act "pointer-button left press" 0.1
act "pointer-motion $((MX + MW / 2 + 100)) $((MY + MH / 2))" 0.2
act "pointer-motion 1500 $((MY + MH / 2))" 0.25
act "pointer-button left release" 1.2
OUT1=$(winf w1 out)
LINE=$(since "$MARK" | grep -a '挪到' | tail -1)
echo "  ${LINE:-(没有换屏日志)}"
[ "$OUT1" != "$OUT0" ] && pass "拖过屏边 → 换屏（$OUT0 → $OUT1）" || fail "没换屏（还在 $OUT0）"
[ "$(winf w1 mini)" = "mini" ] && pass "换屏之后形态保持 mini" || fail "换屏把形态改了：$(winf w1 mini)"

# ================================================================ S4：滚轮 / 工具条
mark "S4 滚轮降档：一次一格，到底停住"
# 挪回屏 1（后面工具条 / 甩出都在屏 1 上算）
./build/vidowmsg move-window-to-output HEADLESS-1 --id "$ID" >/dev/null 2>&1
./build/vidowmsg set-window-position 900 40 --id "$ID" >/dev/null 2>&1
sleep 0.6
set_state free; sleep 1.0
scroll_on() {  # $1 = 滚轮方向（1 = 下滚降一档，-1 = 上滚升一档）
	# 每滚一下都要重新把指针挪到**当前**那块盒子上：降档会让窗口换成更小的盒子
	# （icon 更是贴到边角），指针留在旧中心就等于滚在空气上。
	local x y w h
	read -r x y w h <<< "$(echo "$(geom)" | tr 'x,' ' ')"
	act "pointer-motion $((x + w / 2)) $((y + h / 2))" 0.2
	./build/vidowmsg pointer-scroll 0 "$1" >/dev/null 2>&1
	sleep 0.9
}
read -r RX RY RW RH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
echo "  自由小窗 $((RX + RW / 2)),$((RY + RH / 2))（$RX,$RY ${RW}x${RH}，输出 $(winf w1 out)）"
scroll_on 1
[ "$(winf w1 mini)" = "mini" ] && pass "自由小窗下滚一档 → mini" ||
	fail "下滚降档没生效：$(winf w1 mini)"
scroll_on 1
[ "$(winf w1 mini)" = "icon" ] && pass "再下滚一档 → icon" || fail "第二档降档没生效"
scroll_on 1
[ "$(winf w1 mini)" = "icon" ] && pass "图标态（到底）再下滚 → 停住，不循环" ||
	fail "到底还降档 / 循环了：$(winf w1 mini)"
scroll_on -1
[ "$(winf w1 mini)" = "mini" ] && pass "上滚升一档 → mini" || fail "上滚升档没生效"

# 前置：把迷你盒挪回工作区里。上面的判据（迷你态 / 自由态能拖出屏边）会把它留在
# 屏边半挂的位置 —— 那是**合法**位置，但工具条摆位会夹回工作区
# （mini_toolbar_layout），下面那条「工具条跟小窗居中」就没法断言了。
./build/vidowmsg move-window-to-output HEADLESS-2 --id "$(wid w1)" >/dev/null 2>&1
sleep 0.8
set_state mini; sleep 0.6
read -r RX0 RY0 RW0 RH0 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
echo "  工具条前置：迷你盒 $RX0,$RY0 ${RW0}x${RH0}（工作区 1290..2550）"

mark "S4 工具条：悬停露出 + 点「还原成列」"
# 悬停 **迷你** 小窗（不是图标）：图标悬停 250ms 会出预览，预览会吞点击 ——
# 那条路走的是「预览只读但吞点击」，不是工具条这条。
set_state mini; sleep 1.0
read -r IX IY IW IH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
MARK=$(logmark)
act "pointer-motion $((IX + IW / 2)) $((IY + IH / 2))" 0.5
TL=$(since "$MARK" | grep -a 'mini toolbar: w1 露出' | tail -1)
echo "  ${TL:-(工具条没露出)}"
if [ -n "$TL" ]; then
	pass "悬停迷你小窗 → 工具条露出"
	# ---- 工具条**有内容**（每个按钮上真画了字形）且**横向跟小窗居中对齐** ----
	# 用户报的「没有内容只有四个格子且位置偏左」：以前四个按钮就是四个纯色
	# wlr_scene_rect（实测每个按钮 0/884 个非底色像素 = 空方框），整条 x = 窗口
	# 左边（迷你盒 353 宽 / 工具条 151 宽 → 比窗口中心偏左 101px）。
	TBX=$(echo "$TL" | sed -n 's/.*整块 \([0-9]*\),\([0-9]*\) \([0-9]*\)x\([0-9]*\).*/\1/p')
	TBW=$(echo "$TL" | sed -n 's/.*整块 \([0-9]*\),\([0-9]*\) \([0-9]*\)x\([0-9]*\).*/\3/p')
	if [ -n "$TBX" ] && [ -n "$TBW" ]; then
		DX=$((TBX + TBW / 2 - (IX + IW / 2)))
		echo "  工具条 $TBX + $TBW/2 中心 $((TBX + TBW / 2))，窗口中心 $((IX + IW / 2))，差 $DX px"
		# 前置：盒子得**整个在工作区里**才谈得上居中 —— 工具条摆位会夹回工作区
		# （mini_toolbar_layout），盒子半挂屏外时它当然不居中，那是正确行为。
		# 盒子现在可以合法地半挂屏外（用户要的「迷你态和自由态一样，能拖出屏边」），
		# 所以这一条按前置判断，不成立就跳过。
		if [ "$IX" -ge 1290 ] && [ $((IX + IW)) -le 2550 ]; then
			[ "${DX#-}" -le 2 ] &&
				pass "工具条横向跟小窗居中（差 $DX px ≤ 2）" ||
				fail "工具条没跟小窗居中：偏 $DX px（窗口 $IX,$IY ${IW}x${IH}）"
		else
			echo "  跳过居中判据：盒子半挂屏外（$IX..$((IX + IW))，工作区 1290..2550）"
		fi
		BAD=0
		GBOX=""
		for grp in "放大" "收图标" "还原成列" "关闭"; do
			BX=$(echo "$TL" | sed -n "s/.*$grp \([0-9]*\),\([0-9]*\).*/\1/p")
			BY=$(echo "$TL" | sed -n "s/.*$grp \([0-9]*\),\([0-9]*\).*/\2/p")
			[ -z "$BX" ] && continue
			if shot "$RUNDIR/tb-$grp.ppm" "$BX" "$BY" 34 26; then
				N=$(inkpix "$RUNDIR/tb-$grp.ppm")
				GBOX="$GBOX $grp=$N"
				[ "${N%%/*}" -ge 40 ] || BAD=$((BAD + 1))
			fi
		done
		echo "  四个按钮上的字形像素（非底色/整块）$GBOX"
		[ "$BAD" = "0" ] &&
			pass "四个按钮上都有字形（每个 ≥ 40 个字形像素 —— 不是四个空格子）" ||
			fail "有 $BAD 个按钮是空的（没有字形）"
	fi
	# 日志里四个按钮的左上角：放大 / 收图标 / 还原成列 / 关闭（每个 34x26）
	BX=$(echo "$TL" | sed -n 's/.*还原成列 \([0-9]*\),\([0-9]*\).*/\1/p')
	BY=$(echo "$TL" | sed -n 's/.*还原成列 \([0-9]*\),\([0-9]*\).*/\2/p')
	CCX=$((BX + 17)); CCY=$((BY + 13))
	# 走**真实路径**：先经过工具条与窗口之间那条 4px 缝，再到按钮上。一步跳到按钮
	# 上测不出「缝里被收掉」那个 bug（用户报的「整个工具条都不能交互」）。修法见
	# vidow_mini_toolbar_near()。
	# 缝在哪取决于工具条摆上还是摆下（摆上方放不下时会翻到窗口下面，见
	# mini_toolbar_layout），所以按日志里工具条实际的 y 取。
	TBY=$(echo "$TL" | sed -n 's/.*整块 [0-9]*,\([0-9]*\) [0-9]*x[0-9]*.*/\1/p')
	if [ -n "$TBY" ] && [ "$TBY" -gt "$IY" ]; then
		GAPY=$((IY + IH + 2))
	else
		GAPY=$((IY - 2))
	fi
	act "pointer-motion $CCX $GAPY" 0.3
	act "pointer-motion $CCX $CCY" 0.25
	act "pointer-button left press" 0.5
	act "pointer-button left release" 0.8
	BTN=$(since "$MARK" | grep -a '点了按钮' | tail -1)
	echo "  ${BTN:-(没有点按钮的日志)}"
	[ -n "$BTN" ] && pass "点中按钮（日志：「点了按钮 2（现在 还原成列）」）" ||
		fail "按钮没点中（工具条 $(echo "$BX,$BY")，点的是 $CCX,$CCY）"
	[ "$(winf w1 mini)" = "none" ] && pass "「还原成列」按钮 → 形态回到 none" ||
		fail "按钮点了但形态没还原：$(winf w1 mini)"
else
	fail "工具条没露出来"
fi

# ================================================================ S4：自由小窗拖拽
mark "S4 自由小窗拖拽：Mod+左键 interactive-move（跟手 + 尺寸不许跳回原生）"
# 用户原话：「自由小窗的时候没有拖拽的方式……我试图用 win+鼠标 拖拽自由小窗，
# 此时小窗的大小会变成原始大小且拖不动」。两个现象、两个根因：
#   ① 拖不动：float.c 的 vidow_float_grab_update() 只写 float_geom.x/y，而小窗的
#      位置语义是比例锚点（vidow_mini_resolve_geom 是唯一换算点）—— 下一次 arrange
#      按锚点把位置算回来，整段拖拽盒子的 x/y 逐字段不变（实测 1668,203 → 1668,203，
#      期望 1868,343）；
#   ② 尺寸跳回原生：拖动会起果冻（jelly），而 jelly 的落点换算 `dst = buf/fx->scale`
#      里 fx->scale 已经含了 contain 系数（view_anim_bake 传的是 view_scale*fx_ov），
#      被除了第二遍 —— 内容的屏幕脚印从 272x304 涨到 641x507（≈ 原生 665x740）。
unmini; sleep 0.7
# 挪到有壁纸的那块屏（wall.py 只盖它所在那块屏，非背景像素的判据要它当底）。
./build/vidowmsg move-window-to-output HEADLESS-2 --id "$ID" >/dev/null 2>&1
sleep 0.6
[ "$(winf w1 float)" != "True" ] &&
	{ ./build/vidowmsg toggle-floating --id "$ID" >/dev/null; sleep 0.6; }
set_state free; sleep 1.2
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
SW=$(winf w1 sw); SH=$(winf w1 sh)
echo "  自由小窗 $X,$Y ${W}x${H}（原生 ${SW}x${SH}，屏 $(winf w1 out)）；Mod+左键往左下拖 -60,+40"
FB="0 0"
shot "$RUNDIR/free-before.ppm" "$X" "$Y" "$W" "$H" &&
	FB=$(nonbgbox "$RUNDIR/free-before.ppm")
echo "  拖前盒内内容脚印：$FB"
CX=$((X + W / 2)); CY=$((Y + H / 2))
MARK=$(logmark)
act "pointer-motion $CX $CY" 0.25
act "pointer-button left press mod" 0.25
act "pointer-motion $((CX - 30)) $((CY + 20))" 0.3
act "pointer-motion $((CX - 60)) $((CY + 40))" 0.3
read -r MX MY MW MH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
echo "  拖到一半：盒 = $MX,$MY ${MW}x${MH}（期望 ≈ $((X - 60)),$((Y + 40))）"
# 拖拽中：盒里的内容尺寸**不许变**（果冻那条路会把内容放大回原生大小）
if shot "$RUNDIR/free-mid.ppm" "$MX" "$MY" "$MW" "$MH"; then
	FM=$(nonbgbox "$RUNDIR/free-mid.ppm")
	echo "  拖拽中盒内内容脚印：$FM（拖前 $FB）"
	python3 - "$FB" "$FM" <<'PY'
import sys
f = [int(v) for v in sys.argv[1].split()]
m = [int(v) for v in sys.argv[2].split()]
d = max(abs(m[0] - f[0]), abs(m[1] - f[1]))
print("  PASS  拖拽中内容尺寸不变（%dx%d → %dx%d，差 %d px ≤ 10）" %
      (f[0], f[1], m[0], m[1], d) if d <= 10 else
      "  FAIL  一拖内容就变大了：%dx%d → %dx%d（差 %d px）" %
      (f[0], f[1], m[0], m[1], d))
sys.exit(0 if d <= 10 else 1)
PY
	[ $? -eq 0 ] || FAILED=1
else
	fail "拖拽中盒内截图失败"
fi
act "pointer-button left release mod" 1.2
read -r X2 Y2 W2 H2 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
echo "  松手后：盒 = $X2,$Y2 ${W2}x${H2}"
DX=$((X2 - X + 60)); DY=$((Y2 - Y - 40))
[ "${DX#-}" -le 4 ] && [ "${DY#-}" -le 4 ] &&
	pass "盒子跟着光标走了（$X,$Y → $X2,$Y2 = 位移 +$((X2 - X)),+$((Y2 - Y))）" ||
	fail "拖了没动 / 动得不对：$X,$Y → $X2,$Y2（期望 +(-60),+(40)，差 $DX,$DY）"
[ "$W2" = "$W" ] && [ "$H2" = "$H" ] &&
	pass "拖完盒子尺寸没变（${W2}x${H2} = 拖前的 ${W}x${H}）" ||
	fail "拖完尺寸变了：${W}x${H} → ${W2}x${H2}"
[ "$(winf w1 mini)" = "free" ] && pass "拖完形态仍是 free" ||
	fail "拖完形态变了：$(winf w1 mini)"
# 收尾：挪回屏 1（下面 S5 甩出那条判据的前提是「左边外侧没有相邻输出」= 屏 1）。
./build/vidowmsg move-window-to-output HEADLESS-1 --id "$ID" >/dev/null 2>&1
sleep 0.6

# ================================================================ S5：出帧分叉
mark "S5 force_render 关：图标与预览都静止、客户端 0 新帧"
unmini; sleep 0.6
set_state icon; sleep 1.0
read -r IX IY IW IH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
OUTX=0; [ "$(winf w1 out)" = "HEADLESS-2" ] && OUTX=1280
VIS=$((OUTX + 1280 - IX)); [ "$VIS" -gt "$IW" ] && VIS=$IW
ICONBOX="$IX $IY $VIS $IH"
CX=$((IX + IW / 2)); CY=$((IY + IH / 2))
if shot "$RUNDIR/icon-probe.ppm" $ICONBOX; then
	N=$(nonbg "$RUNDIR/icon-probe.ppm")
	[ "$N" -gt 200 ] && pass "截图区域里有图标内容（非背景像素 $N —— 后面两条静止/在动的判据才有意义）" ||
		fail "截图区域是空的（非背景 $N），后面「两帧一样」会假通过"
fi
F1=$(frame); sleep 0.8; F2=$(frame)
if [ "$((F2 - F1))" -le 1 ]; then
	pass "图标态客户端停帧（0.8s 内 $((F2 - F1)) 帧）"
else
	fail "图标态客户端还在出帧（$((F2 - F1)) 帧）"
fi
if shot "$RUNDIR/icon-off-1.ppm" $ICONBOX; then
	sleep 0.35
	shot "$RUNDIR/icon-off-2.ppm" $ICONBOX
	if same "$RUNDIR/icon-off-1.ppm" "$RUNDIR/icon-off-2.ppm"; then
		pass "图标两次截图逐像素相同（静止的最后一帧）"
	else
		fail "图标两帧不同（差 $(diffpct "$RUNDIR/icon-off-1.ppm" "$RUNDIR/icon-off-2.ppm")%）"
	fi
fi
MARK=$(logmark)
act "pointer-motion $CX $CY" 0.5
PLINE=$(since "$MARK" | grep -a '的预览在' | tail -1)
echo "  ${PLINE:-(预览没出来)}"
if [ -n "$PLINE" ]; then
	pass "悬停 250ms → 预览出来"
	read -r PX PY PW PH <<< "$(echo "$PLINE" |
		sed -n 's/.*的预览在 \([0-9-]*\),\([0-9-]*\) \([0-9]*\)x\([0-9]*\).*/\1 \2 \3 \4/p')"
	if [ "$PX" -lt "$IX" ] && [ $((PX + PW)) -le "$((IX + 2))" ]; then
		pass "贴右边的图标 → 预览在它左边（朝屏内展开）：$PX..$((PX + PW)) < $IX"
	else
		fail "预览没朝屏内展开：$PX..$((PX + PW))，图标从 $IX 起"
	fi
	echo "$PLINE" | grep -aq '快照' && pass "预览走快照那条路（force_render 关）" ||
		fail "预览不是快照"
	F3=$(frame); sleep 0.7; F4=$(frame)
	[ "$((F4 - F3))" -le 1 ] && pass "悬停预览期间客户端仍然停帧（$((F4 - F3)) 帧）" ||
		fail "悬停时客户端又在出帧（$((F4 - F3)) 帧）"
	sleep 0.3
	if shot "$RUNDIR/prev-off-1.ppm" "$PX" "$PY" "$PW" "$PH"; then
		sleep 0.35
		shot "$RUNDIR/prev-off-2.ppm" "$PX" "$PY" "$PW" "$PH"
		if same "$RUNDIR/icon-off-1.ppm" "$RUNDIR/prev-off-1.ppm" >/dev/null 2>&1; then
			: # 不同区域，不比对
		fi
		if same "$RUNDIR/prev-off-1.ppm" "$RUNDIR/prev-off-2.ppm"; then
			pass "预览两次截图逐像素相同（图标静止、预览也静止）"
		else
			fail "预览两帧不同（差 $(diffpct "$RUNDIR/prev-off-1.ppm" "$RUNDIR/prev-off-2.ppm")%）"
		fi
	fi
else
	fail "预览没出来"
fi

mark "S5 force_render 开：图标与预览都活"
./build/vidowmsg set-window-force-render --on true --id "$ID" >/dev/null
# 快照态下客户端已经停帧：没有 commit 来触发重新摆位，指针一动是那个触发点
act "pointer-motion $((CX + 1)) $CY" 0.15
act "pointer-motion $CX $CY" 0.15
sleep 1.0
F5=$(frame); sleep 0.8; F6=$(frame)
if [ "$((F6 - F5))" -ge 5 ]; then
	pass "图标态客户端恢复出帧（0.8s 内 $((F6 - F5)) 帧）"
else
	fail "客户端没恢复出帧（$((F6 - F5)) 帧）"
fi
if shot "$RUNDIR/icon-on-1.ppm" $ICONBOX; then
	sleep 0.35
	shot "$RUNDIR/icon-on-2.ppm" $ICONBOX
	D=$(diffpct "$RUNDIR/icon-on-1.ppm" "$RUNDIR/icon-on-2.ppm")
	echo "  图标那一条截图差 $D%（图标态被裁剪 bug 缩到一小条，像素差量不做判据 —— 下面用预览那份判）"
fi
# 预览那一份也跟着活（预览是同一张表面的第二份镜面，范围大、最看得出在动）
act "pointer-motion 640 400" 0.5
MARK=$(logmark)
act "pointer-motion $CX $CY" 0.6
PLINE2=$(since "$MARK" | grep -a '的预览在' | tail -1)
[ -n "$PLINE2" ] || PLINE2="$PLINE"
read -r PX PY PW PH <<< "$(echo "$PLINE2" |
	sed -n 's/.*的预览在 \([0-9-]*\),\([0-9-]*\) \([0-9]*\)x\([0-9]*\).*/\1 \2 \3 \4/p')"
echo "  ${PLINE2:-(预览没出来)}"
if [ -n "$PX" ] && shot "$RUNDIR/prev-on-1.ppm" "$PX" "$PY" "$PW" "$PH"; then
	echo "$PLINE2" | grep -aq '活镜面' && pass "预览改走活镜面那条路（force_render 开）" ||
		fail "预览还是快照"
	sleep 0.35
	shot "$RUNDIR/prev-on-2.ppm" "$PX" "$PY" "$PW" "$PH"
	echo "  预览两次截图差 $(diffpct "$RUNDIR/prev-on-1.ppm" "$RUNDIR/prev-on-2.ppm")%（活的那条判据用上面的客户端帧数：0.8s 内 $((${F6:-0} - ${F5:-0})) 帧）"
fi
./build/vidowmsg set-window-force-render --on false --id "$ID" >/dev/null
sleep 0.6
F7=$(frame); sleep 0.8; F8=$(frame)
[ "$((F8 - F7))" -le 1 ] && pass "关掉 force_render（不碰指针）→ 客户端又停帧（$((F8 - F7)) 帧）" ||
	fail "关掉之后客户端还在出帧（$((F8 - F7)) 帧）"
if shot "$RUNDIR/icon-off3-1.ppm" $ICONBOX; then
	sleep 0.35
	shot "$RUNDIR/icon-off3-2.ppm" $ICONBOX
	D=$(diffpct "$RUNDIR/icon-off3-1.ppm" "$RUNDIR/icon-off3-2.ppm")
	[ "${D%%.*}" -lt 5 ] && pass "图标回到静止（两帧差 $D%）" ||
		fail "图标还在动（差 $D%）"
fi

# ================================================================ S5：过渡动画
mark "S5 过渡动画：相邻位移 ≤ 屏宽 10%、单调、第一帧在起点"
unmini; sleep 0.8
check_anim() {  # $1 = 名字，$2 = 该趟的「过渡动画」日志行，$3 = 只看这一行之后的采样
	local label="$1" line="$2" from="$3"
	local sx sy ex ey
	sx=$(echo "$line" | sed -n 's/.*起点 \([0-9-]*\),\([0-9-]*\)（.*/\1/p')
	sy=$(echo "$line" | sed -n 's/.*起点 \([0-9-]*\),\([0-9-]*\)（.*/\2/p')
	ex=$(echo "$line" | sed -n 's/.*目标 \([0-9-]*\),\([0-9-]*\) .*/\1/p')
	ey=$(echo "$line" | sed -n 's/.*目标 \([0-9-]*\),\([0-9-]*\) .*/\2/p')
	if [ -z "$sx" ] || [ -z "$ex" ]; then
		fail "$label：没有「过渡动画」日志（$(echo "$line" | head -c 80)）"
		return
	fi
	echo "  $label：起点 $sx,$sy → 目标 $ex,$ey"
	since "$from" | grep -a 'mini anim: w1' | python3 -c "
import sys, re
sx, sy, ex, ey = $sx, $sy, $ex, $ey
pts = []
for ln in sys.stdin:
    m = re.search(r'画在 (-?\d+),(-?\d+)', ln)
    if m: pts.append((int(m.group(1)), int(m.group(2))))
if len(pts) < 5:
    print('  FAIL  $label：采样点太少（%d 帧）' % len(pts)); sys.exit(1)
maxd = 0; bad = 0; nonmono = 0
dx, dy = ex - sx, ey - sy
for i in range(1, len(pts)):
    px, py = pts[i-1]; cx, cy = pts[i]
    d = ((cx-px)**2 + (cy-py)**2) ** 0.5
    maxd = max(maxd, d)
    if d > 128: bad += 1
    if (cx-px)*dx + (cy-py)*dy < 0: nonmono += 1
first = pts[0]
print('  PASS  $label：相邻位移最大 %.1f px ≤ 屏宽 10%%（128）' % maxd if not bad else
      '  FAIL  $label：有 %d 处相邻位移 > 128px' % bad)
print('  PASS  $label：位置全程单调' if not nonmono else
      '  FAIL  $label：有 %d 处倒退' % nonmono)
ok = abs(first[0]-sx) <= 2 and abs(first[1]-sy) <= 2
print('  PASS  $label：第一帧已在起点（%d,%d）' % first if ok else
      '  FAIL  $label：第一帧不在起点（%d,%d，差 %d,%d）' % (first[0], first[1], first[0]-sx, first[1]-sy))
sys.exit(0 if (not bad and not nonmono and ok) else 1)
"
	[ $? -eq 0 ] || FAILED=1
}
MARK=$(logmark); set_state mini; sleep 1.2
check_anim "none→mini" "$(since "$MARK" | grep -a 'mini: 过渡动画' | tail -1)" "$MARK"
MARK=$(logmark); set_state icon; sleep 1.2
check_anim "mini→icon" "$(since "$MARK" | grep -a 'mini: 过渡动画' | tail -1)" "$MARK"
MARK=$(logmark); set_state free; sleep 1.2
check_anim "icon→free" "$(since "$MARK" | grep -a 'mini: 过渡动画' | tail -1)" "$MARK"
MARK=$(logmark); unmini; sleep 1.2
check_anim "free→none" "$(since "$MARK" | grep -a 'mini: 过渡动画' | tail -1)" "$MARK"
LAST=$(grep -a 'mini anim: w1' "$LOG" | tail -1)
echo "  $LAST"
echo "$LAST" | grep -qE "偏移 0\.0,-?0\.0" && pass "转换结束偏移归零（画在 geom 上）" ||
	fail "最后偏移没归零"
echo "$LAST" | grep -q "缩放 1.000" && pass "转换结束缩放归 1" || fail "最后缩放不是 1"

# ---- 退出小窗那一瞬间：起点必须是「上一帧画在哪」，不许闪原生尺寸 ----
# 用户原话：「退出小窗（还原成列 / 关闭）的一瞬间显示原始大小」。根因：起点是从
# **盒面积**推的 sqrt(area_from/area_to)，只在内容正好填满盒子时才等于屏上真正画的
# contain 系数。判据两条（都只读日志 + geometry，不依赖时序）：
#   ① 起点缩放 ≈ min(盒宽/原生宽, 盒高/原生高)（自由小窗那趟推出来是 0.602、
#      真值是 0.426，差 41%；图标那趟 0.097 vs 0.087）—— **自由小窗例外**：它按
#      盒子尺寸 configure 客户端（不保留 buffer），内容与盒子 1:1，起点缩放 = 1.0、
#      内容原点 = 盒原点（第 2 个参数传 1:1）；
#   ② 起点位置是**内容**原点（盒原点 + contain 居中偏移），不是盒原点。
# ②的容差给 ±16：实现里的居中用的是「可见窗口」几何，测试只能从 surface 尺寸算
# （GTK CSD 那圈留白差 ~9px）。
check_exit_from() {  # $1 = 退出前的形态，$2 = 1:1（内容与盒子 1:1，FREE 那条）
	local st="$1" bx by bw bh sw sh ms cx line sc sx0
	set_state "$st"; sleep 1.3
	read -r bx by bw bh <<< "$(echo "$(geom)" | tr 'x,' ' ')"
	sw=$(winf w1 sw); sh=$(winf w1 sh)
	if [ "${2:-}" = "1:1" ]; then
		ms="1.0000"; cx=0
	else
		read -r ms cx < <(python3 - "$bw" "$bh" "$sw" "$sh" <<'PY'
import sys
bw, bh, sw, sh = (float(v) for v in sys.argv[1:5])
ms = min(bw / sw, bh / sh)
print("%.4f %d" % (ms, round((bw - sw * ms) / 2.0)))
PY
)
	fi
	MARK=$(logmark)
	unmini; sleep 1.3
	line=$(since "$MARK" | grep -a 'mini: 过渡动画' | tail -1)
	sc=$(echo "$line" | sed -n 's/.*缩放 \([0-9.]*\) → 1.*/\1/p')
	sx0=$(echo "$line" | sed -n 's/.*起点 \([0-9-]*\),\([0-9-]*\).*/\1/p')
	echo "  $st→none：盒 $bx,$by ${bw}x${bh} / 原生 ${sw}x${sh}"
	echo "    期望起点缩放 $ms、起点 x ≈ $((bx + cx))；日志：$line"
	python3 - "$st" "$sc" "$ms" "$sx0" "$bx" "$cx" <<'PY'
import sys
st, sc, ms, sx0, bx, cx = sys.argv[1], sys.argv[2], float(sys.argv[3]), \
    sys.argv[4], int(sys.argv[5]), int(sys.argv[6])
try:
    scf = float(sc)
except ValueError:
    print("  FAIL  %s→none：日志里没有「缩放 X → 1」（%s）" % (st, sys.argv[4])); sys.exit(1)
d = abs(scf - ms)
print("  PASS  %s→none：起点缩放 %.4f = 上一帧的 contain 系数（差 %.4f ≤ 0.005）"
      % (st, scf, d) if d <= 0.005 else
      "  FAIL  %s→none：起点缩放 %.4f ≠ 上一帧的 %.4f（差 %.4f）" % (st, scf, ms, d))
try:
    sx = int(sx0)
except ValueError:
    print("  FAIL  %s→none：日志里没有起点坐标" % st); sys.exit(1)
d2 = abs(sx - (bx + cx))
print("  PASS  %s→none：起点在内容原点（x=%d，期望 %d，差 %d ≤ 16）"
      % (st, sx, bx + cx, d2) if d2 <= 16 else
      "  FAIL  %s→none：起点在盒原点不是内容原点（x=%d，期望 %d，差 %d）"
      % (st, sx, bx + cx, d2))
sys.exit(0 if (d <= 0.005 and d2 <= 16) else 1)
PY
	[ $? -eq 0 ] || FAILED=1
}
check_exit_from free 1:1
check_exit_from icon

# ================================================================ S5：甩出
mark "S5 惯性甩出：速度阈值 / 不过冲 / <4px 快甩的逃生口"
unmini; sleep 0.8
set_state mini; sleep 1.0
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
CX=$((X + W / 2)); CY=$((Y + H / 2))
echo "  迷你小窗 $X,$Y ${W}x${H}（屏 1 = 0..1280）"
MARK=$(logmark)
act "pointer-motion $CX $CY" 0.15
act "pointer-button left press" 0.05
act "pointer-motion $((CX - 60)) $CY" 0.01
act "pointer-motion $((CX - 140)) $CY" 0.01
act "pointer-motion $((CX - 240)) $CY" 0.01
act "pointer-motion $((CX - 340)) $CY" 0.005
act "pointer-button left release" 1.2
LINE=$(since "$MARK" | grep -a 'mini fling:' | tail -1)
echo "  $LINE"
[ "$(winf w1 mini)" = "icon" ] && pass "快甩向左（外侧没有屏）→ 收成图标" ||
	fail "甩出没收成图标：$(winf w1 mini)"
echo "$LINE" | grep -q "内边缘" && pass "方向取最近的那条边（左）" || fail "方向判错"
SPEED=$(echo "$LINE" | sed -n 's/.*以 \([0-9]*\) px\/s.*/\1/p')
[ "${SPEED:-0}" -ge 800 ] && pass "松手速度 ${SPEED} px/s ≥ 默认阈值 800" ||
	fail "速度算错了：${SPEED:-?}"
DUR=$(echo "$LINE" | sed -n 's/.*，\([0-9]*\)ms$/\1/p')
if [ "${DUR:-999}" -gt 0 ] && [ "${DUR:-999}" -lt 600 ]; then
	pass "时长由速度推出来：${DUR}ms < 普通 600ms"
else
	fail "时长没跟着速度走：${DUR:-?}"
fi
# 不过冲：这一趟的位置采样不许越过目标边
since "$MARK" | grep -a 'mini anim: w1' | python3 -c '
import sys, re
pts = []
for ln in sys.stdin:
    m = re.search(r"画在 (-?\d+),(-?\d+)", ln)
    if m: pts.append((int(m.group(1)), int(m.group(2))))
if len(pts) < 3:
    print("  FAIL  甩出采样点太少（%d）" % len(pts)); sys.exit(1)
tx = pts[-1][0]; minx = min(p[0] for p in pts)
mono = all(pts[i][0] <= pts[i-1][0] for i in range(1, len(pts)))
print("  PASS  不过冲（采样 %d 帧，目标 x=%d，最小 x=%d）" % (len(pts), tx, minx) if minx >= tx - 2
      else "  FAIL  越过了目标边 %d px" % (tx - minx))
print("  PASS  全程单调向左" if mono else "  FAIL  位置不单调")
sys.exit(0 if (minx >= tx - 2 and mono) else 1)
'
[ $? -eq 0 ] || FAILED=1

set_state mini; sleep 1.0
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
CX=$((X + W / 2)); CY=$((Y + H / 2))
OUT0=$(winf w1 out)
MARK=$(logmark)
act "pointer-motion $CX $CY" 0.15
act "pointer-button left press" 0.05
act "pointer-motion $((CX + 60)) $CY" 0.01
act "pointer-motion $((CX + 140)) $CY" 0.01
act "pointer-motion $((CX + 240)) $CY" 0.01
act "pointer-motion $((CX + 340)) $CY" 0.005
act "pointer-button left release" 1.2
OUT1=$(winf w1 out)
echo "  $(since "$MARK" | grep -a 'mini fling:' | tail -1)"
[ "$OUT1" != "$OUT0" ] && pass "甩向跨屏边 → 换屏（$OUT0 → $OUT1）" || fail "没换屏（还在 $OUT0）"
[ "$(winf w1 mini)" = "mini" ] && pass "换屏之后形态保持 mini" || fail "形态变了：$(winf w1 mini)"

# 4px 逃生口：位移 < mini_drag_threshold_px 但速度够 → 甩出（不是点击放大）
./build/vidowmsg move-window-to-output HEADLESS-1 --id "$ID" >/dev/null 2>&1
sleep 0.5
set_state mini; sleep 1.0
# 阈值降到 30 px/s：注入出来的时间精度做不出「3px 位移 + 800 px/s」，
# 这一条验的是**机制**（速度优先于 4px 位移阈值），不是默认阈值的数值。
# 往**左**甩（屏 1 左边外侧没有屏 = 内边缘）→ 收成图标；往右甩是换屏，形态不变。
./build/vidowmsg set-config mini_fling_speed 30 >/dev/null; sleep 0.3
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
CX=$((X + W / 2)); CY=$((Y + H / 2))
MARK=$(logmark)
act "pointer-motion $CX $CY" 0.15
act "pointer-button left press" 0.05
act "pointer-motion $((CX - 1)) $CY" 0.025
act "pointer-motion $((CX - 2)) $CY" 0.025
act "pointer-motion $((CX - 3)) $CY" 0.025
act "pointer-button left release" 1.2
echo "  $(since "$MARK" | grep -a 'mini fling:' | tail -1)"
[ "$(winf w1 mini)" = "icon" ] &&
	pass "位移 3px（< 4px 阈值）但速度够 → 甩出去收成图标（不是被判成单击放大）" ||
	fail "被判成单击了：mini=$(winf w1 mini)"
./build/vidowmsg set-config mini_fling_speed 800 >/dev/null 2>&1

# 4px 阈值本身：单击（0 位移）放大 / 拖 10px 只是移动
mark "S4 单击 vs 拖拽：4px 累计位移阈值（mini_drag_threshold_px）"
./build/vidowmsg set-config mini_fling_speed 8000 >/dev/null 2>&1
# 先把指针挪到空处停一会儿：上一节刚悬停过，悬停预览（只读但吞点击）可能还挂着，
# 它压在窗口上会把这一下按下吞掉 —— 那不是这条判据要验的东西。
act "pointer-motion 640 690" 0.6
unmini; sleep 0.7
set_state mini; sleep 1.0
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
CX=$((X + W / 2)); CY=$((Y + H / 2))
OUT0=$(winf w1 out)
MARK2=$(logmark)
act "pointer-motion $CX $CY" 0.2
act "pointer-button left press" 0.15
act "pointer-motion $((CX + 3)) $CY" 0.15
act "pointer-motion $((CX + 10)) $((CY + 10))" 0.15
act "pointer-motion $((CX + 30)) $((CY + 30))" 0.2
act "pointer-button left release" 1.0
X2=$(winf w1 x); Y2=$(winf w1 y)
[ "$(winf w1 mini)" = "mini" ] && pass "拖 30px：形态仍是 mini（判成移动，不是放大）" ||
	fail "拖拽被判成单击了：$(winf w1 mini)"
# 注意：迷你盒是**夹在工作区里**的（mini_edge_margin），拖到靠右时 x 顶死不动 ——
# 判据得看 x / y 任一跟着走，只看 x 会漏。
MOVED=0
[ "$X2" -ne "$X" ] && MOVED=$((MOVED + 1))
[ $((Y2 - Y)) -ge 20 ] && MOVED=$((MOVED + 1))
[ "$MOVED" -ge 1 ] && pass "窗口跟着指针挪了（$X,$Y → $X2,$Y2）" ||
	fail "拖了没动：$X,$Y → $X2,$Y2（$(since "$MARK2" | grep -a 'mini preview' | tail -1)）"
[ "$(winf w1 out)" = "$OUT0" ] && pass "还在原来那块屏上（$OUT0）" || fail "没拖出屏却换屏了"

# 单击（位移 0 ≤ 阈值）→ 放大一档
act "pointer-motion 640 690" 0.5
set_state mini; sleep 1.0
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
CX=$((X + W / 2)); CY=$((Y + H / 2))
act "pointer-motion $CX $CY" 0.2
act "pointer-button left press" 0.15
act "pointer-button left release" 1.0
# 这一条是「工具条点过按钮之后」的回归：以前 `server->mini_toolbar_pressed` 会留在
# true（`vidow_mini_toolbar_hide()` 在松手之前就把 mini_toolbar_view 清成 NULL，松手
# 那一支再也进不去、标志没人清），于是「悬停着工具条的迷你窗口」上的下一对按下/松开
# 被陈旧的标志吞掉：形态不变、而且那次按下起的 mini_grab 再没人收尾（紧接着一次普通
# pointer-motion 就能把窗口在没按键的情况下拖走，run4 现场 `place w1 -> 464,506`）。
# 现在松手那一半无条件清标志 + 顺带把还挂着的 grab 交给小窗那条松手收尾（src/input.c）。
[ "$(winf w1 mini)" = "free" ] && pass "单击迷你（0 位移 ≤ 4px）→ 放大成自由小窗（工具条点过按钮之后也一样）" ||
	fail "单击迷你没放大：$(winf w1 mini)"
# 无按键的 motion 不许把窗口拖走（泄漏的 grab 会一直跟着光标）
act "pointer-motion 640 690" 0.5
X3=$(winf w1 x); Y3=$(winf w1 y)
act "pointer-motion 900 400" 0.5
X4=$(winf w1 x); Y4=$(winf w1 y)
[ "$X3,$Y3" = "$X4,$Y4" ] && pass "不带按键的 pointer-motion 不拖窗口（$X3,$Y3 没动 —— 没有泄漏的 grab）" ||
	fail "没按键也把窗口拖走了：$X3,$Y3 → $X4,$Y4（mini_grab 泄漏）"
unmini; sleep 0.7
set_state icon; sleep 1.0
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
CX=$((X + W / 2)); CY=$((Y + H / 2))
act "pointer-motion $CX $CY" 0.15
act "pointer-button left press" 0.15
act "pointer-button left release" 1.0
# ⚠️ 这条现在**已知不过**：图标态是缩放的（contain），裁剪那一步
#    （src/view.c:1484 那个 local 框）没按 hit_scale 换算，于是图标的**命中区**
#    也被挪到了一小条上（wlroots 的 hit test 认 src_box），点在图标正中间命中不到。
#    同一个根因还让图标漏画到隔壁屏（上面 S3 那条）。留着当信号，不给它开后门。
[ "$(winf w1 mini)" = "mini" ] && pass "单击图标（0 位移）→ 放大成迷你" ||
	fail "单击图标没放大：$(winf w1 mini)（图标命中区被裁剪挪走了，见 view.c 的 clip_local）"
./build/vidowmsg set-config mini_fling_speed 800 >/dev/null 2>&1

# ================================================================ 规则入口
mark "S1 规则入口：mini_state / mini_aspect / mini_preserve_buffer"
python3 ./tools/pattern.py rulewin >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.8
if [ "$(winf rulewin mini)" = "icon" ]; then
	pass "规则 mini_state = \"icon\"：窗口一开出来就是图标态"
else
	fail "规则没生效：rulewin 形态 = $(winf rulewin mini)"
fi
RID=$(winf rulewin id)
[ "$(winf rulewin w)" = "64" ] && pass "规则窗口的图标盒也是 64x64" ||
	fail "规则窗口的盒子不对：$(winf rulewin w)x$(winf rulewin h)"
# 全局显式写成 auto（默认值现在是 free，这条判的是「规则 lock 压过全局」——
# 全局值必须写死，不然它跟着默认值走，判据就变成「lock 压过 free」，弱一些）。
./build/vidowmsg set-config mini_aspect auto >/dev/null; sleep 0.3
MARK=$(logmark)
./build/vidowmsg set-mini-state --id "$RID" --state free >/dev/null; sleep 0.8
E=$(since "$MARK" | grep -ao '有效值=[a-z]*' | tail -1 | cut -d= -f2)
[ "$E" = "lock" ] && pass "规则 mini_aspect = \"lock\" 压过全局 auto（有效值 lock）" ||
	fail "规则 mini_aspect 没生效：有效值 $E"
SWS=$(winf rulewin sw); SHS=$(winf rulewin sh)
echo "  规则窗口在列里时 surface = ${SWS}x${SHS}（原生）"
./build/vidowmsg set-mini-state --id "$RID" --state none >/dev/null; sleep 0.4
./build/vidowmsg set-mini-state --id "$RID" --state icon >/dev/null; sleep 0.8
SWA=$(winf rulewin sw)
if [ -n "$SWA" ] && [ "$SWA" -lt "$SWS" ]; then
	pass "规则 mini_preserve_buffer = false：进小窗时客户端**真的被重配小了**（$SWS → $SWA，和默认 true 那条相反）"
else
	fail "preserve_buffer=false 这条没验到：$SWS → $SWA"
fi

# ================================================================ MRU（Alt+Tab）
mark "S5 MRU（Alt+Tab）选中图标态小窗 → 展开成自由小窗并拿到焦点"
# 真走 MRU 提交那条路：`action next-window` 打开切换器（键位动作 'next-window'
# 同一个入口 vidow_mru_open），再从 `vidowmsg mru` 拿卡片矩形点它一下
# （vidow_mru_click → vidow_mru_commit → vidow_focus_view_expand_mini）。
act "pointer-motion 640 400" 0.4
set_state icon; sleep 1.0
# 再起一个**普通**窗口并聚焦它：切换器是按「当前聚焦那块屏的工作区」列清单的，
# 一个都不聚焦时清单可能是空的（那验的就不是「选中迷你态小窗」这条了）。
python3 ./tools/pattern.py mruwin >/dev/null 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 1.8
./build/vidowmsg focus-window --title mruwin >/dev/null 2>&1; sleep 1.0
# 默认 scope=workspace：迷你 / 图标态是**屏级对象**（不属于任何工作区），
# 列不出来 —— 换成 all 才看得到它们（切换器里列着它们正是 8838b19 的语义）。
./build/vidowmsg set-config mru_scope all >/dev/null 2>&1; sleep 0.3
./build/vidowmsg action next-window >/dev/null 2>&1
SHOWING=""
for _ in $(seq 1 20); do
	SHOWING=$(./build/vidowmsg mru | python3 -c 'import sys,json;print(json.load(sys.stdin)["showing"])' 2>/dev/null)
	[ "$SHOWING" = "True" ] && break
	sleep 0.15
done
if [ "$SHOWING" = "True" ]; then
	pass "next-window 打开切换器（mru.showing = true）"
	CARD=$(./build/vidowmsg mru | python3 -c '
import sys, json
d = json.load(sys.stdin)
for e in d["entries"]:
    if e["title"] == "w1":
        print("%d %d" % (e["cx"] + e["cw"] // 2, e["cy"] + e["ch"] // 2))
        break')
	echo "  w1 的卡片中心 = ${CARD:-(清单里没有 w1)}"
	if [ -n "$CARD" ]; then
		read -r CCX CCY <<< "$CARD"
		MARK=$(logmark)
		act "pointer-motion $CCX $CCY" 0.3
		act "pointer-button left press" 0.4
		act "pointer-button left release" 0.6
		PICK=$(since "$MARK" | grep -a 'mru: 点选' | tail -1)
		echo "  ${PICK:-(没有点选日志)}"
		[ -n "$PICK" ] && pass "点中卡片（日志：mru: 点选 …）" || fail "卡片没点中"
		[ "$(winf w1 mini)" = "free" ] &&
			pass "MRU 提交选中图标态小窗 → 展开成自由小窗" ||
			fail "MRU 提交没展开：$(winf w1 mini)"
		[ "$(winf w1 foc)" = "True" ] && pass "展开之后焦点给了它" || fail "展开之后没给焦点"
	else
		echo "  （mru 清单：$(./build/vidowmsg mru | head -c 300)）"
		fail "切换器清单里找不到 w1 的卡片（图标态小窗没进 MRU 清单？）"
	fi
else
	echo "  （mru 状态：$(./build/vidowmsg mru | head -c 300)）"
	fail "切换器没打开（mru.showing = $SHOWING）"
fi

# ================================================================ S7：HiDPI（scale 2）
mark "S7 HiDPI scale 2：输出逻辑尺寸 / 几何按逻辑算 / 跨缩放边界裁剪 / 指针坐标换算"
# 前六节都是 scale 1、两块屏在逻辑 0..1280 / 1280..2560（S1–S5 那堆坐标都写死了）。
# 这一节**不另起一次运行**：同一套 harness、同一份配置、同一个探针，改成运行时用
# IPC 把 HEADLESS-2 的缩放调到 2，再把 HEADLESS-1 摆到它右边 —— 逻辑坐标下就是
# 同一张布局：scale 2 屏占 1280..1920（逻辑 640x360），scale 1 的邻屏从 1920 起、
# 正好贴着它的逻辑右边缘（图标贴右溢出那半块才落得上邻屏）。
# ⚠️ 布局坐标只能这么写死：`vidowmsg outputs` 的 JSON 里**没有**布局坐标字段。
# 两块辅助客户端（rulewin / mruwin）先关掉：邻屏那条像素差分判据要的是「这块屏
# 上除了图标没别人在画」，留着它们只是给差分添噪声。
./build/vidowmsg close-window --title rulewin >/dev/null 2>&1
./build/vidowmsg close-window --title mruwin >/dev/null 2>&1
sleep 0.6
MARK=$(logmark)
./build/vidowmsg output-scale HEADLESS-2 2 >/dev/null 2>&1
sleep 0.8
./build/vidowmsg output-position HEADLESS-2 1280 0 >/dev/null 2>&1
./build/vidowmsg output-position HEADLESS-1 1920 0 >/dev/null 2>&1
sleep 1.2

# ---- 判据 1：scale 生效（物理 1280x720 / 逻辑 640x360）
./build/vidowmsg outputs | python3 -c '
import sys, json
for o in json.load(sys.stdin):
    print("  %s scale=%s 物理=%sx%s 逻辑=%sx%s" % (
        o.get("name"), o.get("scale"), o.get("width"), o.get("height"),
        o.get("logical_width"), o.get("logical_height")))
'
read -r S2 OW OH LW LH <<< "$(./build/vidowmsg outputs | python3 -c '
import sys, json
for o in json.load(sys.stdin):
    if o.get("name") == "HEADLESS-2":
        print(o["scale"], o["width"], o["height"], o["logical_width"],
              o["logical_height"])
        break')"
[ "$S2" = "2" ] && pass "HEADLESS-2 scale = 2（物理 ${OW}x${OH}，逻辑 ${LW}x${LH}）" ||
	fail "HEADLESS-2 scale 不是 2（读到 '$S2'）"
[ "$LW $LH" = "640 360" ] && pass "逻辑尺寸 = 物理 ${OW}x${OH} / scale 2（缩放生效）" ||
	fail "逻辑尺寸不对：${LW}x${LH}（期望 640x360）"
since "$MARK" | grep -aq 'HEADLESS-1: 布局位置 -> 1920,0' &&
	pass "邻屏 HEADLESS-1 摆到布局 1920,0（紧贴 scale 2 屏的逻辑右边缘）" ||
	fail "邻屏没摆到 1920,0：$(since "$MARK" | grep -a '布局位置' | tr '\n' ' ')"

H2X=1280; H2W=640; EDGE=$((H2X + H2W))     # scale 2 屏的逻辑范围 1280..1920
ID=$(mini_id)
unmini; sleep 0.8
./build/vidowmsg move-window-to-output HEADLESS-2 --id "$ID" >/dev/null 2>&1
sleep 1.0
echo "  窗口：$(geom) 输出=$(winf w1 out) surface=$(winf w1 sw)x$(winf w1 sh)"

# ---- 判据 2：几何按**逻辑**工作区算（逻辑工作区 = 640x360 各去 gaps 10 = 620x340）
set_state mini; sleep 1.0
read -r MX MY MW MH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
W2=620; H2=340
EXPW=$((W2 * 28 / 100)); EXPH=$((H2 * 28 / 100))     # mini_free_w/h 默认 28%
echo "  迷你盒（逻辑）$MX,$MY ${MW}x${MH}；逻辑工作区 ${W2}x${H2} 的 28% = ${EXPW}x${EXPH}"
if [ "$MW" -ge $((EXPW - 3)) ] && [ "$MW" -le $((EXPW + 3)) ] &&
   [ "$MH" -ge $((EXPH - 3)) ] && [ "$MH" -le $((EXPH + 3)) ]; then
	pass "迷你盒按**逻辑**工作区算：${MW}x${MH} ≈ ${EXPW}x${EXPH}（物理脚印是它的 2 倍）"
else
	fail "迷你盒尺寸不对：${MW}x${MH}，期望 ≈ ${EXPW}x${EXPH}"
fi
[ "$MX" -ge "$H2X" ] && [ "$MX" -lt "$EDGE" ] && [ "$MY" -ge 0 ] && [ "$MY" -lt 360 ] &&
	pass "迷你盒落在 scale 2 屏的逻辑范围（${H2X}..${EDGE} × 0..360）" ||
	fail "迷你盒在 scale 2 屏逻辑范围外：$MX,$MY（应在 ${H2X}..${EDGE}）"
if shot "$RUNDIR/s7-mini.ppm" "$MX" "$MY" "$MW" "$MH"; then
	SZ=$(python3 -c "from PIL import Image;print('%dx%d' % Image.open('$RUNDIR/s7-mini.ppm').size)")
	NB=$(nonbg "$RUNDIR/s7-mini.ppm"); TOT=$((MW * MH * 4))
	echo "  盒内截图 = $SZ 像素；非壁纸像素 $NB / $TOT"
	[ "$SZ" = "$((MW * 2))x$((MH * 2))" ] &&
		pass "scale 2 下按 2 倍物理像素渲染（grim 的 -g 是逻辑坐标、不乘 scale）" ||
		fail "scale 2 下像素数不对：$SZ，期望 $((MW * 2))x$((MH * 2))"
	# 内容脚印 = contain 几何的 2 倍物理像素（这才是「小窗按逻辑尺寸缩、按物理
	# 像素画」该有的样子）。盒里剩下的透明边露壁纸，探针每帧画面不同 —— **不能**
	# 拿「盒里有纯红」判。判据是**非壁纸像素的外接框**（含 CSD 那圈半透明阴影，
	# 它也盖在壁纸上，所以正好等于整块内容的脚印）。
	#
	# 这条以前只当「有内容」的信号用（门槛放宽到 5%）：那时实测脚印是 86x95 物理
	# 像素（= 该有的一半），根因是 `view_mini_native()` 读到过期的原生尺寸（见
	# README 278），修完按真值判。
	SW2=$(winf w1 sw); SH2=$(winf w1 sh)
	read -r EXW EXH <<< "$(python3 -c "
mw, mh, sw, sh = $MW, $MH, $SW2, $SH2
s = min(mw / sw, mh / sh)
print(round(sw * s * 2), round(sh * s * 2))")"
	read -r CBW CBH <<< "$(nonbgbox "$RUNDIR/s7-mini.ppm")"
	DW=$((CBW > EXW ? CBW - EXW : EXW - CBW)); DH=$((CBH > EXH ? CBH - EXH : EXH - CBH))
	echo "  内容脚印 ${CBW}x${CBH} 物理（原生 ${SW2}x${SH2} contain 进 ${MW}x${MH} 逻辑"\
		"= ${EXW}x${EXH} 物理，非壁纸 $NB / $TOT）"
	[ "$DW" -le $((EXW * 12 / 100)) ] && [ "$DH" -le $((EXH * 12 / 100)) ] &&
		pass "盒里内容 = contain 几何的 2 倍物理像素（实测 ${CBW}x${CBH} ≈ ${EXW}x${EXH}）" ||
		fail "盒里内容脚印不对：${CBW}x${CBH} 物理，期望 ≈ ${EXW}x${EXH}（含 CSD 阴影，±12%）"
fi

# ---- 判据 3：图标贴边裁剪跨缩放边界（scale 2 屏右边缘 → scale 1 邻屏）
# 邻屏**没有壁纸**（wall.py 只盖它所在那块屏 = HEADLESS-2），拿「非背景像素」判
# 漏画会 100% 假阳性 —— 必须用**同一条带的像素差分**：正常态先抓一次基线，进图标态
# 再抓同一条带，两者逐像素相同才算「没漏画」。
diffpx() { python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
import numpy as np
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
print(int(((np.abs(a - b).sum(axis=2)) > 6).sum()))
PY
}
unmini; sleep 1.0     # 先回 NONE：mini_edge 只在「从 NONE 进 ICON」时才给
[ "$(winf w1 mini)" = "none" ] &&
	pass "先回 NONE（图标那条路只在从 NONE 进 ICON 时给锚点）" ||
	fail "没回到 NONE：$(winf w1 mini)"
shot "$RUNDIR/s7-strip-base.ppm" 1920 10 40 80 &&
	echo "  基线条带（正常态，邻屏逻辑 1920,10 40x80）已抓"
set_state icon; sleep 1.0
read -r IX IY IW IH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
echo "  图标（逻辑）$IX,$IY ${IW}x${IH}  屏=$(winf w1 out)  形态=$(winf w1 mini)  edge=$(winf w1 edge)"
[ "$(winf w1 mini)" = "icon" ] && pass "进图标态" || fail "没进图标态（mini=$(winf w1 mini)）"
SPILL=$((IX + IW - EDGE))
if [ "$SPILL" -gt 0 ] && [ "$SPILL" -le 40 ]; then
	pass "图标贴右边缘、溢出到邻屏 $SPILL 逻辑 px（overflow 0.4*64 ≈ 26）"
	if shot "$RUNDIR/s7-strip-icon.ppm" 1920 10 40 80; then
		D=$(diffpx "$RUNDIR/s7-strip-base.ppm" "$RUNDIR/s7-strip-icon.ppm")
		echo "  邻屏（scale 1）逻辑 1920,10 40x80：图标态 vs 正常态 差异像素 $D / 3200"
		[ "$D" = "0" ] &&
			pass "跨缩放边界不漏画：该条带逐像素相同（差 0/3200）" ||
			fail "图标漏到邻屏（scale 边界）：差异像素 $D"
	fi
	# 对照（不判）：图标在**自己那块屏**上的可见部分有内容
	if shot "$RUNDIR/s7-icon-self.ppm" "$IX" "$IY" "$((EDGE - IX))" "$IH"; then
		echo "  自己屏上图标可见那一块 ${IX},$IY $((EDGE - IX))x${IH}：非壁纸像素 $(nonbg "$RUNDIR/s7-icon-self.ppm")"
	fi
else
	fail "图标没贴右边缘溢出（IX=$IX IW=$IW，SPILL=$SPILL）—— 裁剪这条验不了"
fi

# ---- 判据 4：指针坐标换算（scale 2 屏、FREE 态）
# 迷你 / 图标态故意吞输入，所以坐标换算只能在 FREE 态测。盒心正好落在内容中心
# （contain 居中：盒心 ↔ surface 中心），容差 ±3px。两次 motion：一次微移，一次到
# 目标 —— 「原地不动」不会产生 motion 事件。
set_state free; sleep 1.2
read -r FX FY FW FH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
SW=$(winf w1 sw); SH=$(winf w1 sh)
./build/vidowmsg focus-window --id "$ID" >/dev/null 2>&1; sleep 0.4
CX=$((FX + FW / 2)); CY=$((FY + FH / 2))
echo "  自由小窗（逻辑）$FX,$FY ${FW}x${FH}；盒心 $CX,$CY；surface = ${SW}x${SH}"
act "pointer-motion $((CX - 7)) $((CY - 5))" 0.3
act "pointer-motion $CX $CY" 0.5
GOT=$(grep -a '^probe: motion' "$PLOG" | tail -1 |
	sed -n 's/probe: motion \([0-9.-]*\),\([0-9.-]*\)/\1 \2/p')
EX=$(echo "$SW $SH" | awk '{printf "%.0f %.0f", $1 / 2, $2 / 2}')
echo "  注入 $CX,$CY → 期望客户端 [$SW/2, $SH/2] = $EX，实收 ${GOT:-（空）}"
python3 - "$GOT" "$EX" <<'PY'
import sys
g = sys.argv[1].split()
e = sys.argv[2].split()
ok = (len(g) >= 2 and abs(float(g[0]) - float(e[0])) <= 3.0 and
      abs(float(g[1]) - float(e[1])) <= 3.0)
print("  PASS  scale 2 下盒心坐标换算正确（客户端收到 = 原生对应点 ±3px）" if ok else
      "  FAIL  scale 2 下坐标换算错：实收 %s，期望 %s" % (sys.argv[1] or "（空）", sys.argv[2]))
sys.exit(0 if ok else 1)
PY
[ $? -eq 0 ] || FAILED=1

# ================================================================ 关闭小窗
mark "S5 关闭小窗：关闭动画不许闪原生尺寸"
# 用户原话：「关闭小窗的时候也会显示原始大小」。根因：view_snapshot_close() 不管
# view->mini —— 快照是原生缓冲，画布 / 快照节点的尺寸也照原生算（实测关一个
# 504x315 的自由小窗：画布 649x724 = 原生 625x700 + 装饰留白），于是关闭动画第一帧
# 直接按原生大小铺开（内容脚印 637x521，静止时只有 272x304）。修后画布 290x322、
# 内容脚印 280x311。**另起一个客户端**跑这一节（w1 后面 S7 还要用），跑完让它走。
python3 "$RUNDIR/probe.py" closewin > "$RUNDIR/close.log" 2>&1 &
CLIENT_PIDS="$CLIENT_PIDS $!"
sleep 2.0
CID=$(winf closewin id)
if [ -z "$CID" ]; then
	fail "另起的客户端 closewin 没起来"
else
	echo "  closewin id = $CID"
	./build/vidowmsg move-window-to-output HEADLESS-2 --id "$CID" >/dev/null 2>&1
	sleep 0.6
	./build/vidowmsg set-mini-state --id "$CID" --state free >/dev/null 2>&1
	sleep 1.3
	read -r BX BY BW BH <<< "$(echo "$(winf closewin x),$(winf closewin y) $(winf closewin w)x$(winf closewin h)" | tr 'x,' ' ')"
	echo "  自由小窗 $BX,$BY ${BW}x${BH}"
	# 静止时盒里的内容脚印（后面每一帧都不许比它大太多）
	RFW=0; RFH=0
	if shot "$RUNDIR/close-rest.ppm" "$BX" "$BY" "$BW" "$BH"; then
		read -r RFW RFH <<< "$(nonbgbox "$RUNDIR/close-rest.ppm")"
		echo "  静止时盒内内容脚印 ${RFW}x${RFH}"
		[ "$RFW" -gt 50 ] &&
			pass "关闭前盒里有内容（${RFW}x${RFH}，判据才有意义）" ||
			fail "关闭前盒里就没什么内容（${RFW}x${RFH}），后面会假通过"
	fi
	# 关闭动画期间连拍：内容外接框不许超过静止那一份的 1.5 倍
	./build/vidowmsg close-window --id "$CID" >/dev/null 2>&1
	MXW=0
	# 只量**盒子那块**：往外扩会框进同一块屏上列里的别的窗口（实测扩 80px 时
	# 外接框 515 = 自己的快照和隔壁列连成一片），那不是「小窗闪了原生尺寸」。
	for i in 1 2 3 4; do
		if shot "$RUNDIR/close-$i.ppm" "$BX" "$BY" "$BW" "$BH"; then
			W=$(nonbgbox "$RUNDIR/close-$i.ppm" | cut -d' ' -f1)
			echo "  关闭第 $i 帧：内容外接框宽 ${W}px（静止 ${RFW}px）"
			[ "${W:-0}" -gt "$MXW" ] && MXW=$W
		fi
	done
	[ "$RFW" -le 0 ] && RFW=1
	if [ "$MXW" -le $((RFW * 3 / 2)) ]; then
		pass "关闭动画全程没有原生尺寸那一帧（最大外接框 ${MXW} ≤ 静止 ${RFW} × 1.5）"
	else
		fail "关闭动画闪了原生尺寸：某帧外接框 ${MXW} > 静止 ${RFW} × 1.5"
	fi
	kill "$(pgrep -f 'probe.py closewin' | head -1)" 2>/dev/null
fi


# ================================================================ S8：FREE 自由摆放
mark "S8 FREE 自由摆放：拖动跟手 / 拖动中跨屏改归属 / 全屏可达 / resize 真的重配客户端"
# 用户报的三条（这一节是它们的回归）：
#   ① 「小窗移动范围受限，没有全屏幕移动，左右有不可移动区域」+「跨显示器小窗应该
#      移动到临近边，现在会弹到同向边」——FREE 拖动的范围 / 归属；
#   ② 「自由小窗默认应该不限制比例，改大小的时候被限制了比例」——FREE 的 resize 要
#      **真的改客户端尺寸**（客户端跟着重新排版），不是只改留白；
#   ③ 「按盒子右边缘起 Mod+右键没反应」——resize 的命中区。
# 这一节先把布局复位成两块 scale 1 的屏并排（S7 把 HEADLESS-2 调成 scale 2 挪到了
# 右边），判据里的坐标才写死得住：屏 1 工作区 10,10 1260x700、屏 2 工作区 1290,10
# 1260x700（两块屏都是 gaps 10、scale 1）。
./build/vidowmsg output-position HEADLESS-1 0 0 >/dev/null 2>&1; sleep 0.5
./build/vidowmsg output-scale HEADLESS-2 1 >/dev/null 2>&1; sleep 0.6
./build/vidowmsg output-position HEADLESS-2 1280 0 >/dev/null 2>&1; sleep 1.0
# 全局 mini_aspect 在 S3b 里被改过（那节验三值）；FREE 的拖动 / resize 判据要在
# 「不锁比例」下做 —— 那正是新默认值 free。
./build/vidowmsg set-config mini_aspect free >/dev/null; sleep 0.3
ID=$(mini_id)
unmini; sleep 0.7
./build/vidowmsg move-window-to-output HEADLESS-1 --id "$ID" >/dev/null 2>&1; sleep 0.8
[ "$(winf w1 out)" = "HEADLESS-1" ] || fail "w1 没挪回屏 1（$(winf w1 out)）"
set_state free; sleep 1.2
A_X=10; A_Y=10; A_W=1260; A_H=700      # 屏 1 工作区
B_X=1290                              # 屏 2 工作区左上（1280 + gaps 10）
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
echo "  自由小窗 $X,$Y ${W}x${H}（屏 $(winf w1 out)；工作区 ${A_X},${A_Y} ${A_W}x${A_H}）"

# ---- 判据 1：拖动跟手 + **拖动中**（不松手）跨屏改归属 + 从右边进新屏贴左边缘
GX=$((X + W / 2)); GY=$((Y + H / 2))   # 抓盒子中心
OFF=$((GX - X))                        # 抓取偏移：整个拖拽里不变
MARK=$(logmark)
act "pointer-motion $GX $GY" 0.25
act "pointer-button left press mod" 0.2
act "pointer-motion 900 $GY" 0.2       # 同一块屏上先挪一挪
read -r X1 Y1 W1 H1 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
[ $((X1 - (900 - OFF))) -le 3 ] && [ $((X1 - (900 - OFF))) -ge -3 ] &&
	pass "同屏拖动跟手：光标 900 → 盒原点 $X1 ≈ 900 - $OFF = $((900 - OFF))" ||
	fail "同屏拖动没跟手：盒原点 $X1，期望 ≈ $((900 - OFF))"
act "pointer-motion 1300 $GY" 0.25     # 跨过 1280 边界
MID_OUT=$(winf w1 out); MID_GEOM=$(geom)
since "$MARK" | grep -aq '挪到 HEADLESS-2' &&
	pass "拖过边界 → **拖动中**（没松手）就改归属：日志有「挪到 HEADLESS-2」" ||
	fail "拖动中没改归属（$(since "$MARK" | grep -a '挪到' | tail -1)）"
[ "$MID_OUT" = "HEADLESS-2" ] && pass "拖动中 vidowmsg windows 已经报 HEADLESS-2" ||
	fail "拖动中归属还是 $MID_OUT"
read -r X2 Y2 W2 H2 <<< "$(echo "$MID_GEOM" | tr 'x,' ' ')"
echo "  光标 1300（刚过界 20px）→ 盒 $X2,$Y2 ${W2}x${H2}"
[ "$X2" = "$B_X" ] &&
	pass "从右边进新屏 → 盒贴新屏**左**边缘（x=$X2 = 工作区左 $B_X，不是远边）" ||
	fail "进新屏没贴左边缘：x=$X2（期望 $B_X；改前的二进制这里会弹到远边）"
act "pointer-motion 1700 $GY" 0.25
read -r X3 Y3 W3 H3 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
[ $((X3 - (1700 - OFF))) -le 3 ] && [ $((X3 - (1700 - OFF))) -ge -3 ] &&
	pass "跨屏后继续跟手：光标 1700 → 盒原点 $X3 ≈ $((1700 - OFF))" ||
	fail "跨屏后没跟手：盒原点 $X3，期望 ≈ $((1700 - OFF))"
act "pointer-button left release mod" 1.2
read -r X4 Y4 W4 H4 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
[ $((X4 - (1700 - OFF))) -le 3 ] && [ $((X4 - (1700 - OFF))) -ge -3 ] &&
	pass "松手落在光标处：盒原点 $X4 ≈ 1700 - $OFF（屏 $(winf w1 out)）" ||
	fail "松手没落在光标处：盒原点 $X4，期望 ≈ $((1700 - OFF))"
[ "$(winf w1 mini)" = "free" ] && [ "$(winf w1 out)" = "HEADLESS-2" ] &&
	pass "松手后形态仍是 free、归属 HEADLESS-2" ||
	fail "松手后形态/归属不对：$(winf w1 mini) / $(winf w1 out)"
[ "$W4" = "$W" ] && [ "$H4" = "$H" ] &&
	pass "拖动全程盒子尺寸不变（${W4}x${H4} = ${W}x${H}）" ||
	fail "拖动把盒子尺寸改了：${W}x${H} → ${W4}x${H4}"

# ---- 判据 2：FREE 拖到工作区**边缘**松手不收成图标（MINI / ICON 那条一字不改）
MARK=$(logmark)
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
GX=$((X + W / 2)); GY=$((Y + H / 2))
act "pointer-motion $GX $GY" 0.25
act "pointer-button left press mod" 0.2
act "pointer-motion $((B_X + 1260 - 2)) $GY" 0.25
act "pointer-motion $((B_X + 1260 - 1)) $((GY + 1))" 0.25
act "pointer-button left release mod" 1.2
read -r X5 Y5 W5 H5 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
[ "$(winf w1 mini)" = "free" ] &&
	pass "拖到工作区右边缘松手：形态仍是 free（盒 $X5,$Y5 ${W5}x${H5}）" ||
	fail "FREE 拖到边缘被收成图标了：$(winf w1 mini)"
[ "$W5" = "$W" ] && pass "盒子尺寸没被换掉（${W5}x${H5}）" ||
	fail "边缘松手把盒子换掉了：${W}x${H} → ${W5}x${H5}"
since "$MARK" | grep -aq '拖到内边缘' &&
	fail "FREE 走了「拖到内边缘 → 收成图标」那条路（日志有）" ||
	pass "日志里没有「拖到内边缘 → 收成图标」（FREE 不吃这条）"

# ---- 判据 3：可达范围 = 整块工作区（四个方向的极限坐标）
# 拖到每个方向的极限：光标甩到屏外，盒子该顶到工作区的边上（不是内缩
# mini_edge_margin 的 18 / 758）。
./build/vidowmsg move-window-to-output HEADLESS-1 --id "$ID" >/dev/null 2>&1; sleep 0.8
extreme() {  # $1 = 方向名，$2 = 光标 x，$3 = 光标 y
	local bx by bw bh
	read -r bx by bw bh <<< "$(echo "$(geom)" | tr 'x,' ' ')"
	act "pointer-motion $((bx + bw / 2)) $((by + bh / 2))" 0.2
	act "pointer-button left press mod" 0.15
	act "pointer-motion $2 $3" 0.3
	act "pointer-motion $(($2 + 2)) $(($3 + 1))" 0.3
	echo "    $1：光标 $2,$3 → 盒 $(winf w1 x),$(winf w1 y) $(winf w1 w)x$(winf w1 h)"
	act "pointer-button left release mod" 0.7
}
E_L=$(winf w1 w); E_H=$(winf w1 h)
echo "  四个方向的极限（盒子左上角；工作区 ${A_X},${A_Y} ${A_W}x${A_H}，盒 ${E_L}x${E_H}）："
extreme "最左" 5 $((A_Y + A_H / 2))
LX=$(winf w1 x); EX_L=$A_X; [ "$LX" -le $((A_X + 1)) ] &&
	pass "最左 = 工作区左边缘（x=$LX ≤ $A_X + 1）" ||
	fail "最左到不了工作区左边缘：x=$LX（期望 $A_X；改前是 $((A_X + 8)) = 内缩 mini_edge_margin 8）"
extreme "最右" 1270 $((A_Y + A_H / 2))
RX=$(winf w1 x); EX_R=$((A_X + A_W - E_L))
[ "$RX" -ge $((EX_R - 1)) ] &&
	pass "最右 = 工作区右边缘（x=$RX ≥ $EX_R；改前是 $((EX_R - 8))）" ||
	fail "最右到不了工作区右边缘：x=$RX（期望 $EX_R）"
extreme "最上" $((A_X + A_W / 2)) 5
TY=$(winf w1 y)
[ "$TY" -le $((A_Y + 1)) ] && pass "最上 = 工作区上边缘（y=$TY ≤ $A_Y + 1）" ||
	fail "最上到不了工作区上边缘：y=$TY（期望 $A_Y）"
extreme "最下" $((A_X + A_W / 2)) 710
BY=$(winf w1 y); EX_B=$((A_Y + A_H - E_H))
[ "$BY" -ge $((EX_B - 1)) ] &&
	pass "最下 = 工作区下边缘（y=$BY ≥ $EX_B）" ||
	fail "最下到不了工作区下边缘：y=$BY（期望 $EX_B）"
echo "  可达范围：x ∈ [$A_X, $EX_R]、y ∈ [$A_Y, $EX_B]（整块工作区 ${A_W}x${A_H} 都能放）"

# ---- 判据 4：resize 摸得到（按盒子右边缘起 Mod+右键）且**真的**改客户端尺寸
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
SWA=$(winf w1 sw); SHA=$(winf w1 sh)
PMARK=$(plogmark)
MARK=$(logmark)
act "pointer-motion $((X + W - 3)) $((Y + H / 2))" 0.25
act "pointer-button right press mod" 0.25
act "pointer-motion $((X + W + 97)) $((Y + H / 2))" 0.5
act "pointer-button right release mod" 1.3
read -r X6 Y6 W6 H6 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
SWB=$(winf w1 sw); SHB=$(winf w1 sh)
since "$MARK" | grep -aq 'grab resize:' &&
	pass "按盒子右边缘起 resize 命中（日志 grab resize: w1）" ||
	fail "resize 没命中（日志没有 grab resize:；改前的二进制这里抓不住）"
DW=$((W6 - W))
[ "$DW" -ge 90 ] && [ "$DW" -le 104 ] &&
	pass "盒子跟着拖动的宽度改：${W}x${H} → ${W6}x${H6}（宽 +$DW）" ||
	fail "盒子宽度没跟着走：${W}x${H} → ${W6}x${H6}"
[ "$H6" = "$H" ] && pass "高不动（$H6，mini_aspect = free 不锁比例）" ||
	fail "free 下改宽动了高：${H6}（期望 $H）"
DSW=$((SWB - SWA))
[ "${DSW#-}" -ge 90 ] && [ "${DSW#-}" -le 104 ] &&
	pass "客户端**真的**被改成那个大小：surface ${SWA}x${SHA} → ${SWB}x${SHB}（宽 +$DSW）" ||
	fail "客户端尺寸没跟着盒子变：${SWA}x${SHA} → ${SWB}x${SHB}（改前 FREE 里一个 configure 都不发）"
psince "$PMARK" | grep -aq "configure ${SWB}x${SHB}" &&
	pass "客户端自己收到 configure ${SWB}x${SHB}（probe 日志）" ||
	fail "客户端没收到 configure ${SWB}x${SHB}：$(psince "$PMARK" | grep -a 'configure' | tail -1)"
R0=$(python3 -c "print(round($SWA / $SHA, 3))")
R1=$(python3 -c "print(round($SWB / $SHB, 3))")
python3 - "$R0" "$R1" <<'PY'
import sys
r0, r1 = float(sys.argv[1]), float(sys.argv[2])
d = abs(r1 - r0)
print("  PASS  内容比例跟着盒子变（%.3f → %.3f，差 %.3f ≥ 0.2 —— 不是只改留白）" % (r0, r1, d)
      if d >= 0.2 else
      "  FAIL  内容比例没变（%.3f → %.3f）：盒子只改了留白？" % (r0, r1))
sys.exit(0 if d >= 0.2 else 1)
PY
[ $? -eq 0 ] || FAILED=1

# ---- 判据 5：scale 2 的屏上再走一趟（跨屏 / 贴边 / 跟手都得对）
./build/vidowmsg output-scale HEADLESS-2 2 >/dev/null 2>&1; sleep 0.9
./build/vidowmsg output-position HEADLESS-2 1280 0 >/dev/null 2>&1; sleep 0.9
./build/vidowmsg move-window-to-output HEADLESS-1 --id "$ID" >/dev/null 2>&1; sleep 0.8
set_state free; sleep 1.2
read -r X Y W H <<< "$(echo "$(geom)" | tr 'x,' ' ')"
echo "  scale 2 布局：屏 1 工作区 ${A_W}x${A_H}（scale 1）、屏 2 工作区 ${B_X},10 620x340（scale 2）"
GX=$((X + W / 2)); GY=$((Y + H / 2)); OFF=$((GX - X))
# 拖过界之后的 y 必须落在**目标屏的逻辑范围里**：注入走的是 wlr_cursor_warp，落在
# 所有输出之外的坐标 wlroots 直接不认（光标原地不动 → 后面每一次 motion 都是「没
# 动」，判据看着就像「拖不动」）。scale 2 的屏逻辑只有 620x340，屏 1 上盒子中心那
# 个 y（$GY）正好在它下沿之外 —— 真鼠标到不了这种点，测试也别用。
TY=180
MARK=$(logmark)
act "pointer-motion $GX $GY" 0.25
act "pointer-button left press mod" 0.2
act "pointer-motion 1300 $TY" 0.3
# 读「刚进屏那一刻」的位置：再往下走光标盒子会跟着跟手（那是下一条判据），
# 在这里读 x 才对得上「进屏贴左边缘」。
read -r X7 Y7 W7 H7 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
OUT7=$(winf w1 out)
[ "$OUT7" = "HEADLESS-2" ] && pass "scale 2 屏：拖动中改归属（out = HEADLESS-2）" ||
	fail "scale 2 屏上没换屏：$OUT7"
[ "$X7" = "$B_X" ] &&
	pass "scale 2 屏：从右进 → 贴左边缘（x=$X7 = $B_X）" ||
	fail "scale 2 屏进屏没贴左边缘：x=$X7（期望 $B_X）"
# 盒子尺寸按**记住的比例 × 新屏工作区**重算（mini_remember_size 默认开：记的是
# 比例不是像素），所以从拖之前那个盒子（${W}x${H}，屏 1 的 ${A_W}x${A_H}）推。
XW=$((W * 620 / 1260)); XH=$((H * 340 / 700))
[ "$W7" -ge $((XW - 3)) ] && [ "$W7" -le $((XW + 3)) ] &&
	[ "$H7" -ge $((XH - 3)) ] && [ "$H7" -le $((XH + 3)) ] &&
	pass "scale 2 屏：盒按**逻辑**工作区算（${W7}x${H7} ≈ ${XW}x${XH} = ${W}x${H} × 620/1260 × 340/700）" ||
	fail "scale 2 屏的盒尺寸不对：${W7}x${H7}（期望 ≈ ${XW}x${XH}）"
act "pointer-motion 1500 $TY" 0.3
act "pointer-motion 1900 $TY" 0.3
read -r X8 Y8 W8 H8 <<< "$(echo "$(geom)" | tr 'x,' ' ')"
[ $((X8 - (1900 - OFF))) -le 3 ] && [ $((X8 - (1900 - OFF))) -ge -3 ] &&
	pass "scale 2 屏：继续跟手（光标 1900 → 盒原点 $X8 ≈ $((1900 - OFF))）" ||
	fail "scale 2 屏上没跟手：盒原点 $X8，期望 ≈ $((1900 - OFF))"
act "pointer-button left release mod" 1.2
echo "  跨屏日志：$(since "$MARK" | grep -a '挪到' | tail -1)"
./build/vidowmsg output-scale HEADLESS-2 1 >/dev/null 2>&1; sleep 0.6

# ================================================================ S9：工具条在 scale 2 屏上
# 用户：「整个工具条都不能交互」（他在 DP-2，3840x2160 @ scale 2 上）。上面那条
# 判据只在 scale 1 的单屏上验过 —— 这里在 scale 2 屏上照同一条路再走一遍：
# 悬停迷你小窗 → 工具条露出 → 点「还原成列」→ 形态回到 none。
mark "S9 工具条在 scale 2 屏上：悬停露出 + 点「还原成列」"
ID=$(mini_id)
unmini; sleep 0.7
./build/vidowmsg move-window-to-output HEADLESS-2 --id "$ID" >/dev/null 2>&1
./build/vidowmsg output-scale HEADLESS-2 2 >/dev/null 2>&1
sleep 1.0
set_state mini; sleep 1.0
read -r IX IY IW IH <<< "$(echo "$(geom)" | tr 'x,' ' ')"
echo "  scale 2 屏上的迷你盒：$IX,$IY ${IW}x${IH}"
MARK=$(logmark)
act "pointer-motion $((IX + IW / 2)) $((IY + IH / 2))" 0.5
TL=$(since "$MARK" | grep -a 'mini toolbar: w1 露出' | tail -1)
echo "  ${TL:-(工具条没露出)}"
if [ -n "$TL" ]; then
	pass "scale 2 屏：悬停迷你小窗 → 工具条露出"
	BX=$(echo "$TL" | sed -n 's/.*还原成列 \([0-9]*\),\([0-9]*\).*/\1/p')
	BY=$(echo "$TL" | sed -n 's/.*还原成列 \([0-9]*\),\([0-9]*\).*/\2/p')
	CCX=$((BX + 17)); CCY=$((BY + 13))
	# 走**真实路径**：先经过工具条与窗口之间那条 4px 缝，再到按钮上。一步跳到按钮
	# 上测不出「缝里被收掉」那个 bug（用户报的「整个工具条都不能交互」）。修法见
	# vidow_mini_toolbar_near()。
	# 缝在哪取决于工具条摆上还是摆下（摆上方放不下时会翻到窗口下面，见
	# mini_toolbar_layout），所以按日志里工具条实际的 y 取。
	TBY=$(echo "$TL" | sed -n 's/.*整块 [0-9]*,\([0-9]*\) [0-9]*x[0-9]*.*/\1/p')
	if [ -n "$TBY" ] && [ "$TBY" -gt "$IY" ]; then
		GAPY=$((IY + IH + 2))
	else
		GAPY=$((IY - 2))
	fi
	act "pointer-motion $CCX $GAPY" 0.3
	act "pointer-motion $CCX $CCY" 0.25
	act "pointer-button left press" 0.5
	act "pointer-button left release" 0.8
	BTN=$(since "$MARK" | grep -a '点了按钮' | tail -1)
	echo "  ${BTN:-(没有点按钮的日志)}"
	[ -n "$BTN" ] && pass "scale 2 屏：点中按钮（工具条 $BX,$BY，点的是 $CCX,$CCY）" ||
		fail "scale 2 屏：按钮没点中（工具条 $BX,$BY，点的是 $CCX,$CCY）"
	[ "$(winf w1 mini)" = "none" ] && pass "scale 2 屏：「还原成列」→ 形态回到 none" ||
		fail "scale 2 屏：按钮点了但形态没还原：$(winf w1 mini)"
else
	fail "scale 2 屏：工具条没露出来"
fi
./build/vidowmsg output-scale HEADLESS-2 1 >/dev/null 2>&1; sleep 0.5

# ================================================================
exec 9>&-
echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-mini.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
