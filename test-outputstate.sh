#!/bin/bash
# 每块屏那一组的「其余项」：模式（分辨率 / 刷新率）、VRR、位深、方向、开关
#
#   vidow.output{ name = "DP-3", mode = "3840x2160@144", vrr = true,
#                 depth = 10, transform = "90", enabled = true }
#
# 判据（两块 1280x720 的 headless 屏）：
#   1) mode：HEADLESS-1 上 800x600@75（headless 只支持自定义模式）—— vidowmsg 里
#      宽高刷新率都对，日志说了「模式 -> 800x600 @ 75.00Hz」
#   2) depth = 10：输出缓冲换成 XRGB2101010（vidowmsg 报 depth=10），日志说清楚
#   3) vrr = true：headless 后端在 wlroots 里不吃 adaptive sync —— 只报一句
#      「不支持 VRR」，别的设置照常生效、合成器照常跑（不许整次 commit 一起失败）
#   4) transform = "90"：vidowmsg 里方向是 90
#   5) enabled = false：那块屏关掉，另一块不动
#   6) 改配置 + SIGHUP：上面每一项都跟着改（不用重启）
#   7) 写错的值（mode="800x600x2" / depth=12 / transform="sideways"）当场报错，
#      不静默生效
#   8) 日志里没有 assert / abort
#
# 用法：./test-outputstate.sh   （VIDOW_TEST_KEEP=1 保留运行目录）
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-ostate-test
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

write_config() { # <每块屏那几行>
	cat > "$CFGDIR/.config/vidow/config.lua" <<CFG
scale = "auto"
xwayland = false
$1
CFG
}

# 从 vidowmsg outputs 里取一块屏的字段：字段 名字 -> 值
field_of() { # <输出名> <字段>
	./build/vidowmsg outputs 2>/dev/null | python3 -c '
import json, sys
name, key = sys.argv[1], sys.argv[2]
for o in json.load(sys.stdin):
    if o["name"] == name:
        v = o.get(key)
        if isinstance(v, float):
            v = "%g" % v   # 75.0 -> 75（JSON 里就是这么打的）
        print("True" if v is True else "False" if v is False else v)
        break
else:
    print("(没有这块屏)")
' "$1" "$2"
}

start_vidow() { # <日志文件>
	setsid ./build/vidow > "$1" 2>&1 &
	VIDOW_PGID=$!
	for _ in $(seq 1 60); do grep -aq "vidow ready" "$1" && break; sleep 0.25; done
	if ! grep -aq "vidow ready" "$1"; then
		echo "vidow 没起来"; tail -5 "$1"; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$1" | tail -1 | cut -d= -f2)
	sleep 1.0
}

check() { # <期望> <实际> <说明>
	if [ "$1" = "$2" ]; then
		pass "$3（$2）"
	else
		fail "$3（期望 $1，实际 $2）"
	fi
}

mark "1) 每块屏一组：模式 / 位深 / VRR / 方向 / 开关"
write_config 'vidow.output{ name = "HEADLESS-1", mode = "800x600@75", depth = 10, vrr = true, transform = "90" }
vidow.output{ name = "HEADLESS-2", enabled = false }'
start_vidow "$RUNDIR/log"
grep -a "output 配置" "$RUNDIR/log" | sed 's/^.*\] //' | sed 's/^/  /'
grep -a "output HEADLESS-1: 模式\|output HEADLESS-1: 输出缓冲\|output HEADLESS-1: 这块屏\|output HEADLESS-2: 关掉\|output HEADLESS-1: 方向" "$RUNDIR/log" | sed 's/^.*\] //' | sed 's/^/  /'

mark "2) 断言：模式 / 位深 / VRR / 方向 / 开关"
check "800" "$(field_of HEADLESS-1 width)" "HEADLESS-1 上了 800x600（宽）"
check "600" "$(field_of HEADLESS-1 height)" "HEADLESS-1 上了 800x600（高）"
check "75" "$(field_of HEADLESS-1 refresh_hz)" "HEADLESS-1 刷新率 75Hz"
check "10" "$(field_of HEADLESS-1 depth)" "HEADLESS-1 输出缓冲 10 位"
check "xrgb2101010" "$(field_of HEADLESS-1 render_format)" "HEADLESS-1 缓冲格式 XRGB2101010"
check "False" "$(field_of HEADLESS-1 adaptive_sync)" "headless 上 VRR 没生效（后端不支持）"
check "False" "$(field_of HEADLESS-1 vrr_supported)" "headless 报 vrr_supported=false（vidow 自己先问过再试）"
check "90" "$(field_of HEADLESS-1 transform)" "HEADLESS-1 方向 90"
check "False" "$(field_of HEADLESS-2 enabled)" "HEADLESS-2 按配置关掉了"
check "True" "$(field_of HEADLESS-1 enabled)" "HEADLESS-1 还开着"

if grep -aq "这块屏 / 这个后端不支持 VRR" "$RUNDIR/log"; then
	pass "日志说清楚了 VRR 为什么没生效（不是静默）"
else
	fail "日志没说 VRR 为什么没生效"
fi

mark "3) 改配置 + SIGHUP：每一项都跟着改，不用重启"
write_config 'vidow.output{ name = "HEADLESS-1", mode = "1024x768@60", depth = 8, vrr = false, transform = "normal" }
vidow.output{ name = "HEADLESS-2", enabled = true, mode = "640x480" }'
kill -HUP "$VIDOW_PGID" 2>/dev/null
sleep 1.5
grep -a "按配置（vidow.output）" "$RUNDIR/log" | tail -4 | sed 's/^.*\] //' | sed 's/^/  /'
check "1024" "$(field_of HEADLESS-1 width)" "SIGHUP 后 HEADLESS-1 换成 1024x768"
check "768" "$(field_of HEADLESS-1 height)" "SIGHUP 后 HEADLESS-1 换成 1024x768（高）"
check "8" "$(field_of HEADLESS-1 depth)" "SIGHUP 后位深回到 8"
check "xrgb8888" "$(field_of HEADLESS-1 render_format)" "SIGHUP 后缓冲格式回到 XRGB8888"
check "normal" "$(field_of HEADLESS-1 transform)" "SIGHUP 后方向回到 normal"
check "True" "$(field_of HEADLESS-2 enabled)" "SIGHUP 后 HEADLESS-2 又打开了"
check "640" "$(field_of HEADLESS-2 width)" "SIGHUP 后 HEADLESS-2 上了 640x480"

mark "4) 写错的值当场报错，不静默"
for bad in 'vidow.output{ name = "HEADLESS-1", mode = "800x600x2" }' \
	'vidow.output{ name = "HEADLESS-1", depth = 12 }' \
	'vidow.output{ name = "HEADLESS-1", transform = "sideways" }' \
	'vidow.output{ name = "HEADLESS-1", vrrv = true }'; do
	echo "  --- $bad"
	write_config "$bad"
	kill -HUP "$VIDOW_PGID" 2>/dev/null
	sleep 0.9
	LINE=$(grep -a "config.lua:.*vidow.output" "$RUNDIR/log" | tail -1 | sed 's/^.*\] //')
	if [ -n "$LINE" ]; then
		pass "报错了：$(echo "$LINE" | sed 's|.*config.lua:||')"
	else
		fail "没报错（这一行静默生效了）"
	fi
done

mark "5) 收尾"
# 配置换回正常的一份，别让最后一次的坏配置留在那儿
write_config 'vidow.output{ name = "HEADLESS-1", mode = "1024x768@60" }'
kill -HUP "$VIDOW_PGID" 2>/dev/null
sleep 1.0
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	pass "合成器还活着（没有 abort）"
else
	fail "合成器死了"
fi
if grep -aq "Assertion" "$RUNDIR/log" || grep -aqi "abort" "$RUNDIR/log"; then
	fail "日志里有 assert / abort"
else
	pass "日志里没有 assert / abort"
fi

echo
if [ $FAILED -eq 0 ]; then
	echo "test-outputstate.sh: OK"
else
	echo "test-outputstate.sh: 有失败项"
fi
exit $FAILED
