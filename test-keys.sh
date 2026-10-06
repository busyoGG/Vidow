#!/bin/bash
# 用 wtype 通过虚拟键盘给嵌套的 vidow 注入按键，验证 niri 式焦点/滚动/改宽。
#
# 手动脚本（要先用 ./run-nested.sh 起一个嵌套会话）。**键位来自配置**：合成器
# 一个键位都不内置，这里按的 Mod+H/J/K/L/R 是 configs/14-keys.lua 里默认那套给的
# —— 自己删过绑定的话，这个脚本按了不会有反应（自动化的那份见 ./test-binds.sh）。
set -u
cd "$(dirname "$0")"
export WAYLAND_DISPLAY=wayland-0

log=/tmp/vidow-nested.log
mark() { echo "--- $1 ---"; }
places() { grep -E "place wm-" "$log" | tail -3; }

mark "初始（3 列，焦点在最右列）"; places

wtype -M logo -k h; sleep 0.7
mark "Mod+H 焦点左移一列"; places

wtype -M logo -k h; sleep 0.7
mark "Mod+H 再左移（回到第一列）"; places

wtype -M logo -k l; sleep 0.7
mark "Mod+L 焦点右移"; places

wtype -M logo -k r; sleep 0.7
mark "Mod+R 列宽切到 1/3"; places

wtype -M logo -M shift -k l; sleep 0.7
mark "Mod+Shift+L 当前列右移一位"; places

wtype -M logo -k j; sleep 0.7
mark "Mod+J 焦点下移"; places

echo "=== 合成器是否存活 ==="
pgrep -x vidow >/dev/null && echo "vidow 存活" || echo "vidow 挂了"
