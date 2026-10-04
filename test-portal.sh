#!/usr/bin/env bash
# 在私有 dbus session 里跑一份 xdg-desktop-portal + GTK/KDE 后端，让它们连到
# vidow（而不是宿主合成器），再发真实的 FileChooser 请求。
#
# 用法: ./test-portal.sh [switch-gtk|switch-kde|raw-gtk|raw-kde]
#   switch-gtk/kde  走 vidow 的生成流程，验证「写一行就换文件选择器」这条链：
#                     ~/.config/vidow/portals.conf
#                       → vidow 生成
#                     $XDG_CONFIG_HOME/xdg-desktop-portal/vidow-portals.conf
#                       → xdg-desktop-portal 读
#                     选中对应后端
#   raw-gtk/kde     让 vidow 别碰 portal 配置（portal_desktop=），手写一份，
#                   单独验证协议链路 —— 对应「嵌套在别的桌面里跑」那种部署
set -u

MODE="${1:-switch-gtk}"
RUNDIR=/tmp/vidow-run
CFG=/tmp/vidow-portal-cfg
LOG=/tmp/vidow-portal-test

mkdir -p "$RUNDIR" "$LOG"
export LOG RUNDIR CFG
rm -f "$RUNDIR"/wayland-*

case "$MODE" in
switch-*)
	WANT="${MODE#switch-}"
	export HOME="$CFG/home"
	export XDG_CONFIG_HOME="$HOME/.config"
	mkdir -p "$HOME/.config/vidow"
	# vidow 自己的配置：这里不设 portal_desktop，走默认的 Vidow
	rm -f "$HOME/.config/vidow/config"
	cat >"$HOME/.config/vidow/portals.conf" <<EOF
[preferred]
# 换文件选择器就这一行
org.freedesktop.impl.portal.FileChooser=$WANT
EOF
	;;
raw-*)
	WANT="${MODE#raw-}"
	export HOME="$CFG/home"
	export XDG_CONFIG_HOME="$HOME/.config"
	mkdir -p "$HOME/.config/vidow" "$XDG_CONFIG_HOME/xdg-desktop-portal"
	rm -f "$XDG_CONFIG_HOME/xdg-desktop-portal/"*-portals.conf
	# vidow 别碰 portal 配置，这份手写的才是唯一真相
	cat >"$HOME/.config/vidow/config" <<'EOF'
portal_desktop=
EOF
	rm -f "$HOME/.config/vidow/portals.conf"
	cat >"$XDG_CONFIG_HOME/xdg-desktop-portal/vidow-portals.conf" <<EOF
[preferred]
default=gtk
org.freedesktop.impl.portal.FileChooser=$WANT
EOF
	;;
*)
	echo "未知模式: $MODE"; exit 1
	;;
esac

# ---- 1. headless vidow -------------------------------------------------
XDG_RUNTIME_DIR="$RUNDIR" \
WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 WLR_RENDERER=pixman \
VIDOW_LOG=debug VIDOW_PORTAL_NO_RESTART=1 \
	./build/vidow >"$LOG/vidow.log" 2>&1 &
VIDOW_PID=$!
sleep 1.5
if ! grep -aq "vidow ready" "$LOG/vidow.log"; then
	echo "vidow 起不来"; kill $VIDOW_PID 2>/dev/null; exit 1
fi
export XDG_RUNTIME_DIR="$RUNDIR"
export WAYLAND_DISPLAY=wayland-0

# ---- 2. 配置链路：vidow 有没有把 portals.conf 生成出来 ------------------
case "$MODE" in
switch-*)
	GEN="$XDG_CONFIG_HOME/xdg-desktop-portal/vidow-portals.conf"
	echo "=== vidow 那边发生了什么 ==="
	grep -a "portal: 应用用户覆盖\|portal: 写入" "$LOG/vidow.log" | sed 's/^[0-9:.]* //'
	echo "=== $GEN ==="
	if [ -f "$GEN" ]; then
		cat "$GEN"
	else
		echo "没生成 —— 下面 portal 会读不到东西"
	fi
	;;
esac

# ---- 3. 一个真实的 xdg-foreign 导出端，给 portal 当 parent_window ------
./build/foreign >"$LOG/foreign.out" 2>&1 &
FOREIGN_PID=$!
sleep 2
H_V1=$(grep -m1 '^V1=' "$LOG/foreign.out" | cut -d= -f2)
H_V2=$(grep -m1 '^V2=' "$LOG/foreign.out" | cut -d= -f2)
echo "=== foreign 导出端: $(head -1 "$LOG/foreign.out") ==="

# 后端是被 dbus 激活起来的，继承的是 dbus-daemon 的环境变量，所以这些必须在
# dbus-run-session 之前 export 好。注意别设 XDG_DESKTOP_PORTAL_DIR：一旦设了，
# portal 就会去那个目录找 portals.conf，而不是 $XDG_CONFIG_HOME。
export XDG_CURRENT_DESKTOP=Vidow
export G_MESSAGES_DEBUG="xdg-desktop-portal"
export H_V1 H_V2

# ---- 4. 私有 dbus + portal + 三个真实请求 ------------------------------
cat >"$LOG/inner.sh" <<'INNER'
set -u
/usr/lib/xdg-desktop-portal >"$LOG/xdp.log" 2>&1 &
XDP_PID=$!
sleep 2

kill -0 $XDP_PID 2>/dev/null && echo "前端 pid=$XDP_PID 活着" || echo "前端已退出"

echo "--- 读到的 portals.conf ---"
grep -a "Looking for portals configuration" "$LOG/xdp.log" | sed 's/.*: //'
echo "--- FileChooser 选了谁 ---"
grep -a "Using .* for org.freedesktop.impl.portal.FileChooser" "$LOG/xdp.log" | tail -1

echo "--- FileChooser 版本 ---"
gdbus call --session --dest org.freedesktop.portal.Desktop \
  --object-path /org/freedesktop/portal/desktop \
  --method org.freedesktop.DBus.Properties.Get \
  org.freedesktop.portal.FileChooser version 2>&1 | head -1

echo "--- 请求 1: 不带 parent_window ---"
gdbus call --session --dest org.freedesktop.portal.Desktop \
  --object-path /org/freedesktop/portal/desktop \
  --method org.freedesktop.portal.FileChooser.OpenFile \
  "" "vidow portal test" "{}" 2>&1 | tail -1
sleep 5

if [ -n "${H_V1:-}" ]; then
  echo "--- 请求 2: parent_window=wayland:$H_V1 (v1 导出的 handle) ---"
  gdbus call --session --dest org.freedesktop.portal.Desktop \
    --object-path /org/freedesktop/portal/desktop \
    --method org.freedesktop.portal.FileChooser.OpenFile \
    "wayland:$H_V1" "vidow parent test" "{}" 2>&1 | tail -1
  sleep 5
fi

if [ -n "${H_V2:-}" ]; then
  echo "--- 请求 3: parent_window=wayland:$H_V2 (v2 导出的 handle, 跨版本) ---"
  gdbus call --session --dest org.freedesktop.portal.Desktop \
    --object-path /org/freedesktop/portal/desktop \
    --method org.freedesktop.portal.FileChooser.OpenFile \
    "wayland:$H_V2" "vidow cross version" "{}" 2>&1 | tail -1
  sleep 5
fi

kill $XDP_PID 2>/dev/null

# 被 dbus 激活起来的东西（后端、KIO worker……）不会跟着前端一起退，而它们继承了
# 我们的 stdout，dbus-run-session 的管道就永远等不到 EOF（表现为整个脚本挂死）。
# 判据是「环境变量里的 bus 地址就是这条私有总线」—— 宿主 session 里那些真 portal
# 的地址是 /run/user/1000/bus，不会被误伤。
for p in /proc/[0-9]*; do
  pid=${p#/proc/}
  [ "$pid" = "$$" ] && continue
  [ "$pid" = "$PPID" ] && continue
  [ "$(cat "$p/comm" 2>/dev/null)" = "dbus-daemon" ] && continue
  if tr '\0' '\n' < "$p/environ" 2>/dev/null \
      | grep -qF "${DBUS_SESSION_BUS_ADDRESS:-@}"; then
    kill "$pid" 2>/dev/null
  fi
done
INNER

timeout 90 dbus-run-session -- bash "$LOG/inner.sh" 2>&1 \
	| grep -avE "^dbus-daemon|GLib-GIO-DEBUG|Activating service|Successfully activated|dconf-DEBUG|glycin|gufo|gvfsd"

echo
echo "=== vidow 侧证据 ==="
grep -a -E "map |im focus -> 0x|portal: 父子" "$LOG/vidow.log" \
	| sed 's/^[0-9:.]* //' | tail -12
echo "=== vidow 里 map 了几个窗口: $(grep -ac 'map ' "$LOG/vidow.log") ==="
echo "=== 成功挂到应用窗口上的对话框: $(grep -ac '父子 .* -> foreign-app' "$LOG/vidow.log") 个 ==="
echo "=== 合成器有没有 abort: $(grep -aicE 'Assertion|Aborted' "$LOG/vidow.log") ==="

kill $FOREIGN_PID 2>/dev/null
kill $VIDOW_PID 2>/dev/null
wait $VIDOW_PID 2>/dev/null
exit 0
