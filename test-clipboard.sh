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

# xclip 和 wl-copy 都会 fork 一个「持有选择」的子进程，父进程立刻退出 —— 子进程被
# init 收养之后 is_ours 就认不出它了（踩过：早先那种「扫 /proc 找 pid 再杀」杀不掉它，
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
cat > "$CFGDIR/.config/vidow/config.lua" <<'EOF'
scale = "1"
xwayland = true
clipboard = true
clipboard_primary = true
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
import base64, json, sys
resp = json.loads(sys.argv[1])
try:
    print(eval(sys.argv[2], {"resp": resp, "json": json, "base64": base64}))
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

mark "H) 历史与文件复制"

stop_holder
jf '{"cmd":"set-config","key":"clipboard_history","value":"3"}' 'resp["ok"]' >/dev/null
jf '{"cmd":"clipboard-history-clear"}' 'resp["ok"]' >/dev/null
check "清空后 0 条" '{"cmd":"clipboard-history"}' 'resp["result"]["count"]' '0'
check "上限报 3" '{"cmd":"clipboard-history"}' 'resp["result"]["limit"]' '3'

H1="hist-from-x11-$$"
printf '%s' "$H1" > "$RUNDIR/in.txt"
start_holder "$RUNDIR/in.txt" xclip -selection clipboard -in
sleep 0.7
ck "X11 拷的这份进了历史" "$(clip_text)" "$H1"
check "历史 1 条" '{"cmd":"clipboard-history"}' 'resp["result"]["count"]' '1'
check "内容对" '{"cmd":"clipboard-history"}' \
	'resp["result"]["entries"][0]["text"]' "$H1"
check "来源记成 x11" '{"cmd":"clipboard-history"}' \
	'resp["result"]["entries"][0]["origin"]' 'x11'
check "是剪贴板不是选区" '{"cmd":"clipboard-history"}' \
	'resp["result"]["entries"][0]["selection"]' 'clipboard'
check "id 是正整数" '{"cmd":"clipboard-history"}' \
	'resp["result"]["entries"][0]["id"] > 0' 'True'
stop_holder

H2="hist-ipc-$$"
jf "{\"cmd\":\"clipboard-set\",\"text\":\"$H2\"}" 'resp["ok"]' >/dev/null
sleep 0.3
check "IPC 塞的也进历史（新的在最前）" '{"cmd":"clipboard-history"}' \
	'[e["text"] for e in resp["result"]["entries"]]' "['$H2', '$H1']"
ID_BEFORE=$(jf '{"cmd":"clipboard-history"}' 'resp["result"]["entries"][0]["id"]')
jf "{\"cmd\":\"clipboard-set\",\"text\":\"$H2\"}" 'resp["ok"]' >/dev/null
sleep 0.3
check "同样的内容再塞不新增（去重）" '{"cmd":"clipboard-history"}' \
	'resp["result"]["count"]' '2'
check "id 也没变（是挪到最前，不是新记一条）" '{"cmd":"clipboard-history"}' \
	'resp["result"]["entries"][0]["id"]' "$ID_BEFORE"

# 上限 3 条：塞第 4 份时把最旧的挤掉
H3="hist-three-$$"
H4="hist-four-$$"
jf "{\"cmd\":\"clipboard-set\",\"text\":\"$H3\"}" 'resp["ok"]' >/dev/null
jf "{\"cmd\":\"clipboard-set\",\"text\":\"$H4\"}" 'resp["ok"]' >/dev/null
sleep 0.3
check "到上限就不涨了" '{"cmd":"clipboard-history"}' 'resp["result"]["count"]' '3'
check "最旧的被淘汰" '{"cmd":"clipboard-history"}' \
	'[e["text"] for e in resp["result"]["entries"]]' \
	"['$H4', '$H3', '$H2']"

# 拿回来：按 index 选第 2 条（0 = 最新）
jf '{"cmd":"clipboard-history-use","index":1}' 'resp["ok"]' >/dev/null
sleep 0.3
ck "use 第 2 条后剪贴板是它" "$(clip_text)" "$H3"
check "用过的挪到最前（去重不新增）" '{"cmd":"clipboard-history"}' \
	'[e["text"] for e in resp["result"]["entries"]]' \
	"['$H3', '$H4', '$H2']"
check "还是 3 条" '{"cmd":"clipboard-history"}' 'resp["result"]["count"]' '3'

# 删
jf '{"cmd":"clipboard-history-delete","index":0}' 'resp["ok"]' >/dev/null
check "删掉最新一条后 2 条" '{"cmd":"clipboard-history"}' 'resp["result"]["count"]' '2'
check "删的是那一条" '{"cmd":"clipboard-history"}' \
	'[e["text"] for e in resp["result"]["entries"]]' "['$H4', '$H2']"
jf '{"cmd":"clipboard-history-delete","id":99999}' 'resp["ok"]' >/dev/null
check "删不存在的 id 不动历史" '{"cmd":"clipboard-history"}' 'resp["result"]["count"]' '2'
check "越界 index 报错" '{"cmd":"clipboard-history-delete","index":9}' 'resp["ok"]' 'False'

# 文件复制：file:// 的要连着 uri-list + gnome-copied-files 一起发
printf 'hello\n' > "$RUNDIR/f1.txt"
printf 'world\n' > "$RUNDIR/f2 空格.txt"
jf "{\"cmd\":\"clipboard-set-files\",\"files\":[\"$RUNDIR/f1.txt\",\"$RUNDIR/f2 空格.txt\"]}" \
	'resp["ok"]' >/dev/null
sleep 0.3
check "uri-list 两条" '{"cmd":"clipboard-get","mime":"text/uri-list"}' \
	'len(resp["result"]["text"].splitlines())' '2'
check "第一条 URI 对" '{"cmd":"clipboard-get","mime":"text/uri-list"}' \
	'resp["result"]["text"].splitlines()[0]' "file://$RUNDIR/f1.txt"
check "空格被百分号编码（按 RFC 3986，中文也是）" \
	'{"cmd":"clipboard-get","mime":"text/uri-list"}' \
	'resp["result"]["text"].splitlines()[1]' \
	"file://$RUNDIR/f2%20%E7%A9%BA%E6%A0%BC.txt"
check "gnome-copied-files 头是 copy" \
	'{"cmd":"clipboard-get","mime":"x-special/gnome-copied-files"}' \
	'base64.b64decode(resp["result"]["base64"]).decode().split("\n")[0]' 'copy'
check "gnome 第二行是第一条文件" \
	'{"cmd":"clipboard-get","mime":"x-special/gnome-copied-files"}' \
	'base64.b64decode(resp["result"]["base64"]).decode().split("\n")[1]' \
	"file://$RUNDIR/f1.txt"
check "默认取到的是路径文本（终端粘出来是路径）" '{"cmd":"clipboard-get"}' \
	'resp["result"]["text"]' "$(printf '%s\n%s\n' "$RUNDIR/f1.txt" "$RUNDIR/f2 空格.txt")"
check "历史里这条认成文件" '{"cmd":"clipboard-history"}' \
	'resp["result"]["entries"][0]["is_file"]' 'True'
check "历史里列出 2 个文件" '{"cmd":"clipboard-history"}' \
	'len(resp["result"]["entries"][0]["files"])' '2'
check "历史里文件 URI 对" '{"cmd":"clipboard-history"}' \
	'resp["result"]["entries"][0]["files"][0]' "file://$RUNDIR/f1.txt"
check "历史里也留着 gnome 那个类型（不然复制回来就不是文件了）" \
	'{"cmd":"clipboard-history"}' \
	'"x-special/gnome-copied-files" in resp["result"]["entries"][0]["mimes"]' 'True'
check "历史取全文拿得到 uri-list" \
	'{"cmd":"clipboard-history-get","index":0,"mime":"text/uri-list"}' \
	'resp["result"]["text"].splitlines()[0]' "file://$RUNDIR/f1.txt"
jf '{"cmd":"clipboard-history-use","index":0}' 'resp["ok"]' >/dev/null
sleep 0.3
check "文件那条复制回来还是文件（uri-list 还在）" \
	'{"cmd":"clipboard-get","mime":"text/uri-list"}' \
	'resp["result"]["text"].splitlines()[0]' "file://$RUNDIR/f1.txt"
check "gnome 那份也还在" \
	'{"cmd":"clipboard-get","mime":"x-special/gnome-copied-files"}' \
	'base64.b64decode(resp["result"]["base64"]).decode().split("\n")[0]' 'copy'

# 关掉历史 = 当场清空 + 不再记
jf '{"cmd":"set-config","key":"clipboard_history","value":"0"}' 'resp["ok"]' >/dev/null
sleep 0.3
check "clipboard_history=0 当场清空" '{"cmd":"clipboard-history"}' \
	'resp["result"]["count"]' '0'
check "报 enabled=false" '{"cmd":"clipboard-history"}' 'resp["result"]["enabled"]' 'False'
jf '{"cmd":"clipboard-set","text":"history-off-$$"}' 'resp["ok"]' >/dev/null
sleep 0.3
check "关掉之后不再记" '{"cmd":"clipboard-history"}' 'resp["result"]["count"]' '0'
jf '{"cmd":"set-config","key":"clipboard_history","value":"4"}' 'resp["ok"]' >/dev/null
sleep 0.3
jf '{"cmd":"clipboard-set","text":"history-on-$$"}' 'resp["ok"]' >/dev/null
sleep 0.3
check "打开之后又记了" '{"cmd":"clipboard-history"}' 'resp["result"]["count"]' '1'
check "上限跟着变" '{"cmd":"clipboard-history"}' 'resp["result"]["limit"]' '4'

# 事件（先清空，好让下面这条 push 的 count 是确定的 1）
jf '{"cmd":"clipboard-history-clear"}' 'resp["ok"]' >/dev/null
timeout 20 ./build/vidowmsg --socket "$SOCK" --raw watch clipboard-history-changed \
	> "$WATCH_FILE" 2>&1 &
WATCH_PID=$!
sleep 0.8
jf '{"cmd":"clipboard-set","text":"history-event-$$"}' 'resp["ok"]' >/dev/null
sleep 0.8
kill "$WATCH_PID" 2>/dev/null
WATCH_PID=""
ck_has "收到 clipboard-history-changed 事件" "$(cat "$WATCH_FILE" 2>/dev/null)" \
	'clipboard-history-changed'
# --raw 给的是紧凑 JSON（没空格），别拿 '"reason": "push"' 去 grep
hist_event_field() {
	python3 -c '
import json, sys
for line in open(sys.argv[1]):
    line = line.strip()
    if not line:
        continue
    try:
        ev = json.loads(line)
    except ValueError:
        continue
    if ev.get("event") == "clipboard-history-changed":
        print(ev.get("data", {}).get(sys.argv[2], ""))
        break' "$WATCH_FILE" "$1"
}
ck "事件里 reason=push" "$(hist_event_field reason)" 'push'
ck "事件里带条数" "$(hist_event_field count)" '1'

# GUI（GTK）那条 IPC 通路：不建窗口，只把命令走一遍
if python3 -c 'import gi' 2>/dev/null; then
	ck "vidowclip.py --check 走得通" \
		"$(python3 ./tools/vidowclip.py --check > "$RUNDIR/clipcheck.log" 2>&1; echo $?)" '0'
	ck_has "--check 报出了历史条数" "$(cat "$RUNDIR/clipcheck.log")" 'history'
else
	echo "  SKIP  vidowclip.py --check（这台没装 python3-gi）"
fi

mark "I) 退出与清理"

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
