#!/bin/bash
# 剪贴板桥（X11(Xwayland) <-> Wayland）：同步、缓存、INCR、PRIMARY、IPC、开关。
#
# 判据分七段：
#   A) 接管：起没起、拦的是不是我们、wlroots 那套有没有被晾在一边
#   B) X11 -> Wayland：焦点在 Wayland 窗口上照样同步（wlroots 那套在这里会拒），
#      而且 X11 那个程序退出之后两边都还能粘（内存里有一份）
#   C) Wayland -> X11：IPC 塞一份 / 真客户端 wl-copy，X11 都读得到；客户端退出
#      之后照样读得到（我们把缓存重新发布一份）
#   D) PRIMARY（选区）两个方向
#   E) 大块：200KB 两个方向都要完整（X 那边靠 INCR 分块，不是一次塞进属性）
#   F) 错误路径：没内容 / 参数乱写 / base64 坏 / 二进制 / 事件订阅
#   G) 开关：clipboard = 0 之后要真的退回 wlroots 那套（日志里能看到它自己的
#      selection 代码又动起来了），再打开要能恢复
#
# 全 headless，不碰宿主的 X / 合成器。X 客户端用 xclip（它自己会连上来把
# Xwayland 拉起来），Wayland 客户端用 wl-copy / wl-paste（它们要键盘能力，所以挂
# 一个 wtype 虚拟键盘）。大块数据走文件，不走命令行参数（200KB 塞进 argv 会 E2BIG）。
set -u
cd "$(dirname "$0")"

RUNDIR=/tmp/vidow-clip-test
CFGDIR="$RUNDIR/home"
LOG="$RUNDIR/log"
FAILED=0
VIDOW_PGID=""
VIDOW_PIDS=""
SOCK=""
JF="$RUNDIR/jf.py"
JF2="$RUNDIR/jf2.py"
REQ="$RUNDIR/req.json"
RESP="$RUNDIR/resp.json"
WATCH_PID=""
WATCH_FILE="$RUNDIR/watch.txt"
KEYBOARD_PID=""

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
mark() { echo; echo "== $1 =="; }

# 这个 pid 是不是**本测试起的**：沿父进程链往上找 $$（只看命令行会误伤外面那层 shell）。
is_ours() {
	local pid=$1
	local i
	for i in $(seq 1 10); do
		if [ "$pid" = "$$" ]; then return 0; fi
		if [ -z "$pid" ] || [ "$pid" = "0" ] || [ "$pid" = "1" ]; then return 1; fi
		pid=$(awk '/^PPid:/{print $2}' "/proc/$pid/status" 2>/dev/null)
	done
	return 1
}

# 杀掉我们起的某个客户端。**从大 pid 往小 pid 杀**：先杀父进程的话子进程会被
# init 收养，is_ours 就认不出它了（踩过：wl-copy fork 出来的那个一直活着，
# 「客户端退出」这一段就白测了）。
kill_prog() {
	local pat=$1
	local pids=""
	for p in /proc/[0-9]*; do
		local pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		local cmd
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*"$pat"*) is_ours "$pid" && pids="$pids $pid" ;;
		esac
	done
	[ -z "$pids" ] && return 1
	for pid in $(echo $pids | tr ' ' '\n' | sort -rn); do
		kill -9 "$pid" 2>/dev/null
	done
	return 0
}

# 等某个客户端真的没了（最多 4 秒）
wait_prog_gone() {
	local pat=$1
	for _ in $(seq 1 16); do
		if ! kill_prog "$pat" >/dev/null 2>&1; then
			return 0
		fi
		sleep 0.25
	done
	return 1
}

# xclip 和 wl-copy 都会 fork 一个「持有选择」的子进程，父进程立刻退出 —— 子进程被
# init 收养之后 is_ours 就认不出它了（踩过：kill_prog 杀不掉它，「客户端退出」那几条
# 断言其实一直在骗自己：客户端还活着，seat 上的选择当然没变）。所以：整组起
# （setsid 让它当组长），整组杀（kill -- -PGID）。
CLIENT_PGID=""

start_holder() { # start_holder <stdin 文件> <命令...>
	local input=$1
	shift
	setsid "$@" < "$input" &
	CLIENT_PGID=$!
	sleep 0.8
}

stop_holder() { # 真的把它整组杀干净才算成功
	[ -z "$CLIENT_PGID" ] && return 0
	kill -9 -- "-$CLIENT_PGID" 2>/dev/null
	local gone=0
	for _ in $(seq 1 16); do
		if ! kill -0 -- "-$CLIENT_PGID" 2>/dev/null; then
			gone=1
			break
		fi
		sleep 0.25
	done
	CLIENT_PGID=""
	[ "$gone" = "1" ]
}

stop_clients() {
	for p in /proc/[0-9]*; do
		pid=${p#/proc/}
		[ "$pid" = "$$" ] && continue
		cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null) || continue
		case "$cmd" in
		*xclip*|*wl-copy*|*wl-paste*|*wtype*|*kitty*|*vidowmsg*)
			is_ours "$pid" && kill -9 "$pid" 2>/dev/null ;;
		esac
	done 2>/dev/null
}

cleanup() {
	[ -n "$WATCH_PID" ] && kill "$WATCH_PID" 2>/dev/null
	[ -n "$KEYBOARD_PID" ] && kill -9 -- "-$KEYBOARD_PID" 2>/dev/null
	[ -n "$CLIENT_PGID" ] && kill -9 -- "-$CLIENT_PGID" 2>/dev/null
	stop_clients
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -TERM -- "-$pid" 2>/dev/null
	done
	sleep 0.4
	for pid in $VIDOW_PIDS; do
		[ -n "$pid" ] || continue
		kill -KILL -- "-$pid" 2>/dev/null
	done
	[ -n "${VIDOW_TEST_KEEP:-}" ] || rm -rf "$RUNDIR"
}
trap cleanup EXIT

for tool in xclip wl-copy wl-paste wtype kitty; do
	command -v "$tool" >/dev/null || {
		echo "test-clipboard.sh: 需要 $tool"
		exit 1
	}
done
[ -x ./build/vidowmsg ] || { echo "test-clipboard.sh: 先 ninja -C build"; exit 1; }

mkdir -p "$CFGDIR/.config/vidow" "$RUNDIR/run"
chmod 700 "$RUNDIR/run"
cat > "$CFGDIR/.config/vidow/config" <<'EOF'
scale = 1
xwayland = 1
clipboard = 1
clipboard_primary = 1
window_open_ms = 60
window_move_ms = 60
EOF

export XDG_RUNTIME_DIR="$RUNDIR/run"
export HOME="$CFGDIR" VIDOW_LOG=debug
export WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1
export DISPLAY= WAYLAND_DISPLAY=

# ------------------------------------------------------------------ 工具

# 小请求：请求走 argv，应答打 stdout（第三参数给了就顺手求值）。
cat > "$JF" <<'PY'
import json, socket, sys
path, req = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(20)
s.connect(path)
s.sendall((req + "\n").encode("utf-8"))
buf = b""
while b"\n" not in buf:
    chunk = s.recv(1 << 20)
    if not chunk:
        break
    buf += chunk
line = buf.split(b"\n")[0].decode("utf-8", "replace")
if len(sys.argv) > 3:
    resp = json.loads(line)
    try:
        print(eval(sys.argv[3], {"resp": resp, "json": json}))
    except Exception as exc:
        print("ERR:%s" % exc)
else:
    sys.stdout.write(line + "\n")
PY

# 大应答：请求从文件读，应答落文件（200KB 的剪贴板内容不能塞 argv）。
cat > "$JF2" <<'PY'
import socket, sys
path, req_file, resp_file = sys.argv[1], sys.argv[2], sys.argv[3]
req = open(req_file).read().strip()
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(30)
s.connect(path)
s.sendall((req + "\n").encode("utf-8"))
buf = b""
while b"\n" not in buf:
    chunk = s.recv(1 << 20)
    if not chunk:
        break
    buf += chunk
open(resp_file, "w").write(buf.split(b"\n")[0].decode("utf-8", "replace"))
PY

ipc() { python3 "$JF" "$SOCK" "$1"; }
jf() { python3 "$JF" "$SOCK" "$1" "$2"; }
eval_resp() { python3 -c '
import json, sys
resp = json.loads(sys.argv[1])
try:
    print(eval(sys.argv[2], {"resp": resp, "json": json}))
except Exception as exc:
    print("ERR:%s" % exc)' "$1" "$2"; }

# check <描述> <请求> <python 表达式> <期望>
check() {
	local desc="$1" req="$2" expr="$3" want="$4"
	local resp got
	resp=$(ipc "$req")
	got=$(eval_resp "$resp" "$expr")
	if [ "$got" = "$want" ]; then
		pass "$desc"
	else
		fail "$desc（得到 '$got'，期望 '$want'）"
		echo "     应答：$(printf '%s' "$resp" | head -c 300)"
	fi
}

# ck <描述> <实际> <期望>
ck() {
	if [ "$2" = "$3" ]; then
		pass "$1"
	else
		fail "$1（得到 '$2'，期望 '$3'）"
	fi
}

ck_has() {
	case "$2" in
	*"$3"*) pass "$1" ;;
	*) fail "$1（没找到 '$3'）" ;;
	esac
}

# 剪贴板内容：大块也不怕（应答走文件）
clip_fetch() {
	printf '{"cmd":"clipboard-get","selection":"%s"}' "${1:-clipboard}" > "$REQ"
	python3 "$JF2" "$SOCK" "$REQ" "$RESP"
}
clip_text() {
	clip_fetch "${1:-clipboard}"
	python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print("")
    sys.exit(0)
print(d.get("result", {}).get("text", ""))' "$RESP"
}
clip_size() {
	clip_fetch "${1:-clipboard}"
	python3 -c '
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print("")
    sys.exit(0)
print(d.get("result", {}).get("size", ""))' "$RESP"
}

since() { tail -n +"$(( $1 + 1 ))" "$LOG"; }
log_lines() { wc -l < "$LOG"; }

start_vidow() {
	rm -f "$LOG"
	setsid ./build/vidow > "$LOG" 2>&1 &
	VIDOW_PGID=$!
	VIDOW_PIDS="$VIDOW_PIDS $VIDOW_PGID"
	for _ in $(seq 1 40); do
		grep -aq "vidow ready" "$LOG" && break
		sleep 0.25
	done
	if ! grep -aq "vidow ready" "$LOG"; then
		fail "vidow 没起来"; grep -a "ERROR" "$LOG" | head -5; exit 1
	fi
	export WAYLAND_DISPLAY=$(grep -ao "WAYLAND_DISPLAY=[a-z0-9-]*" "$LOG" |
		tail -1 | cut -d= -f2)
	export DISPLAY=$(grep -ao "DISPLAY=:[0-9]*" "$LOG" | tail -1 | cut -d= -f2)
	SOCK="$XDG_RUNTIME_DIR/vidow-$WAYLAND_DISPLAY.sock"
	for _ in $(seq 1 20); do
		[ -S "$SOCK" ] && break
		sleep 0.1
	done
}

# 等日志里出现某个字符串（最多 8 秒）
wait_log() {
	local pat=$1
	for _ in $(seq 1 32); do
		grep -aq "$pat" "$LOG" && return 0
		sleep 0.25
	done
	return 1
}

# ------------------------------------------------------------------ 起步

echo "== test-clipboard.sh：X11(Xwayland) <-> Wayland 剪贴板桥 =="
start_vidow
echo "   WAYLAND_DISPLAY=$WAYLAND_DISPLAY DISPLAY=$DISPLAY"

# 先把 Xwayland 拉起来：xclip 一连上去它才起（懒启动），「接管」就发生在那时候。
timeout 5 xclip -selection clipboard -o >/dev/null 2>&1
if ! wait_log "接管 X11"; then
	fail "Xwayland 没起来 / 没接管"
fi

# Wayland 窗口（焦点要落在它上面，才叫「没有 X11 窗口聚焦」）
kitty --title wlterm > "$RUNDIR/kitty.log" 2>&1 &
for _ in $(seq 1 32); do
	[ "$(jf '{"cmd":"windows"}' \
		'len([w for w in resp["result"] if w["title"] == "wlterm"])')" = "1" ] && break
	sleep 0.25
done
# 虚拟键盘：wl-clipboard 要键盘能力才肯 set_selection。整组起，cleanup 好收。
setsid bash -c 'sleep 600 | wtype -' > "$RUNDIR/wtype.log" 2>&1 &
KEYBOARD_PID=$!
sleep 1.5

mark "A) 接管"

ck_has "启动日志说明接管了" "$(cat "$LOG")" "接管 X11(Xwayland) <-> Wayland 剪贴板"
ck_has "识别出 xwm 的两个代理窗口" "$(cat "$LOG")" "xwm 代理窗口"
check "IPC 报 enabled/bridging" '{"cmd":"clipboard"}' \
	'resp["result"]["enabled"] and resp["result"]["clipboard"]["bridging"]' 'True'
check "还没人拷贝时 origin=none" '{"cmd":"clipboard"}' \
	'resp["result"]["clipboard"]["origin"]' 'none'
check "配置里 primary 也开着" '{"cmd":"clipboard"}' \
	'resp["result"]["primary_enabled"]' 'True'
check "Xwayland 在跑" '{"cmd":"xwayland"}' 'resp["result"]["running"]' 'True'
check "Wayland 窗口在（kind=xdg）" '{"cmd":"windows"}' \
	'len([w for w in resp["result"] if w["kind"] == "xdg" and w["title"] == "wlterm"])' '1'

jf '{"cmd":"focus-window","title":"wlterm"}' 'resp["ok"]' >/dev/null
sleep 0.6
check "焦点在 Wayland 窗口上（wlroots 那套在这种状态下会拒绝同步）" \
	'{"cmd":"focused"}' 'resp["result"]["window"]["kind"]' 'xdg'

mark "B) X11 -> Wayland"

B1="hello-from-x11-$$"
printf '%s' "$B1" > "$RUNDIR/in.txt"
start_holder "$RUNDIR/in.txt" xclip -selection clipboard -in
ck "X11 拷的内容过桥了" "$(clip_text)" "$B1"
ck "字节数对得上" "$(clip_size)" "${#B1}"
check "类型是 UTF-8 文本" '{"cmd":"clipboard-get"}' 'resp["result"]["mime"]' \
	'text/plain;charset=utf-8'
check "状态里 origin=x11" '{"cmd":"clipboard"}' \
	'resp["result"]["clipboard"]["origin"]' 'x11'
check "X 侧 owner 是 xwm 的代理窗口（请求因此归我们答）" '{"cmd":"clipboard"}' \
	'resp["result"]["clipboard"]["x_owner_is_xwm_proxy"]' 'True'
check "类型取全了" '{"cmd":"clipboard"}' \
	'resp["result"]["clipboard"]["types_complete"] >= 1' 'True'
ck_has "日志记了取到几个类型" "$(cat "$LOG")" "从 X11 取到 clipboard 的"

# 真 Wayland 客户端也读得到（wl-paste）
ck "wl-paste（真 Wayland 客户端）读得到" "$(timeout 4 wl-paste 2>/dev/null)" "$B1"

# X11 那个程序退出之后
if stop_holder; then
	pass "X11 那个程序真的退干净了"
else
	fail "X11 那个程序没退干净"
fi
sleep 0.8
ck "X11 程序退出后 Wayland 侧还能粘" "$(clip_text)" "$B1"
ck "X11 程序退出后 X11 侧还能粘（从我们的内存里给）" \
	"$(timeout 4 xclip -selection clipboard -o 2>/dev/null)" "$B1"
ck "X11 程序退出后 wl-paste 还能粘" "$(timeout 4 wl-paste 2>/dev/null)" "$B1"

mark "C) Wayland -> X11"

C1="hello-from-wayland-$$"
check "IPC 塞一份进剪贴板" \
	"{\"cmd\":\"clipboard-set\",\"text\":\"$C1\"}" 'resp["ok"]' 'True'
sleep 0.8
ck "X11 客户端读到了 IPC 塞的那份" \
	"$(timeout 4 xclip -selection clipboard -o 2>/dev/null)" "$C1"

C2="typed-from-wayland-$$"
check "IPC 塞一份带类型的" \
	"{\"cmd\":\"clipboard-set\",\"mime\":\"text/plain\",\"text\":\"$C2\"}" \
	'resp["ok"]' 'True'
sleep 0.8
ck "按类型问也对（xclip -t text/plain）" \
	"$(timeout 4 xclip -selection clipboard -t text/plain -o 2>/dev/null)" "$C2"

# 真 Wayland 客户端（wl-copy）：它的数据会被我们「拉」进内存
C3="wlcopy-$$"
printf '%s' "$C3" > "$RUNDIR/in.txt"
start_holder "$RUNDIR/in.txt" wl-copy
ck "wl-copy 拷的内容 X11 读得到" \
	"$(timeout 4 xclip -selection clipboard -o 2>/dev/null)" "$C3"
check "状态里 origin=wayland" '{"cmd":"clipboard"}' \
	'resp["result"]["clipboard"]["origin"]' 'wayland'
check "wl-copy 给的类型不止一个" '{"cmd":"clipboard"}' \
	'resp["result"]["clipboard"]["types"] > 1' 'True'
ck "IPC 也读得到同一份" "$(clip_text)" "$C3"

if stop_holder; then
	pass "wl-copy 整组都退干净了（下面才算「客户端退出」）"
else
	fail "wl-copy 没退干净"
fi
sleep 1.0
ck "Wayland 客户端退出后 X11 还能粘" \
	"$(timeout 4 xclip -selection clipboard -o 2>/dev/null)" "$C3"
ck "Wayland 客户端退出后 wl-paste 还能粘（我们把缓存重发了一份）" \
	"$(timeout 4 wl-paste 2>/dev/null)" "$C3"
check "重发之后 publishing=true" '{"cmd":"clipboard"}' \
	'resp["result"]["clipboard"]["publishing"]' 'True'

mark "D) PRIMARY（选区）"

D1="primary-from-x11-$$"
printf '%s' "$D1" > "$RUNDIR/in.txt"
start_holder "$RUNDIR/in.txt" xclip -selection primary -in
ck "X11 选区 -> Wayland" "$(clip_text primary)" "$D1"
check "IPC 报的是 primary" '{"cmd":"clipboard-get","selection":"primary"}' \
	'resp["result"]["selection"]' 'primary'

D2="primary-from-wayland-$$"
check "IPC 塞一份进选区" \
	"{\"cmd\":\"clipboard-set\",\"selection\":\"primary\",\"text\":\"$D2\"}" \
	'resp["ok"]' 'True'
sleep 0.8
ck "Wayland 选区 -> X11" \
	"$(timeout 4 xclip -selection primary -o 2>/dev/null)" "$D2"

mark "E) 大块（INCR）"

python3 -c "import sys; sys.stdout.write('X'*199990 + 'TAIL')" > "$RUNDIR/big.txt"
BIG_SIZE=$(wc -c < "$RUNDIR/big.txt")
start_holder "$RUNDIR/big.txt" xclip -selection clipboard -in
sleep 1.5
ck "200KB X11 -> Wayland 字节数" "$(clip_size)" "$BIG_SIZE"
BIG_TEXT=$(clip_text)
ck "开头没坏" "$(printf '%s' "$BIG_TEXT" | cut -c1-8)" "XXXXXXXX"
ck "结尾没坏" "$(printf '%s' "$BIG_TEXT" | rev | cut -c1-4 | rev)" "TAIL"

# Wayland -> X11 的大块：用真客户端（wl-copy 从文件读，不走 argv）
stop_holder
python3 -c "import sys; sys.stdout.write('Y'*149996 + 'END')" > "$RUNDIR/big2.txt"
start_holder "$RUNDIR/big2.txt" wl-copy
sleep 1.5
ck "150KB Wayland -> X11 字节数" \
	"$(timeout 8 xclip -selection clipboard -o 2>/dev/null | wc -c)" "149999"
ck "150KB 内容结尾对得上" \
	"$(timeout 8 xclip -selection clipboard -o 2>/dev/null | tail -c 3)" "END"
stop_holder

mark "F) 错误路径与事件"

check "清空剪贴板" '{"cmd":"clipboard-clear"}' 'resp["ok"]' 'True'
check "清空之后拿不到" '{"cmd":"clipboard-get"}' 'resp["ok"]' 'False'
check "selection 乱写要报错" '{"cmd":"clipboard-get","selection":"nope"}' \
	'resp["ok"]' 'False'
check "clipboard-set 既没 text 也没 base64" '{"cmd":"clipboard-set"}' \
	'resp["ok"]' 'False'
check "base64 坏的" '{"cmd":"clipboard-set","base64":"!!!!"}' \
	'resp["ok"]' 'False'
check "clipboard-set 空文本可以" \
	'{"cmd":"clipboard-set","text":""}' 'resp["ok"]' 'True'

# 二进制（含 0 字节）走 base64
BIN_B64=$(printf 'a\0b' | base64 -w0)
check "二进制也能塞" \
	"{\"cmd\":\"clipboard-set\",\"mime\":\"application/octet-stream\",\"base64\":\"$BIN_B64\"}" \
	'resp["ok"]' 'True'
sleep 0.8
check "TARGETS 里有这个类型" '{"cmd":"clipboard"}' \
	"'application/octet-stream' in [t['mime'] for t in resp['result']['clipboard']['types_list']]" \
	'True'
ck "二进制到 X11 是 3 字节" \
	"$(timeout 4 xclip -selection clipboard -t application/octet-stream -o 2>/dev/null | wc -c)" "3"
check "二进制回来也是 3 字节" '{"cmd":"clipboard-get"}' \
	'resp["result"]["size"]' '3'

# 事件订阅
timeout 25 ./build/vidowmsg --socket "$SOCK" --raw watch > "$WATCH_FILE" 2>&1 &
WATCH_PID=$!
sleep 1.0
E1="event-$$"
printf '%s' "$E1" > "$RUNDIR/in.txt"
start_holder "$RUNDIR/in.txt" xclip -selection clipboard -in
sleep 1.2
ck_has "收到 clipboard-changed 事件" "$(cat "$WATCH_FILE" 2>/dev/null)" \
	'"event":"clipboard-changed"'
ck_has "事件里带着内容字节数" "$(cat "$WATCH_FILE" 2>/dev/null)" '"bytes":'
kill "$WATCH_PID" 2>/dev/null
WATCH_PID=""

mark "G) 开关（配置热切换）"

OFF_LINE=$(log_lines)
check "运行时关掉剪贴板" '{"cmd":"set-config","key":"clipboard","value":"0"}' \
	'resp["ok"]' 'True'
sleep 0.6
ck_has "日志说明交出了剪贴板" "$(since "$OFF_LINE")" "剪贴板: 关了，交出剪贴板"
check "关掉后 IPC 报 enabled=false" '{"cmd":"clipboard"}' \
	'resp["result"]["enabled"]' 'False'
check "关掉后拿不到内容" '{"cmd":"clipboard-get"}' 'resp["ok"]' 'False'

# 关掉之后退回 wlroots 那套：它自己的 selection 代码又动起来了
G1="disabled-$$"
printf '%s' "$G1" > "$RUNDIR/in.txt"
start_holder "$RUNDIR/in.txt" xclip -selection clipboard -in
sleep 1.2
ck_has "wlroots 那套接手了（日志里能看到它自己处理 XFixes）" \
	"$(since "$OFF_LINE")" "xwayland/selection/incoming.c"

check "再打开" '{"cmd":"set-config","key":"clipboard","value":"1"}' \
	'resp["ok"]' 'True'
sleep 0.6
stop_holder
G2="enabled-again-$$"
printf '%s' "$G2" > "$RUNDIR/in.txt"
start_holder "$RUNDIR/in.txt" xclip -selection clipboard -in
sleep 1.2
ck "打开后又能同步了" "$(clip_text)" "$G2"

check "运行时关掉 primary" \
	'{"cmd":"set-config","key":"clipboard_primary","value":"0"}' \
	'resp["ok"]' 'True'
sleep 0.5
check "primary 关掉后 IPC 报错" '{"cmd":"clipboard-get","selection":"primary"}' \
	'resp["ok"]' 'False'
check "primary 关掉后状态里也标着" '{"cmd":"clipboard"}' \
	'resp["result"]["primary_enabled"]' 'False'
check "再把 primary 打开" \
	'{"cmd":"set-config","key":"clipboard_primary","value":"1"}' \
	'resp["ok"]' 'True'
sleep 0.5
stop_holder
G3="primary-again-$$"
printf '%s' "$G3" > "$RUNDIR/in.txt"
start_holder "$RUNDIR/in.txt" xclip -selection primary -in
sleep 0.7
ck "primary 又能同步了" "$(clip_text primary)" "$G3"

mark "H) 退出与清理"

check "quit 干净退出" '{"cmd":"quit"}' 'resp["ok"]' 'True'
sleep 1.5
if kill -0 "$VIDOW_PGID" 2>/dev/null; then
	fail "vidow 没退出"
	kill -9 -- "-$VIDOW_PGID" 2>/dev/null
else
	wait "$VIDOW_PGID" 2>/dev/null
	STATUS=$?
	if [ "$STATUS" = "0" ]; then
		pass "vidow 干净退出（退出码 0：X11 窗口还开着也不崩）"
	else
		fail "vidow 退出码 $STATUS（139=段错误 / 134=断言）"
	fi
fi
if grep -aq "Assertion\|SIGABRT\|core dump" "$LOG"; then
	fail "退出路上有断言/崩溃"
else
	pass "退出路上没有断言/崩溃"
fi

echo
if [ "$FAILED" = "0" ]; then
	echo "全部通过"
else
	echo "有失败项"
fi
exit "$FAILED"
