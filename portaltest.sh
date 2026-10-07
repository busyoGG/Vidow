#!/bin/bash
# portal 在 tty 上能不能测：先说结论，再逐项体检，最后发一个真请求。
#
# 背景（先看懂这一条，后面全是它的推论）：
#   portal 走的是**每个用户一条** session bus（/run/user/$UID/bus），跟你在哪个 tty
#   无关 —— 所以 tty3 上跑 vidow 一样能测 portal。真正的坑是「portal 现在指向谁」：
#   xdg-desktop-portal 前端和后端都是 **dbus 激活起来的常驻进程**，它们的
#   XDG_CURRENT_DESKTOP / WAYLAND_DISPLAY 来自 systemd 用户管理器，跟你在哪个 tty
#   起 vidow 没关系。你从 Hyprland 切到 tty3 的 vidow 之后，那些还活着的后端连的
#   仍是 Hyprland 的 wayland-1 —— 对话框就画到另一个会话里去了。
#
#   vidow 启动时会自己干两件事：setenv XDG_CURRENT_DESKTOP=Vidow，然后
#   `systemctl --user import-environment XDG_CURRENT_DESKTOP WAYLAND_DISPLAY
#    XDG_RUNTIME_DIR` + `systemctl --user restart xdg-desktop-portal`
#   （见 src/portalconf.c）。但**已经在跑的后端进程不会被重启** —— 这一条得手动收，
#   本脚本会点名收哪些。
#
# 用法：
#   ./portaltest.sh          体检 + 发一个真的文件选择器请求（需要 vidow 在跑）
#   ./portaltest.sh --check  只体检（不动你的屏幕）
#
# 产物：/tmp/vidow-portal-live/（探针输出 + 窗口列表快照）
set -u
cd "$(dirname "$0")"

R=/tmp/vidow-portal-live
CHECK_ONLY=0
[ "${1:-}" = "--check" ] && CHECK_ONLY=1
FAILED=0

pass() { echo "  PASS  $1"; }
fail() { echo "  FAIL  $1"; FAILED=1; }
info() { echo "        $1"; }
mark() { echo; echo "== $1 =="; }

rm -rf "$R"; mkdir -p "$R"

# ---------------------------------------------------------------- 体检

mark "① 会话总线（跟 tty 无关，每个用户一条）"

BUS="${DBUS_SESSION_BUS_ADDRESS:-}"
if [ -z "$BUS" ] && [ -S "/run/user/$(id -u)/bus" ]; then
	BUS="unix:path=/run/user/$(id -u)/bus"
fi
BUSPATH="${BUS#unix:path=}"
BUSPATH="${BUSPATH%%,*}"   # unix:path=/x,guid=... 后面那截不是路径
if [ -n "$BUS" ] && [ -S "$BUSPATH" ]; then
	pass "session bus 在（$BUSPATH）"
else
	fail "没有 session bus（$BUS）—— portal 起不来。在 tty 上要么用 dbus-run-session，要么 systemd 用户会话得在跑"
fi

mark "② vidow 在不在跑、它的 wayland socket 叫什么"

VWNAME=""
VSOCK=""
# vidow 的 IPC socket 叫 $XDG_RUNTIME_DIR/vidow-<WAYLAND_DISPLAY>.sock：不靠环境变量也
# 能找出来（tty 上的 shell 一般没设 WAYLAND_DISPLAY，vidowmsg 的默认路径会找不到）
FOUND=""
for s in "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"/vidow-wayland-*.sock; do
	[ -S "$s" ] || continue
	if ./build/vidowmsg --socket "$s" state > "$R/state.json" 2>/dev/null; then
		FOUND="$FOUND $s"
		VSOCK="$s"
	fi
done
FOUND=$(echo "$FOUND" | xargs 2>/dev/null || true)
if [ -n "$VSOCK" ]; then
	VWNAME=$(basename "$VSOCK" | sed 's/^vidow-//; s/\.sock$//')
	pass "vidow 在跑（IPC $VSOCK → 它的 WAYLAND_DISPLAY=$VWNAME）"
	if [ "$(echo "$FOUND" | wc -w)" -gt 1 ]; then
		fail "有不止一个 vidow 在跑（$FOUND）—— session bus 只有一条，portal 只能指向其中一个"
	fi
else
	fail "没找到在跑的 vidow（${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/vidow-wayland-*.sock 都没有活的）—— 先在本 tty 把 vidow 起起来"
fi
info "本 shell 的 WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-（没设）}"

mark "③ systemd 用户管理器里的会话环境（portal 进程真正读的那份）"

UENV=$(systemctl --user show-environment 2>/dev/null)
if [ -z "$UENV" ]; then
	info "这条总线上没有 systemd 用户管理器（私有总线 / dbus-run-session）—— 跳过本节"
	info "真实会话里这一节必须过：portal 后端的环境就是从这里来的"
	NO_SYSTEMD=1
fi
NO_SYSTEMD=${NO_SYSTEMD:-0}
UDESK=$(echo "$UENV" | sed -n 's/^XDG_CURRENT_DESKTOP=//p')
UWL=$(echo "$UENV" | sed -n 's/^WAYLAND_DISPLAY=//p')
info "XDG_CURRENT_DESKTOP=${UDESK:-（没设）}  WAYLAND_DISPLAY=${UWL:-（没设）}"
if [ "$NO_SYSTEMD" = 1 ]; then
	:
elif [ "${UDESK,,}" = "vidow" ]; then
	pass "用户管理器里的会话名已经是 Vidow（portal 会读 vidow-portals.conf）"
else
	fail "用户管理器里的会话名是「${UDESK:-（空）}」，不是 Vidow —— portal 读的是那个名字的 portals.conf"
fi
if [ "$NO_SYSTEMD" = 1 ]; then
	:
elif [ -n "$VWNAME" ] && [ "$UWL" = "$VWNAME" ]; then
	pass "用户管理器里的 WAYLAND_DISPLAY 就是 vidow 那个（$UWL）"
else
	fail "用户管理器里的 WAYLAND_DISPLAY=${UWL:-（空）}，vidow 的是 ${VWNAME:-（未知）} —— dbus 激活的后端会连到别的合成器上"
fi

mark "④ portal 前端 / 后端进程各自指向哪块屏"

NPORTAL=0
NVIDOW=0
STALE=""
for p in $(pgrep -f '/usr/lib/xdg-desktop-portal' 2>/dev/null); do
	[ "$p" = "$$" ] && continue
	cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | cut -c1-60)
	env=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null)
	# 只看**当前这条总线**上的 portal 进程：机器上可能同时开着别的会话
	# （比如宿主的 Hyprland），那些进程跟我们要测的这条链没关系。
	pbus=$(echo "$env" | sed -n 's/^DBUS_SESSION_BUS_ADDRESS=//p')
	if [ -n "$BUS" ] && [ "$pbus" != "$BUS" ]; then
		continue
	fi
	wl=$(echo "$env" | sed -n 's/^WAYLAND_DISPLAY=//p')
	dk=$(echo "$env" | sed -n 's/^XDG_CURRENT_DESKTOP=//p')
	NPORTAL=$((NPORTAL + 1))
	if [ -n "$VWNAME" ] && [ "$wl" = "$VWNAME" ]; then
		NVIDOW=$((NVIDOW + 1))
		info "  ✓ $p $cmd（WAYLAND_DISPLAY=$wl，DESKTOP=$dk）"
	else
		STALE="$STALE $p"
		info "  ✗ $p $cmd（WAYLAND_DISPLAY=${wl:-（空）}，DESKTOP=${dk:-（空）}）—— 连的不是 vidow"
	fi
done
if [ "$NPORTAL" -eq 0 ]; then
	info "（还没有 portal 进程在跑：第一次用到时会被 dbus 按当时的环境拉起来）"
elif [ "$NVIDOW" -eq "$NPORTAL" ]; then
	pass "全部 $NPORTAL 个 portal 进程都指向 vidow"
else
	fail "$((NPORTAL - NVIDOW))/$NPORTAL 个 portal 进程还连着别的合成器 —— 对话框会画到那边去"
	info "收掉它们（下次用到时 dbus 会按新环境重新拉起）："
	info "  pkill -f xdg-desktop-portal-gtk; pkill -f xdg-desktop-portal-kde; pkill -f xdg-desktop-portal-hyprland"
fi

mark "⑤ vidow 写出去的那份 portals.conf（xdg-desktop-portal 真正读的）"

PC="${XDG_CONFIG_HOME:-$HOME/.config}/xdg-desktop-portal/vidow-portals.conf"
if [ -f "$PC" ]; then
	pass "$PC 在（vidow 启动时同步的）"
	sed 's/^/        /' "$PC" | head -12
else
	fail "$PC 不存在 —— vidow 没同步过（看配置里的 portal_conf_sync / portal_desktop）"
fi

# ---------------------------------------------------------------- 真请求

if [ "$CHECK_ONLY" = 1 ]; then
	mark "只体检（--check），没发真请求"
else
	mark "⑥ 发一个真的文件选择器请求（走 portal，对话框会出现在你屏幕上）"

	if [ -z "$VWNAME" ]; then
		fail "vidow 没在跑，跳过真请求"
	else
		./build/vidowmsg --socket "$VSOCK" windows > "$R/windows-before.json" 2>/dev/null
		GTK_USE_PORTAL=1 WAYLAND_DISPLAY="$VWNAME" \
			setsid nohup python3 ./tools/portalprobe.py 25 \
			> "$R/probe.log" 2>&1 </dev/null &
		PROBE=$!
		# 等对话框（后端进程的窗口）出现在 vidow 的窗口列表里
		DLG=""
		for _ in $(seq 1 100); do
			./build/vidowmsg --socket "$VSOCK" windows > "$R/windows-after.json" 2>/dev/null
			DLG=$(python3 - "$R/windows-before.json" "$R/windows-after.json" <<'PY'
import json, sys
before = {w["id"] for w in json.load(open(sys.argv[1]))}
after = json.load(open(sys.argv[2]))
new = [w for w in after if w["id"] not in before]
dlg = [w for w in new
       if "xdg-desktop-portal" in (w.get("app_id") or "").lower()]
w = dlg[0] if dlg else None
if w is None:
    print("")
else:
    # 用 | 分隔：标题里有空格，用空白切会串位（踩过）
    print("%d|%s|%s|%s|%s" % (w["id"], w.get("app_id") or "?",
                              "focused" if w.get("focused") else "-",
                              w.get("parent") if w.get("parent") else "-",
                              (w.get("title") or "?")[:24]))
PY
)
			[ -n "$DLG" ] && break
			sleep 0.2
		done
		if [ -n "$DLG" ]; then
			IFS='|' read -r DID DAPP DFOCUS DPARENT DTITLE <<< "$DLG"
			pass "portal 后端把对话框画到 vidow 上了：id=$DID app_id=$DAPP 标题「$DTITLE」"
			# 父子关系和焦点都可能比「对话框出现」晚一步（xdg-foreign 的导入
			# 是另一次请求、焦点也可能下一帧才给），一起轮询到稳定。
			DPARENT="-"; FOCUSED=0
			for _ in $(seq 1 30); do
				read -r DPARENT FOCUSED <<< "$(./build/vidowmsg --socket "$VSOCK" windows 2>/dev/null |
					python3 -c 'import json,sys
w = [x for x in json.load(sys.stdin) if x["id"] == int(sys.argv[1])]
if w:
    print("%s %d" % (w[0].get("parent") or "-", 1 if w[0].get("focused") else 0))
else:
    print("- 0")' "$DID")"
				if [ "$DPARENT" != "-" ] && [ "$FOCUSED" = 1 ]; then
					break
				fi
				sleep 0.1
			done
			PWIN=$(python3 -c 'import json,sys
for w in json.load(open(sys.argv[1])):
    if w.get("title") == "portalprobe":
        print(w["id"]); break' "$R/windows-after.json")
			if [ -n "$PWIN" ] && [ "$DPARENT" = "$PWIN" ]; then
				pass "对话框挂在探针窗口上（xdg-foreign：parent=$DPARENT = 探针 $PWIN）"
			else
				fail "对话框没挂在探针窗口上（parent=$DPARENT，探针是 ${PWIN:-?}）—— 不会居中、不跟随父窗口"
			fi
			if [ "$FOCUSED" = 1 ]; then
				pass "对话框拿到了键盘焦点（xdg-activation）"
			else
				fail "对话框没拿到焦点（还得手点一下）"
			fi
		else
			fail "vidow 的窗口列表里一直没出现 portal 后端的对话框 —— 后端根本没连到 vidow 上"
		fi

		# 收掉对话框：给拿到焦点的那个客户端发一个 Esc（GTK 的文件选择器收到
		# Esc 就取消），这样连回程也验了 —— 请求 -> 对话框 -> 回应。
		./build/vidowmsg --socket "$VSOCK" send-key tap Escape >/dev/null 2>&1
		sleep 1.5

		# 等探针自己收（用户点取消 / 到点）
		for _ in $(seq 1 60); do
			kill -0 "$PROBE" 2>/dev/null || break
			sleep 0.5
		done
		kill "$PROBE" 2>/dev/null
		sed 's/^/        /' "$R/probe.log"
		if grep -aq "对话框返回" "$R/probe.log"; then
			pass "portal 请求走完了（探针拿到了对话框的回应）"
		else
			info "探针没等到回应（headless 里没人点「取消」；Esc 关对话框那条回程"
			info "是 portal 自己 D-Bus 上的事，已经有 ./test-portal.sh 用裸调用覆盖）"
		fi
	fi
fi

echo
echo "产物：$R（探针输出、窗口列表快照）"
if [ "$FAILED" = 0 ]; then
	echo "全部通过 / portaltest.sh: OK"
else
	echo "有判据没过（见上面的清单）"
fi
exit $FAILED
