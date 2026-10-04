#!/bin/bash
# 在当前 Wayland 会话里嵌套运行 vidow，开 3 个客户端 + waybar，验证平铺与 layer-shell。
# 用法: ./run-nested.sh [客户端命令，默认 kitty]
set -u
cd "$(dirname "$0")"

CLIENT=${1:-kitty}

if [ ! -x build/vidow ]; then
	echo "先构建: meson setup build && ninja -C build" >&2
	exit 1
fi

pkill -f "build/vido[w]" 2>/dev/null
sleep 1

setsid nohup env WLR_BACKENDS=wayland VIDOW_LOG=debug ./build/vidow \
	> /tmp/vidow-nested.log 2>&1 < /dev/null &
sleep 3

sock=$(grep -o "WAYLAND_DISPLAY=[a-z0-9-]*" /tmp/vidow-nested.log | head -1 | cut -d= -f2)
if [ -z "$sock" ]; then
	echo "vidow 启动失败，看 /tmp/vidow-nested.log" >&2
	exit 1
fi
echo "vidow 运行中: WAYLAND_DISPLAY=$sock (日志 /tmp/vidow-nested.log)"
echo "窗口内: Super+Enter 开终端, Super+J/K 切焦点, Super+Q 关窗, Super+Escape 退出"

for t in a b c; do
	setsid nohup env WAYLAND_DISPLAY="$sock" "$CLIENT" --title "wm-$t" \
		> "/tmp/k-$t.log" 2>&1 < /dev/null &
	sleep 2.5
done

# 顶栏：验证 layer-shell + exclusive zone
if command -v waybar >/dev/null; then
	cat > /tmp/wb.conf <<-'EOF'
	{
	  "layer": "top",
	  "position": "top",
	  "height": 30,
	  "modules-left": ["clock"],
	  "clock": { "format": "{:%H:%M}" }
	}
	EOF
	echo 'window#waybar { background: #222222; color: #eeeeee; }' > /tmp/wb.css
	setsid nohup env WAYLAND_DISPLAY="$sock" waybar -c /tmp/wb.conf -s /tmp/wb.css \
		> /tmp/wb.log 2>&1 < /dev/null &
	sleep 3
fi

echo "=== 合成器事件 ==="
grep -E "layer map|map wm|place " /tmp/vidow-nested.log | tail -12
