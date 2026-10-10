#!/bin/bash
# 会话环境：vidow 和 systemd 用户管理器两个方向的对账 + 自启动（exec-once）。
# 见 src/env.c。
#
# 判据：
#   1) 从 systemd 吸：我们自己没有的变量吸进来（WLR_* / WAYLAND_DISPLAY 这类不吸，
#      我们自己有的也不覆盖）
#   2) 推给 systemd / dbus：import-environment + dbus-update-activation-environment
#      --systemd，带上 WAYLAND_DISPLAY / DISPLAY / XDG_CURRENT_DESKTOP / VIDOW_SOCKET
#      和用户 vidow.env{} 声明的那些（systemd 拉起来的应用才看得到 vidow）
#   3) vidow.env()：读 / 写 / 删，改完的子进程（exec-once / spawn）看得到
#   4) IPC 的 env / set-env / unset-env：账本报得出来，改完会再推一轮
#   5) exec-once：整个 vidow 生命周期只起一次 —— 配置重载不重来，改命令原文才重来
#   6) 两个开关：systemd_env = 0 不吸、systemd_env_export = 0 不推
#   7) 环境变量覆盖照旧：VIDOW_SYSTEMD_ENV=0
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-env-test
CFGDIR="$RUNDIR/home"
CFGDIRV="$CFGDIR/.config/vidow"
FAKEBIN="$RUNDIR/bin"
CALLS="$RUNDIR/calls.log"
LOG="$RUNDIR/log"
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

trap 'exit 143' TERM INT
trap cleanup EXIT

our_vidow_pids() {
	local p
	for p in /proc/[0-9]*; do
		[ -r "$p/cmdline" ] || continue
		grep -aq "build/vidow" "$p/cmdline" 2>/dev/null || continue
		[ -r "$p/environ" ] || continue
		grep -aqz "$RUNDIR" "$p/environ" 2>/dev/null || continue
		echo "${p#/proc/}"
	done
}

kill_stray_vidows() {
	local pid
	our_vidow_pids | while read -r pid; do
		kill -KILL "$pid" 2>/dev/null
	done
}

waitgone() {
	local i
	for i in $(seq 1 40); do
		[ -z "$(our_vidow_pids)" ] && return 0
		sleep 0.2
	done
	return 1
}

cleanup() {
	local pid
	our_vidow_pids | while read -r pid; do
		kill -TERM "$pid" 2>/dev/null
	done
	sleep 0.4
	kill_stray_vidows
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}

rm -rf "$RUNDIR"
mkdir -p "$CFGDIRV" "$RUNDIR/run" "$FAKEBIN"
chmod 700 "$RUNDIR/run"

# 假的 systemctl / dbus-update-activation-environment：只记「谁调了什么」，
# 顺便演一个「用户管理器里有这些变量」的会话。
cat > "$FAKEBIN/systemctl" <<EOF
#!/bin/bash
echo "systemctl \$*" >> "$CALLS"
if [ "\$1" = "--user" ] && [ "\$2" = "show-environment" ]; then
	echo "PATH=/usr/bin:/bin"
	echo "TEST_ABSORB=from-systemd"
	echo "TEST_ALREADY=from-systemd"
	echo "WLR_BACKENDS=nonsense"
	echo "WAYLAND_DISPLAY=host-wayland-1"
	echo "DISPLAY=:9"
	exit 0
fi
exit 0
EOF
cat > "$FAKEBIN/busctl" <<EOF
#!/bin/bash
echo "busctl \$*" >> "$CALLS"
exit 0
EOF
cat > "$FAKEBIN/dbus-update-activation-environment" <<EOF
#!/bin/bash
echo "dbus-update-activation-environment \$*" >> "$CALLS"
exit 0
EOF
chmod +x "$FAKEBIN/systemctl" "$FAKEBIN/busctl" \
	"$FAKEBIN/dbus-update-activation-environment"

export PATH="$FAKEBIN:$PATH"
export XDG_RUNTIME_DIR="$RUNDIR/run"
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_LIBINPUT_NO_DEVICES=1
export HOME="$CFGDIR" VIDOW_LOG=debug
export DISPLAY= WAYLAND_DISPLAY=
# 我们自己已有一个同名变量：它不许被 systemd 那份盖掉
export TEST_ALREADY=from-us

write_config() {
	cat > "$CFGDIRV/config.lua" <<EOF
vidow.env{ TEST_MINE = "mine", TEST_DROP = false }
vidow.exec_once("echo once:\$TEST_MINE:\$TEST_ABSORB:\$TEST_ALREADY:\$WAYLAND_DISPLAY:\$VIDOW_SOCKET >> $RUNDIR/once.log")
vidow.exec_once("echo second >> $RUNDIR/once.log")
EOF
}

start_vidow() {
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	for _ in $(seq 1 60); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		fail "vidow 没起来"
		grep -a "ERROR" "$LOG" | head -5
		exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
}

stop_vidow() {
	local pid
	our_vidow_pids | while read -r pid; do
		kill -TERM "$pid" 2>/dev/null
	done
	waitgone || kill_stray_vidows
}

waitline() { # waitline <文件> <行数> —— 等文件攒够这么多行
	local i
	for i in $(seq 1 40); do
		[ "$(wc -l < "$1" 2>/dev/null)" = "$2" ] && return 0
		sleep 0.1
	done
	return 1
}
exports_now() { ./build/vidowmsg env 2>/dev/null |
	python3 -c "import json,sys; print(json.load(sys.stdin)['exports'])" 2>/dev/null; }
env_field() { # env_field <python 表达式，vars 是变量表>
	./build/vidowmsg env 2>/dev/null | python3 -c "
import json, sys
d = json.load(sys.stdin)
vars = d['vars']
print($1)
" 2>/dev/null
}

# ------------------------------------------------------------ 1) 吸收
mark "1) 从 systemd 用户管理器吸（我们自己没有的才吸）"
: > "$CALLS"
write_config
start_vidow
waitline "$RUNDIR/once.log" 2 && pass "两条 exec-once 都起来了" \
	|| fail "exec-once 没起来：$(cat "$RUNDIR/once.log" 2>/dev/null | tr '\n' ' ')"
grep -aq "^once:mine:from-systemd:from-us:" "$RUNDIR/once.log" &&
	pass "子进程环境：吸来的 TEST_ABSORB + 我们自己的 TEST_ALREADY + vidow.env 设的 TEST_MINE" \
	|| fail "子进程环境不对：$(head -1 "$RUNDIR/once.log")"
grep -aq "环境：从 systemd 用户管理器吸了 1 个变量" "$LOG" &&
	pass "只吸了 1 个（WLR_* / WAYLAND_DISPLAY / DISPLAY 被挡，TEST_ALREADY 我们自己有）" \
	|| fail "吸的数量不对：$(grep -a '吸了' "$LOG" | head -2)"
grep -aq "环境：从 systemd 吸来 TEST_ABSORB=from-systemd" "$LOG" &&
	pass "吸了 TEST_ABSORB" || fail "TEST_ABSORB 没吸进来"
grep -aq "环境：从 systemd 吸来 WLR_BACKENDS" "$LOG" &&
	fail "WLR_* 不该吸（会把我们的后端选择搅乱）" || pass "WLR_* 不吸"
grep -aq "环境：从 systemd 吸来 WAYLAND_DISPLAY" "$LOG" &&
	fail "WAYLAND_DISPLAY 不该吸（那是宿主合成器的）" || pass "WAYLAND_DISPLAY / DISPLAY 不吸"

# ------------------------------------------------------------ 2) 推送
mark "2) 推给 systemd / dbus（systemd 拉起来的应用才看得到 vidow）"
grep -a "systemctl --user import-environment" "$CALLS" |
	grep -aq "WAYLAND_DISPLAY" && pass "import-environment 推了 WAYLAND_DISPLAY" \
	|| fail "import-environment 没推 WAYLAND_DISPLAY：$(grep -a import-environment "$CALLS")"
IMP=$(grep -a "systemctl --user import-environment" "$CALLS" | grep -a VIDOW_SOCKET | tail -1)
case "$IMP" in
*VIDOW_SOCKET*TEST_MINE*)
	pass "推的清单里有 VIDOW_SOCKET 和 vidow.env 声明的 TEST_MINE" ;;
*)
	fail "推的清单不对：$IMP" ;;
esac
IMP=$(grep -a "dbus-update-activation-environment" "$CALLS" | tail -1)
case "$IMP" in
*"--systemd"*VIDOW_SOCKET*)
	pass "dbus 激活环境也推了（--systemd + VIDOW_SOCKET）" ;;
*)
	fail "dbus-update-activation-environment 没跟上：$IMP" ;;
esac
grep -a "systemctl --user import-environment" "$CALLS" | grep -aq "DISPLAY" &&
	pass "X 起来之后 DISPLAY 也推了一轮" || fail "DISPLAY 没推"

# ------------------------------------------------------------ 3) vidow.env()
mark "3) vidow.env()：读 / 写 / 删"
[ "$(env_field "[v['value'] for v in vars if v['name']=='TEST_MINE'][0]")" = "mine" ] &&
	pass "账本里报得出来 TEST_MINE=mine" || fail "账本里没报 TEST_MINE"
[ "$(env_field "[v['value'] for v in vars if v['name']=='TEST_DROP'][0]")" = "None" ] &&
	pass "删掉的变量报 value = null" || fail "TEST_DROP 的账不对"
[ "$(env_field "[v['before'] for v in vars if v['name']=='TEST_MINE'][0]")" = "None" ] &&
	pass "记着「进来之前没有」（退出时要还原成没有）" || fail "没记原值"

# ------------------------------------------------------------ 4) IPC
mark "4) IPC：env / set-env / unset-env"
BEFORE=$(exports_now)
./build/vidowmsg --raw set-env TEST_LATE late 2>&1 | grep -aq '"value":"late"' &&
	pass "set-env 报成功" || fail "set-env 失败"
sleep 0.5
grep -a "import-environment" "$CALLS" | grep -aq "TEST_LATE" &&
	pass "改完立刻再推一轮（清单里有 TEST_LATE）" || fail "set-env 之后没推"
[ "$(exports_now)" -gt "$BEFORE" ] && pass "env 的 exports 计数涨了（$BEFORE → $(exports_now)）" \
	|| fail "exports 计数没动（$BEFORE → $(exports_now)）"
./build/vidowmsg action "spawn echo late:\$TEST_LATE >> $RUNDIR/late.log" >/dev/null 2>&1
sleep 0.6
[ "$(cat "$RUNDIR/late.log" 2>/dev/null)" = "late:late" ] &&
	pass "改完之后起的子进程看到了 TEST_LATE" || fail "子进程没看到 TEST_LATE：$(cat "$RUNDIR/late.log" 2>/dev/null)"
./build/vidowmsg --raw unset-env TEST_LATE >/dev/null 2>&1
sleep 0.3
[ "$(env_field "[v['value'] for v in vars if v['name']=='TEST_LATE'][0]")" = "None" ] &&
	pass "unset-env 之后账本里是 null" || fail "unset-env 没生效"
if ./build/vidowmsg set-env "1bad" x >/dev/null 2>"$RUNDIR/badname.err"; then
	fail "非法名字居然成功了"
else
	pass "名字不合法（1bad）当场报错"
fi
grep -aq "不像个环境变量名" "$RUNDIR/badname.err" &&
	pass "错误信息说清了为什么" || fail "错误信息没说清：$(cat "$RUNDIR/badname.err")"

# ------------------------------------------------------------ 5) exec-once
mark "5) exec-once：整个生命周期只起一次"
[ "$(wc -l < "$RUNDIR/once.log")" = "2" ] &&
	pass "开了这些窗口之后还是 2 行（只起一次）" || fail "once.log 被重复跑了：$(wc -l < "$RUNDIR/once.log") 行"
: > "$RUNDIR/once.log"
kill -HUP "$(our_vidow_pids | head -1)" 2>/dev/null
sleep 1.5
[ ! -s "$RUNDIR/once.log" ] &&
	pass "配置重载不重来（exec_once 记着命令原文）" || fail "重载之后又跑了一遍：$(cat "$RUNDIR/once.log")"
grep -aq "exec-once：起 " "$LOG" && grep -aq "exec-once：.*起过了，跳过" "$LOG" &&
	pass "日志里说清了「起过 / 跳过」" || fail "日志里没提 exec-once"
# 改命令原文 = 一条新命令：会再起一次
sed -i 's/echo second/echo second-v2/' "$CFGDIRV/config.lua"
sleep 1.5
waitline "$RUNDIR/once.log" 1 && grep -aq "second-v2" "$RUNDIR/once.log" &&
	pass "改了命令原文就是新命令（重载起了一次 second-v2）" \
	|| fail "改了原文没起：$(cat "$RUNDIR/once.log" 2>/dev/null | tr '\n' ' ')"

# ------------------------------------------- 5b) 配置里删掉的会「忘掉」
mark "5b) exec-once：配置里删掉的命令会被忘掉（写回来会再起一次）"
: > "$RUNDIR/once.log"
cat > "$CFGDIRV/config.lua" <<EOF
vidow.exec_once("echo third >> $RUNDIR/once.log")
EOF
waitline "$RUNDIR/once.log" 1
grep -aq "^third$" "$RUNDIR/once.log" && pass "换成一条没见过的命令：起了" \
	|| fail "新命令没起：$(tr '\n' ' ' < "$RUNDIR/once.log")"
# 攒着不忘的话，名单满了之后就记不住新命令了（那样每重载起一次，最难受）
grep -aq "exec-once：.*配置里没了，忘掉它" "$LOG" &&
	pass "日志里说清了「配置里没了 → 忘掉」" \
	|| fail "日志里没提忘掉：$(grep -a 'exec-once' "$LOG" | tail -3 | tr '\n' ' ')"
# 把最开始那条原样写回来：它已经被忘掉了，所以应该**再起一次**
cat > "$CFGDIRV/config.lua" <<EOF
vidow.exec_once("echo once:\$TEST_MINE:\$TEST_ABSORB:\$TEST_ALREADY:\$WAYLAND_DISPLAY:\$VIDOW_SOCKET >> $RUNDIR/once.log")
EOF
waitline "$RUNDIR/once.log" 2
grep -aq "^once:" "$RUNDIR/once.log" &&
	pass "写回来之后又起了一次（先忘了才可能再起）" \
	|| fail "写回来没起：$(tr '\n' ' ' < "$RUNDIR/once.log")"

# ------------------------------------------------------------ 6) 开关
mark "6) 两个开关（配置键 + 环境变量覆盖）"
stop_vidow
: > "$CALLS"
printf 'systemd_env = 0\nsystemd_env_export = 0\n' > "$CFGDIRV/extra.lua"
cat > "$CFGDIRV/config.lua" <<EOF
vidow.include("extra.lua")
vidow.exec_once("echo off:\$TEST_ABSORB >> $RUNDIR/off.log")
EOF
rm -f "$RUNDIR/off.log"
start_vidow
waitline "$RUNDIR/off.log" 1
grep -aq "^off:$" "$RUNDIR/off.log" &&
	pass "systemd_env = 0：TEST_ABSORB 没吸进来（子进程里是空的）" \
	|| fail "systemd_env = 0 还是吸了：$(cat "$RUNDIR/off.log")"
grep -aq "systemd_env = 0 —— 把启动时吸进来的 1 个变量还回去了" "$LOG" &&
	pass "日志里说清了「启动时吸了、配置说不要，于是还回去」" \
	|| fail "日志里没说明：$(grep -a '环境：' "$LOG" | head -3 | tr '\n' ' ')"
grep -a "systemctl --user import-environment" "$CALLS" | grep -aq "VIDOW_SOCKET" &&
	fail "systemd_env_export = 0 却还是推了：$(grep -a import-environment "$CALLS")" \
	|| pass "systemd_env_export = 0：没推（portal 自己那条不算）"
# 环境变量覆盖
stop_vidow
: > "$CALLS"
cat > "$CFGDIRV/config.lua" <<EOF
vidow.exec_once("echo covered >> $RUNDIR/covered.log")
EOF
rm -f "$RUNDIR/covered.log"
VIDOW_SYSTEMD_ENV=0 start_vidow
waitline "$RUNDIR/covered.log" 1
grep -aq "systemd_env = 0，不从用户管理器吸环境" "$LOG" &&
	pass "VIDOW_SYSTEMD_ENV=0 在配置跑之前就被认了（启动时就没吸）" \
	|| fail "环境变量覆盖没生效：$(grep -a '环境：' "$LOG" | head -3 | tr '\n' ' ')"

# ------------------------------------------------------------ 7) 收尾
mark "7) 退出时把用户管理器里改过的还回去"
./build/vidowmsg action quit >/dev/null 2>&1
waitgone && pass "退出干净" || { fail "进程还在"; our_vidow_pids; }
# 用户管理器里原来有值的（假的 systemctl 报的 host-wayland-1 / :9）→ set 回去；
# 原来没有的（VIDOW_SOCKET，也是我们推上去的）→ unset 掉
grep -a "systemctl --user set-environment" "$CALLS" | grep -aq "WAYLAND_DISPLAY=host-wayland-1" &&
	pass "退出把用户管理器里的 WAYLAND_DISPLAY 还原成原值（host-wayland-1，值里的引号被 sh 剥掉了）" \
	|| fail "退出没还原原值：$(grep -a set-environment "$CALLS" | tail -1)"
grep -a "systemctl --user unset-environment" "$CALLS" | grep -aq "VIDOW_SOCKET" &&
	pass "退出把我们推上去、用户管理器里本来没有的变量删掉（VIDOW_SOCKET）" \
	|| fail "退出没删 VIDOW_SOCKET：$(grep -a unset-environment "$CALLS" | tail -1)"
grep -a "busctl --user call" "$CALLS" | grep -aq "UnsetEnvironment as 1 VIDOW_SOCKET" &&
	pass "还原走的是两条路：systemctl 之外还有 busctl（嵌套/测试跑时只有它能连上）" \
	|| fail "还原没走 busctl：$(grep -a busctl "$CALLS" | tail -1)"
grep -aq "Assertion" "$LOG" && fail "退出时日志里有 assert" || pass "退出时没有 assert"

# ---------------------------------------- 8) 够不着用户管理器：整个跳过
# 嵌套跑（vidow 里再起一个 vidow）/ headless 测试时 XDG_RUNTIME_DIR 是临时的：
# systemctl --user 连不上，可 busctl / dbus-update-activation-environment 走的是会话
# 总线（DBUS_SESSION_BUS_ADDRESS），照样能改到**宿主会话**的用户管理器上 —— 推上去
# 的是嵌套里那个 WAYLAND_DISPLAY，退出还原还会把宿主本来就有、我们却以为「原来没有」
# 的变量 unset 掉（实测删掉过宿主 Hyprland 会话的 WAYLAND_DISPLAY / DISPLAY /
# XDG_CURRENT_DESKTOP）。够不着 = 我们不是这个会话的主人，那就不碰。
mark "8) systemctl --user 够不着用户管理器：不推也不还原（免得改脏宿主会话）"
: > "$CALLS"
cat > "$FAKEBIN/systemctl" <<EOF
#!/bin/bash
echo "systemctl \$*" >> "$CALLS"
exit 1
EOF
chmod +x "$FAKEBIN/systemctl"
cat > "$CFGDIRV/config.lua" <<EOF
vidow.env("TEST_GATE", "1")
EOF
start_vidow
grep -aq "够不着用户管理器" "$LOG" &&
	pass "日志说清了为什么跳过（够不着 = 我们不是这个会话的主人）" \
	|| fail "没说清：$(grep -a '环境：' "$LOG" | head -3 | tr '\n' ' ')"
# 注意：portalconf.c 自己那条 import-environment（XDG_CURRENT_DESKTOP
# WAYLAND_DISPLAY XDG_RUNTIME_DIR）不算 —— 那是 portal 后端要连宿主合成器时用的，
# 不是这一份的账。这一份的清单里有 VIDOW_SOCKET，认它就够了。
grep -a "import-environment" "$CALLS" | grep -aq "VIDOW_SOCKET" &&
	fail "够不着却还是 import 了：$(grep -a import-environment "$CALLS" | grep VIDOW_SOCKET | head -1)" \
	|| pass "没推（我们那份清单一次都没 import）"
# 读可以退到 busctl（读不改别人）；写不行 —— 那条走会话总线，会改到宿主会话上。
grep -aqE "^dbus-update-activation-environment|^busctl .*(Unset|Set)Environment" "$CALLS" &&
	fail "退到 busctl / dbus 的写那条路了 —— 那正是会把宿主会话改脏的一条：$(grep -aE '^dbus-update-activation-environment|^busctl .*(Unset|Set)Environment' "$CALLS" | head -1)" \
	|| pass "也没退到 busctl / dbus 的写那条路（会话总线是真的，那条会改到宿主上）"
./build/vidowmsg action quit >/dev/null 2>&1
waitgone && pass "退出干净" || fail "进程还在"
grep -aqE "unset-environment|UnsetEnvironment" "$CALLS" &&
	fail "退出还去还原了：$(grep -aE 'unset-environment|UnsetEnvironment' "$CALLS" | head -1)" \
	|| pass "退出也没去还原（本来就没改，不用还）"
grep -aq "Assertion" "$LOG" && fail "退出时日志里有 assert" || pass "退出时没有 assert"

echo
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / test-env.sh: OK"
else
	echo "有判据没过（见上面的清单）"
	exit 1
fi
