#!/bin/bash
# 性能基线采集 —— 一个场景跑一次，输出一行 JSON + 明细文件。
#
# 为什么这么搭（每条都是踩过的坑）：
#   * 后端用 headless + gles2 + surfaceless = 真 radeonsi（日志 EGL driver name:
#     radeonsi）。嵌套 wayland 后端量不到任何统计（frames 恒 0），已放弃。
#   * 统计开关是 /tmp/vidow-stats-on 这个文件（运行中的 vidow 也能开），统计行走
#     INFO 打到日志；环境变量 VIDOW_RENDER_STATS=1 那条是 DEBUG 级。
#   * 配置键是全局的（blur_enabled = true）：写成 vidow.blur_enabled 会静默
#     applied (0 commands)；文件必须落在 $HOME/.config/vidow/config.lua。
#   * 拖拽只能走 VIDOW_TEST_ACTION 的 FIFO（vidowmsg 没有 mod 参数，不按 Mod 拖不
#     起来）；坐标必须从 vidowmsg windows 的实测几何算，猜坐标会点空。
#   * 帧数不取 IPC 累积值（vidow_stats.frames 每 500ms 清零，IPC 读到会变小），直接
#     解析每行统计里的 fps / 每帧毫秒。
#
# 用法: tools/perf-scene.sh <idle|drag|game|multiscreen|capture> [--secs 10] [--label 说明]
set -u
cd "$(dirname $0)/.."
ROOT=$PWD
SCENE=idle
SECS=10
LABEL=""
while [ $# -gt 0 ]; do
	case "$1" in
	--secs) SECS="$2"; shift 2 ;;
	--label) LABEL="$2"; shift 2 ;;
	idle|drag|game|multiscreen|capture|busy) SCENE="$1"; shift ;;
	*) echo "用法: $0 <idle|drag|game|multiscreen|capture> [--secs N] [--label 说明]" >&2; exit 2 ;;
	esac
done

TS=$(date +%m%d-%H%M%S)
RUN="/tmp/vidow-perf-$SCENE-$TS"
DETAIL="/tmp/vidow-perf-$SCENE-$TS.txt"
LOG="$RUN/vidow.log"
MSG="$ROOT/build/vidowmsg"
mkdir -p "$RUN/home/.config/vidow" "$RUN/rt"
cat > "$RUN/home/.config/vidow/config.lua" <<'LUA'
scale = "1"
gaps = 10
window_move_ms = 0
blur_enabled = true
blur_all = true
blur_levels = 4
blur_strength = 1.0
jelly = true
corner_radius = 10
border_width = 3
xwayland = false
vidow.bind("Mod+mouse-left", "interactive-move")
vidow.bind("Mod+mouse-right", "interactive-resize")
LUA
# PERF_EXTRA_LUA 允许临时改一条配置做 A/B（例：PERF_EXTRA_LUA="blur_enabled = false"）
[ -n "${PERF_EXTRA_LUA:-}" ] && echo "$PERF_EXTRA_LUA" >> "$RUN/home/.config/vidow/config.lua"
# 也支持从文件读额外配置（避免命令行里嵌引号把 Lua 弄坏）
[ -n "${PERF_EXTRA_LUA_FILE:-}" ] && cat "$PERF_EXTRA_LUA_FILE" >> "$RUN/home/.config/vidow/config.lua"
mkfifo "$RUN/actions"
CARD=""
BEST=0
for c in /sys/class/drm/card[0-9]*/device; do
	[ -r "$c/mem_info_vram_total" ] || continue
	t=$(cat "$c/mem_info_vram_total")
	if [ "$t" -gt "$BEST" ]; then BEST=$t; CARD=$c; fi
done
[ -n "$CARD" ] || CARD=$(ls -d /sys/class/drm/card[0-9]*/device 2>/dev/null | head -1)
GPUSLOT=$(grep -a PCI_SLOT_NAME "$CARD/uevent" 2>/dev/null | cut -d= -f2)
GPUTOTAL=$((BEST / 1048576))
PIDS=()
VIDOW_PID=""
cleanup() {
	for p in ${PIDS[@]:-}; do kill "$p" 2>/dev/null; done
	[ -n "$VIDOW_PID" ] && kill -TERM "$VIDOW_PID" 2>/dev/null
	sleep 0.4
	[ -n "$VIDOW_PID" ] && kill -KILL "$VIDOW_PID" 2>/dev/null
	rm -f /tmp/vidow-stats-on
}
trap cleanup EXIT

touch /tmp/vidow-stats-on
OUTN=1
[ "$SCENE" = multiscreen ] && OUTN=2
chmod 700 "$RUN/rt"
# PERF_RENDERER=default 时走 llvmpipe（调试用），默认强制真 radeonsi
if [ "${PERF_RENDERER:-gpu}" = default ]; then
	REND_ENV=""
else
	REND_ENV="EGL_PLATFORM=surfaceless WLR_RENDERER=gles2"
fi
( export HOME="$RUN/home" XDG_RUNTIME_DIR="$RUN/rt" $REND_ENV
  export WLR_LIBINPUT_NO_DEVICES=1 VIDOW_LOG=debug DISPLAY= WAYLAND_DISPLAY=
  exec env WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS="$OUTN" $REND_ENV \
    VIDOW_TEST_ACTION="file:$RUN/actions" "$ROOT/build/vidow" > "$LOG" 2>&1 ) &
VIDOW_PID=$!
# socket 不猜位置：vidow 可能把 wayland / IPC 落在 XDG_RUNTIME_DIR、/run/user/<uid>
# 或别处，直接找出来（猜错就是客户端连不上 / vidowmsg 报找不到 socket）。
SOCK=""
for _ in $(seq 1 150); do
	for d in "$RUN/rt" "${XDG_RUNTIME_DIR:-}" "/run/user/$(id -u)" /tmp/rt-x; do
		[ -n "$d" ] || continue
		s=$(ls "$d"/vidow-wayland-*.sock 2>/dev/null | head -1)
		if [ -n "$s" ]; then SOCK=$s; break 2; fi
	done
	sleep 0.1
done
if [ -z "$SOCK" ]; then
	echo "找不到 vidow 的 IPC socket（看了 $RUN/rt、/run/user/$(id -u)、/tmp/rt-x）" >&2
	tail -5 "$LOG" >&2
	exit 1
fi
SOCKDIR=$(dirname "$SOCK")
CLIENT_WL=$(basename "$SOCK")
CLIENT_WL=${CLIENT_WL#vidow-}
CLIENT_WL=${CLIENT_WL%.sock}
export VIDOW_SOCKET="$SOCK"
exec 9> "$RUN/actions"
echo "== socket: $SOCK（WLD=$CLIENT_WL）"
echo "== run/user: $(ls /run/user/1000 2>/dev/null | tr '\n' ' ')"

spawn() {
	local n=$(( $(ls "$RUN"/client-*.log 2>/dev/null | wc -l) + 1 ))
	( export HOME="$RUN/home" XDG_RUNTIME_DIR="$SOCKDIR" WAYLAND_DISPLAY="$CLIENT_WL"
	  cd "$ROOT" && exec $@ ) > "$RUN/client-$n.log" 2>&1 &
	PIDS+=($!)
}
vmsg() { VIDOW_SOCKET="$SOCK" "$MSG" $@ 2>/dev/null; }

for i in $(seq 0 $((OUTN - 1))); do
	spawn python3 tools/wall.py --output "$i" --ns "wall$i"
done
sleep 1.2

case "$SCENE" in
idle)
	# 真空闲一帧都不出（屏幕没变化就不提交），所以按 2Hz 极轻地动光标：
	# 这是「最省的时候也省不掉的那点成本」，别拿它当 0。
	;;
drag)
	spawn python3 tools/pattern.py wA      # 被拖的窗：果冻 + 装饰重烘
	sleep 1.2
	spawn python3 tools/silk.py           # 真半透明窗：逼出私有底衬 + 模糊链
	sleep 1.2
	for _ in $(seq 1 60); do
		grep -aq "map wA" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "map wA" "$LOG"; then echo "wA 没 map（见 $RUN/client-*.log）" >&2; exit 1; fi
	sleep 0.8
	vmsg toggle-floating --title wA >/dev/null
	sleep 0.6
	;;
game)
	spawn python3 tools/pattern.py gamepat
	sleep 1.6
	vmsg set-window-size 1280 720
	sleep 0.4
	vmsg toggle-fullscreen
	sleep 0.8
	;;
busy)
	# 每个窗都在不停重画（模拟视频/滚动/动画）：把每帧的合成路径真正压满
	spawn python3 tools/spin.py s1 1 0 0 full
	spawn python3 tools/spin.py s2 0 1 0 full
	spawn python3 tools/spin.py s3 0 0 1 full
	spawn python3 tools/spin.py s4 1 1 0 full
	spawn python3 tools/spin.py s5 0 1 1 full
	spawn python3 tools/spin.py s6 1 0 1 full
	sleep 2.0
	;;
multiscreen)
	# 两块 1280 宽的屏，一屏两格：4 个窗正好摆满，再放就挤到屏外
	# （挤出去会被 workspace 判成「停到屏外」而藏起来，模糊链也就不建了）。
	# 顺序 = 叠放顺序：spin 在 silk 下面不停重画，silk 的私有底衬才会每帧重抓，
	# 模糊成本才量得出来（静止画面里模糊只烘一次，统计几乎全是 0）。
	spawn python3 tools/pattern.py m1
	spawn python3 tools/spin.py sp1 1 0 0 full
	spawn python3 tools/silk.py           # 真半透明窗：逼出私有底衬 + 模糊链
	spawn python3 tools/pattern.py m2
	sleep 2.0
	;;
capture)
	spawn python3 tools/pattern.py cap
	sleep 1.6
	if command -v wf-recorder >/dev/null; then
		( export HOME="$RUN/home" XDG_RUNTIME_DIR="$SOCKDIR" WAYLAND_DISPLAY="$CLIENT_WL"
		  cd "$ROOT" && exec wf-recorder -f "$RUN/cap.mkv" ) >/dev/null 2>&1 &
		PIDS+=($!)
		sleep 1.0
	fi
	;;
esac

vmsg windows > "$RUN/windows0.json" 2>/dev/null
: > "$RUN/gpu.txt"

case "$SCENE" in
idle)
	(
		while :; do
			for x in 640 641; do
				echo "pointer-motion $x 360" >&9
				sleep 0.5
			done
		done
	) &
	PIDS+=($!)
	;;
drag)
	CX=500; CY=400        # kitty 平铺在左上，这个点在它里面（实测能起拖）
	echo "== 拖 wA：起拖点 $CX,$CY（headless 下 vidowmsg windows 返回空，只能固定坐标）=="
	echo "pointer-motion $CX $CY" >&9
	sleep 0.3
	echo "pointer-button left press mod" >&9
	sleep 0.4
	(
		# 一步 8 像素、来回摆 —— 单步跳太远（>100px）会让果冻走「布局搬走」
		# 分支把偏移清零，拖拽看起来就断了（踩过）。
		x=$CX
		dir=1
		while :; do
			x=$((x + dir * 8))
			if [ "$x" -gt $((CX + 100)) ] || [ "$x" -lt $((CX - 100)) ]; then
				dir=$((-1 * dir))
			fi
			echo "pointer-motion $x $CY" >&9
			sleep 0.05
		done
	) &
	PIDS+=($!)
	;;
game|capture|multiscreen)
	(
		while :; do
			for x in 300 500 700 900 700 500; do
				echo "pointer-motion $x 300" >&9
				sleep 0.05
			done
		done
	) &
	PIDS+=($!)
	;;
esac

# 采集中途截一张：数字不对时先看屏幕（空白 = 客户端没画，不是优化成功）
( sleep $((SECS / 2)); if command -v grim >/dev/null; then
	HOME="$RUN/home" XDG_RUNTIME_DIR="$SOCKDIR" WAYLAND_DISPLAY="$CLIENT_WL" \
		grim "$RUN/shot.png" 2>/dev/null
  fi ) &
PIDS+=($!)

END=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$END" ]; do
	printf '%s %s %s\n' "$(date +%s)" \
		"$(cat "$CARD/gpu_busy_percent" 2>/dev/null || echo NA)" \
		"$(awk '{printf "%d", $1/1048576}' "$CARD/mem_info_vram_used" 2>/dev/null || echo NA)" \
		>> "$RUN/gpu.txt"
	sleep 0.2
done

[ "$SCENE" = drag ] && { echo "pointer-button left release" >&9; sleep 0.3; }
DRAG_OK=-
if [ "$SCENE" = drag ]; then
	if grep -aq "grab move: " "$LOG"; then DRAG_OK=1; else DRAG_OK=0; fi
fi
RSS=$(awk '/^Rss:/ {print $2}' "/proc/$VIDOW_PID/smaps_rollup" 2>/dev/null || echo 0)

python3 - "$SCENE" "$LABEL" "$SECS" "$RUN" "$DETAIL" "$DRAG_OK" "$RSS" "$GPUSLOT" "$GPUTOTAL" <<'PY'
import json, re, statistics, sys
scene, label, secs, run, detail, drag_ok, rss, gpuslot, gputotal = sys.argv[1:10]
secs = int(secs)
logtxt = open(run + "/vidow.log", "rb").read().decode("utf-8", "replace")
lines = [l for l in logtxt.splitlines() if "渲染统计" in l]

def num(pat, s, d=0.0):
    m = re.search(pat, s)
    return float(m.group(1)) if m else d

fps, blur, decor, jelly, commit, damage, chains, bakes, passes = [], [], [], [], [], [], [], [], []
for l in lines:
    fps.append(num(r"([\d.]+) fps", l))
    blur.append(num(r"模糊 ([\d.]+)ms/帧", l))
    decor.append(num(r"装饰 ([\d.]+)ms/帧", l))
    jelly.append(num(r"果冻 ([\d.]+)ms/帧", l))
    commit.append(num(r"提交 ([\d.]+)ms/帧", l))
    damage.append(num(r"损伤 ([\d.]+)%/帧", l))
    chains.append(num(r"chain (\d+)", l))
    bakes.append(num(r"\((\d+) 次重烘\)", l))
    passes.append(num(r"趟 (\d+)", l))

def med(v):
    return round(statistics.median(v), 2) if v else 0.0

gpu = []
try:
    for row in open(run + "/gpu.txt"):
        p = row.split()
        if len(p) >= 3 and p[1] != "NA":
            gpu.append((float(p[1]), float(p[2])))
except OSError:
    pass
busy = [g[0] for g in gpu]
vram = [g[1] for g in gpu]

out = {
    "scene": scene, "label": label, "secs": secs,
    "samples": len(lines),
    "fps_median": med(fps),
    "ms_per_frame": {"blur": med(blur), "decor": med(decor),
                     "jelly": med(jelly), "commit": med(commit)},
    "chain_per_frame": int(med(chains)),
    "bakes_per_sample": med(bakes),
    "passes_median": med(passes),
    "damage_pct_median": med(damage),
    "gpu_busy_median": med(busy),
    "gpu_busy_max": max(busy) if busy else 0.0,
    "vram_mb_median": med(vram),
    "rss_mb": round(int(rss or 0) / 1024.0, 1),
    "drag_started": drag_ok,
    "gpu": gpuslot, "gpu_vram_total_mb": int(gputotal),
    "log": run + "/vidow.log",
}
print(json.dumps(out, ensure_ascii=False))
with open(detail, "w") as fh:
    fh.write("场景 %s  时长 %ss  标签 %s\n" % (scene, secs, label))
    fh.write(json.dumps(out, ensure_ascii=False, indent=2) + "\n\n")
    fh.write("== 统计行（最后 40 条）==\n")
    for l in lines[-40:]:
        fh.write(l[-400:] + "\n")
PY
echo "明细: $DETAIL   日志: $LOG"
