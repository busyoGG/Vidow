#!/usr/bin/env bash
# 测试输出端调色（grade.*）：
#   1. 饱和度 0 把纯色背景变成灰度（线性光里算亮度）
#   2. 对比度 0 把画面压到线性 0.18 的中灰
#   3. 色温 < 6500 偏暖（红升蓝降）、6500 回到中性
#   4. 色相旋转、亮度倍率、伽马都真的改变上屏像素
#   5. 全部中性时**关掉后处理**（日志 + 像素回到原样）
#   6. 越界值被夹住（伽马 0 → 0.1、色温 0 → 1000K）
# 像素一律走 grim 的 screencopy：它抓的就是输出交换链（调色后的那张）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

TMP="/tmp/opencode/gradetest_$$"
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

# 纯色背景：sRGB #c04020（8 位）。gamma22 解码后的亮度见下面的断言。
cat > "$TMP/home/.config/vidow/config.lua" << 'EOF'
background_color = "#c04020ff"
grade = { saturation = 0 }
EOF

XDG_RUNTIME_DIR="$TMP/run" \
WLR_BACKENDS=headless \
WLR_HEADLESS_OUTPUTS=1 \
WLR_LIBINPUT_NO_DEVICES=1 \
HOME="$TMP/home" \
VIDOW_LOG=info \
setsid ./build/vidow > "$TMP/log" 2>&1 &

sleep 3
W=$(ls "$TMP/run" | grep -m1 '^wayland-[0-9]*$')
export WAYLAND_DISPLAY="$W" XDG_RUNTIME_DIR="$TMP/run"

FAIL=0
ok()   { echo "  ok: $*"; }
bad()  { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }

# 抓屏幕中心一个像素，输出 "r g b"
pixel() {
    grim -o HEADLESS-1 "$TMP/p.png"
    python3 - "$TMP/p.png" << 'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1])
r, g, b = im.getpixel((im.width // 2, im.height // 2))[:3]
print(r, g, b)
PY
}

# set-config + 等一帧 + 抓像素
setcfg() {
    ./build/vidowmsg set-config "$1" "$2" > /dev/null
    sleep 0.35
}

getcfg() {
    ./build/vidowmsg get-config "$1" 2>/dev/null |
        python3 -c 'import json,sys; print(json.load(sys.stdin)["value"])'
}

# 1. 饱和度 0：纯红 → 灰度（线性亮度 ≈ 0.1486 → gamma22 编码 ≈ 107）
read -r R G B < <(pixel)
if [ "$R" -ge 100 ] && [ "$R" -le 114 ] &&
   [ "$((R > G ? R - G : G - R))" -le 2 ] &&
   [ "$((G > B ? G - B : B - G))" -le 2 ]; then
    ok "饱和度 0 → 灰度 ($R,$G,$B)"
else
    bad "饱和度 0 应得灰度 ~107，实际 ($R,$G,$B)"
fi

# 2. 恢复饱和度：回到背景原色
setcfg grade_saturation 1
read -r R G B < <(pixel)
if [ "$((R > 192 ? R - 192 : 192 - R))" -le 2 ] &&
   [ "$((G > 64 ? G - 64 : 64 - G))" -le 2 ] &&
   [ "$((B > 32 ? B - 32 : 32 - B))" -le 2 ]; then
    ok "中性时像素回到背景原色 ($R,$G,$B)"
else
    bad "中性时应为 (192,64,32)，实际 ($R,$G,$B)"
fi

# 3. 对比度 0：线性 0.18 的中灰（gamma22 编码 ≈ 117）
setcfg grade_contrast 0
read -r R G B < <(pixel)
if [ "$((R > 117 ? R - 117 : 117 - R))" -le 2 ] &&
   [ "$((R > G ? R - G : G - R))" -le 1 ] &&
   [ "$((G > B ? G - B : B - G))" -le 1 ]; then
    ok "对比度 0 → 中灰 ($R,$G,$B)"
else
    bad "对比度 0 应得 ~(117,117,117)，实际 ($R,$G,$B)"
fi
setcfg grade_contrast 1

# 4. 色温 3000K：偏暖（红升、蓝降）
setcfg grade_temperature 3000
read -r R G B < <(pixel)
if [ "$R" -gt 192 ] && [ "$B" -lt 32 ]; then
    ok "色温 3000K 偏暖 ($R,$G,$B)"
else
    bad "色温 3000K 应红升蓝降（原色 192,_,32），实际 ($R,$G,$B)"
fi
setcfg grade_temperature 6500
read -r R2 G2 B2 < <(pixel)
if [ "$R2" = "$R" ] && [ "$B2" = "$B" ]; then
    bad "色温 6500K 应回到中性，实际还是 ($R2,$G2,$B2)"
else
    ok "色温 6500K 回到中性 ($R2,$G2,$B2)"
fi

# 5. 色相 +90°：颜色明显离开原色（红 → 蓝紫）
setcfg grade_hue 90
read -r R G B < <(pixel)
DR=$((R > 192 ? R - 192 : 192 - R))
DB=$((B > 32 ? B - 32 : 32 - B))
if [ "$DR" -ge 20 ] && [ "$DB" -ge 20 ]; then
    ok "色相 90° 改变颜色 ($R,$G,$B)"
else
    bad "色相 90° 应明显偏色，实际 ($R,$G,$B)"
fi
setcfg grade_hue 0

# 6. 亮度 1.5：更亮；伽马 2：中间调更亮
setcfg grade_brightness 1.5
read -r R G B < <(pixel)
if [ "$R" -gt 192 ] && [ "$G" -gt 64 ]; then
    ok "亮度 1.5 更亮 ($R,$G,$B)"
else
    bad "亮度 1.5 应更亮，实际 ($R,$G,$B)"
fi
setcfg grade_brightness 1
setcfg grade_gamma 2
read -r Rr Gg Bb < <(pixel)
if [ "$Rr" -gt 192 ] && [ "$Gg" -gt 64 ] && [ "$Bb" -gt 32 ]; then
    ok "伽马 2 中间调更亮 ($Rr,$Gg,$Bb)"
else
    bad "伽马 2 应整体更亮，实际 ($Rr,$Gg,$Bb)"
fi
setcfg grade_gamma 1

# 7. 全部中性：像素原样，日志说「调色 关」
read -r R G B < <(pixel)
if [ "$R" = "192" ] && [ "$G" = "64" ] && [ "$B" = "32" ]; then
    ok "全中性 → 回到直通，像素原样 ($R,$G,$B)"
else
    bad "全中性应回到 (192,64,32)，实际 ($R,$G,$B)"
fi
sleep 0.3
if grep -q "输出端调色 关" "$TMP/log"; then
    ok "日志：全中性时关掉后处理"
else
    bad "日志里没有「输出端调色 关」"
fi

# 8. 越界值被夹住
setcfg grade_gamma 0
V=$(getcfg grade_gamma)
if [ "$V" = "0.1" ]; then
    ok "伽马 0 被夹到 0.1"
else
    bad "伽马 0 应夹到 0.1，读到 $V"
fi
setcfg grade_temperature 0
V=$(getcfg grade_temperature)
if [ "$V" = "1000" ]; then
    ok "色温 0 被夹到 1000K"
else
    bad "色温 0 应夹到 1000K，读到 $V"
fi
setcfg grade_gamma 1
setcfg grade_temperature 6500

# 9. 配置里写组名 / 平铺键两种都认（启动那份用的组名，这里补平铺键）
setcfg grade_saturation 0
V=$(getcfg grade_saturation)
if [ "$V" = "0" ]; then
    ok "set-config 写平铺键生效"
else
    bad "set-config grade_saturation=0 读到 $V"
fi

if [ "$FAIL" -ne 0 ]; then
    echo "$FAIL 项失败（日志 $TMP/log）"
    exit 1
fi
echo "全部通过"
