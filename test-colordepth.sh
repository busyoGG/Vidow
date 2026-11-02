#!/usr/bin/env bash
# 测试位深与色彩管理：
# 1. wp_color_management_v1 协议导出与 preferred 空间
# 2. 灰阶渐变渲染与抓帧无异常、直方图连贯性
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

TMP="/tmp/opencode/colortest_$$"
mkdir -p "$TMP/home/.config/vidow" "$TMP/run"
chmod 700 "$TMP/run"

cleanup() {
    for p in /proc/[0-9]*; do
        [ -r "$p/cmdline" ] || continue
        c=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null || true)
        case "$c" in
            *build/vidow*)
                if tr '\0' '\n' < "$p/environ" 2>/dev/null | grep -qx "XDG_RUNTIME_DIR=$TMP/run"; then
                    kill "${p#/proc/}" 2>/dev/null || true
                fi
                ;;
        esac
    done
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

cat > "$TMP/home/.config/vidow/config.lua" << 'EOF'
vidow.output{ name = "HEADLESS-1", mode = "1024x768", depth = 10, hdr = true }
EOF

XDG_RUNTIME_DIR="$TMP/run" \
WLR_BACKENDS=headless \
WLR_HEADLESS_OUTPUTS=1 \
WLR_LIBINPUT_NO_DEVICES=1 \
HOME="$TMP/home" \
VIDOW_LOG=error \
setsid ./build/vidow > "$TMP/log" 2>&1 &

sleep 3
W=$(ls "$TMP/run" | grep -m1 '^wayland-[0-9]*$')
export WAYLAND_DISPLAY="$W" XDG_RUNTIME_DIR="$TMP/run"

# 1. 验证 colorprobe 协议导出
./build/colorprobe > "$TMP/probe.txt" 2>&1
grep -q "wp_color_manager_v1: 有" "$TMP/probe.txt"
grep -q "parametric" "$TMP/probe.txt"
grep -q "gamma22" "$TMP/probe.txt"
grep -q "srgb" "$TMP/probe.txt"

# 2. 跑渐变窗并抓帧
python3 tools/gradient.py --hold 1.5 &
GPID=$!
sleep 0.8
grim -o HEADLESS-1 "$TMP/gradient.png"
wait "$GPID" 2>/dev/null || true

# 3. 验证直方图：渐变非空且平滑
python3 -c '
from PIL import Image
import numpy as np

img = Image.open("'"$TMP/gradient.png"'")
arr = np.array(img)
assert arr.shape == (768, 1024, 3), f"bad shape {arr.shape}"
# 水平渐变，窗口内部从左到右单调递增
sub = arr[200:500, 10:1010, :]
assert sub[:, 0, :].mean() < 15, "left not black"
assert sub[:, -1, :].mean() > 240, "right not white"
col_means = sub.mean(axis=(0, 2))
diffs = np.diff(col_means)
assert (diffs >= 0).sum() / len(diffs) > 0.95, "not monotonic gradient"
'

echo "test-colordepth.sh: OK"
