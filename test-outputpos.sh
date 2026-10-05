#!/bin/bash
# 输出布局位置：`output_positions = "DP-3:0,0; DP-2:1920,0"`
#
# 背景：哪块屏在左边**不能**靠自动排 —— wlr_output_layout_add_auto 的顺序是后端
# 给的（headless 上实测是反的：先建的那块被摆到 x=1280）。要确定布局就得写下来。
# 坐标是逻辑坐标（= 物理像素 / 那块屏的缩放），所以两块 4K@2x 的屏并排就是
# 0,0 和 1920,0。
#
# 判据（两块 1280x720@2x 的 headless 屏，逻辑各 640x360）：
#   1) 按配置摆：HEADLESS-1 在 0,0、HEADLESS-2 在 640,0（vidowmsg outputs）
#   2) 日志里说得出「output_positions 指定」（不然人不知道位置哪来的）
#   3) 逗号分隔的写法也认（隔壁 outputs 键就是逗号分隔的，手滑写错不该静默失效）
#   4) 拔掉再插回来：还是摆回配置说的位置
#   5) 改配置 + SIGHUP：位置跟着改（不用重启）
#   6) 日志里没有 assert / abort
#
# 用法：./test-outputpos.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-outputpos-test
CFGDIR="$RUNDIR/home"
FAILED=0
VIDOW_PGID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

cleanup() {
	if [ -n "$VIDOW_PGID" ]; then
		kill -TERM -- "-$VIDOW_PGID" 2>/dev/null
		sleep 0.4
		kill -KILL -- "-$VIDOW_PGID" 2>/dev/null
	fi
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=

# 两块屏的布局（逻辑坐标）：HEADLESS-1 在左，HEADLESS-2 在它右边 640 处
write_config() { # <positions 行>
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
outputs = "HEADLESS-1:2,HEADLESS-2:2"
scale = "auto"
xwayland = false
$1
CFG
}

positions_of() { # 从 vidowmsg outputs 里读「名字 -> 布局 x」
	./build/vidowmsg outputs 2>/dev/null | python3 -c '
import json, sys
d = json.load(sys.stdin)
outs = d.get("outputs", d) if isinstance(d, dict) else d
print(" ".join("%s=%d" % (o["name"], o["layout"]["x"]) for o in outs))
'
}

mark "1) 按配置摆位置"
write_config 'output_positions = "HEADLESS-1:0,0; HEADLESS-2:640,0"'
setsid ./build/vidow > "$RUNDIR/log" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$RUNDIR/log" && break; sleep 0.25; done
if ! grep -aq "vidow ready" "$RUNDIR/log"; then
	echo "vidow 没起来"; tail -5 "$RUNDIR/log"; exit 1
fi
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$RUNDIR/log" | tail -1 | cut -d= -f2)
sleep 0.5
GOT=$(positions_of)
echo "  布局: $GOT"
grep -a "output HEADLESS" "$RUNDIR/log" | sed 's/^/  /'

mark "2) 逗号分隔的写法也认"
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null; sleep 0.5
write_config 'output_positions = "HEADLESS-2:640,0,HEADLESS-1:0,0"'
setsid ./build/vidow > "$RUNDIR/log2" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$RUNDIR/log2" && break; sleep 0.25; done
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$RUNDIR/log2" | tail -1 | cut -d= -f2)
sleep 0.5
GOT2=$(positions_of)
echo "  布局: $GOT2"

mark "3) 拔掉再插回来"
kill -TERM -- "-$VIDOW_PGID" 2>/dev/null; sleep 0.5
write_config 'output_positions = "HEADLESS-1:0,0; HEADLESS-2:640,0"'
VIDOW_TEST_OUTPUTS="1200:remove:HEADLESS-2,1800:add:1280x720:HEADLESS-2" \
	setsid ./build/vidow > "$RUNDIR/log3" 2>&1 &
VIDOW_PGID=$!
for _ in $(seq 1 60); do grep -aq "vidow ready" "$RUNDIR/log3" && break; sleep 0.25; done
export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$RUNDIR/log3" | tail -1 | cut -d= -f2)
sleep 2.5
GOT3=$(positions_of)
echo "  拔插后: $GOT3"
grep -a "test:\|output HEADLESS" "$RUNDIR/log3" | sed 's/^/  /'

mark "4) 改配置 + SIGHUP"
# 左右对调
cat > "$CFGDIR/.config/vidow/config.lua" <<'CFG'
outputs = "HEADLESS-1:2,HEADLESS-2:2"
scale = "auto"
xwayland = false
output_positions = "HEADLESS-1:640,0; HEADLESS-2:0,0"
CFG
kill -HUP "$VIDOW_PGID" 2>/dev/null
sleep 1.2
GOT4=$(positions_of)
echo "  SIGHUP 后: $GOT4"
grep -a "布局位置" "$RUNDIR/log3" | sed 's/^/  /'

mark "5) 断言"
python3 - "$GOT" "$GOT2" "$GOT3" "$GOT4" <<'PY'
import re, sys

got, got2, got3, got4 = sys.argv[1:5]
failed = []

def check(ok, msg):
    print(("  PASS  " if ok else "  FAIL  ") + msg)
    if not ok:
        failed.append(msg)

def pos(s, name):
    m = re.search(r"\b%s=(-?\d+)\b" % re.escape(name), s)
    return int(m.group(1)) if m else None

check(pos(got, "HEADLESS-1") == 0 and pos(got, "HEADLESS-2") == 640,
      "按配置摆：HEADLESS-1 在 0、HEADLESS-2 在 640（%s）" % got)
check(pos(got2, "HEADLESS-1") == 0 and pos(got2, "HEADLESS-2") == 640,
      "逗号分隔写法也认（%s）" % got2)
check(pos(got3, "HEADLESS-1") == 0 and pos(got3, "HEADLESS-2") == 640,
      "拔掉再插回来还是配置说的位置（%s）" % got3)
check(pos(got4, "HEADLESS-1") == 640 and pos(got4, "HEADLESS-2") == 0,
      "SIGHUP 后左右对调生效（%s）" % got4)

print()
if failed:
    print("test-outputpos.sh: %d 项没通过" % len(failed))
    sys.exit(1)
print("全部通过 / test-outputpos.sh: OK")
PY
[ $? -eq 0 ] || FAILED=1

mark "6) 日志"
for f in log log2 log3; do
	if grep -aq "按 output_positions" "$RUNDIR/$f"; then
		pass "$f：日志里说了位置是 output_positions 定的"
	else
		fail "$f：日志里没说位置是 output_positions 定的"
	fi
	if grep -aq "Assertion" "$RUNDIR/$f" || grep -aqi "abort" "$RUNDIR/$f"; then
		fail "$f：日志里有 assert / abort"
	else
		pass "$f：没有 assert / abort"
	fi
done

echo
if [ $FAILED -eq 0 ]; then
	echo "test-outputpos.sh: OK"
else
	echo "test-outputpos.sh: 有失败项"
fi
exit $FAILED
