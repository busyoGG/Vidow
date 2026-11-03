#!/usr/bin/env bash
# 测试「纯通道调色走 KMS LUT、保住直通」这条混合路线：
#   1. 只有色温 / 亮度 / 对比度 / 伽马时，日志说走 KMS LUT（不是后处理）
#   2. 这条路上的上屏像素和 shader 那条路**逐字节一致**（像素从 LUT 来）
#   3. 同一个参数组合在纯 LUT / 纯 shader 下抓到的图完全相同
#   4. 饱和度 / 色相一开就先降级到 shader，关掉又回到 LUT
#   5. 全中性时两条路都不开
#
# headless 的输出没有 KMS gamma 表（wlr_output_get_gamma_size = 0），所以用
# VIDOW_TEST_GRADE_LUT_SIZE 假装一个 4096 项的表，让「曲线 → LUT → 提交」
# 这条链路跑起来。像素读的是 grim 的 screencopy（它读的就是输出交换链）。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

BASE="/tmp/opencode/gradeluttest_$$"
mkdir -p "$BASE"
FAIL=0
ok()  { echo "  ok: $*"; }
bad() { echo "  FAIL: $*"; FAIL=$((FAIL + 1)); }

cleanup() {
    for d in "$BASE"/*/; do
        [ -d "$d" ] || continue
        stop_one "${d%/}"
    done
    rm -rf "$BASE"
}
trap cleanup EXIT INT TERM

# 只杀 XDG_RUNTIME_DIR 指着自己临时目录的那个 vidow（不碰用户的会话）
stop_one() {
    local dir="$1" p
    for p in /proc/[0-9]*; do
        [ -r "$p/cmdline" ] || continue
        case "$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null || true)" in
            *build/vidow*)
                if tr '\0' '\n' < "$p/environ" 2>/dev/null |
                        grep -qx "XDG_RUNTIME_DIR=$dir/run"; then
                    kill "${p#/proc/}" 2>/dev/null || true
                fi
                ;;
        esac
    done
}

# 起一份 headless vidow。$1 = 名字（也是临时子目录），$2 = 额外环境
# （比如 VIDOW_TEST_GRADE_LUT_SIZE=4096 或 "-"），stdin = config.lua 内容。
start() {
    local name="$1" env2="$2" dir="$BASE/$1"
    rm -rf "$dir"
    mkdir -p "$dir/home/.config/vidow" "$dir/run"
    chmod 700 "$dir/run"
    cat > "$dir/home/.config/vidow/config.lua"
    local -a extra=()
    [ "$env2" != "-" ] && extra=("$env2")
    env XDG_RUNTIME_DIR="$dir/run" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=1 \
        WLR_LIBINPUT_NO_DEVICES=1 HOME="$dir/home" VIDOW_LOG=info \
        "${extra[@]}" setsid ./build/vidow > "$dir/log" 2>&1 &
    local i w
    for i in $(seq 1 60); do
        w=$(ls "$dir/run" 2>/dev/null | grep -m1 '^wayland-[0-9]*$' || true)
        [ -n "$w" ] && break
        sleep 0.1
    done
    [ -n "${w:-}" ] || { bad "$name 起不来"; return 1; }
    export WAYLAND_DISPLAY="$w" XDG_RUNTIME_DIR="$dir/run"
    # 等第一帧上屏
    sleep 1.2
}

# 抓中心像素到 $2（png），输出 "r g b"
shot() {
    grim -o HEADLESS-1 "$2"
    python3 - "$2" << 'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1])
r, g, b = im.getpixel((im.width // 2, im.height // 2))[:3]
print(r, g, b)
PY
}

# 最后一条「走了哪条路」的日志：LUT / shader / 关（只看最后一条，
# 日志是累积的，前面的模式还在）
mode_now() {
    local last
    last=$(grep -o "调色走 [^（]*\|输出端调色 关" "$1" | tail -1 || true)
    case "$last" in
        *"KMS LUT"*) echo kms ;;
        *shader*)    echo shader ;;
        *关)         echo off ;;
        *)           echo none ;;
    esac
}

# 等日志里出现某个模式（配置重载 → 排帧 → 提交有延迟）
wait_mode() {
    local log="$1" want="$2" i
    for i in $(seq 1 40); do
        [ "$(mode_now "$log")" = "$want" ] && return 0
        sleep 0.1
    done
    return 1
}

setcfg() {
    ./build/vidowmsg set-config "$1" "$2" > /dev/null
}

# ---------------------------------------------------------------------------
# A. 同一组参数：纯 LUT（VIDOW_TEST_GRADE_LUT_SIZE）vs 纯 shader（不给 LUT）
#    两边都从「临时目录里的 config」启动，参数完全一样。
GRADE='contrast = 1.2, temperature = 4200, brightness = 0.9, gamma = 1.7'
CFG=$(printf 'background_color = "#c04020ff"\ngrade = { %s }\n' "$GRADE")

# A1：纯 LUT
start lut "VIDOW_TEST_GRADE_LUT_SIZE=4096" <<< "$CFG" || exit 1
LUTLOG="$BASE/lut/log"
if [ "$(mode_now "$LUTLOG")" = kms ]; then
    ok "纯通道曲线 → 走 KMS LUT ($(grep -o '调色走 [^（]*' "$LUTLOG" | tail -1))"
else
    bad "纯通道曲线应走 KMS LUT，日志是 $(grep -o '调色走 [^（]*\|全中性' "$LUTLOG" | tail -1)"
fi
read -r LR LG LB < <(shot lut "$BASE/lut.png")
stop_one "$BASE/lut"

# A2：同样参数、但后端没有 LUT（VIDOW_TEST_GRADE_LUT_SIZE=0）→ shader
start sh "VIDOW_TEST_GRADE_LUT_SIZE=0" <<< "$CFG" || exit 1
SHLOG="$BASE/sh/log"
if [ "$(mode_now "$SHLOG")" = shader ]; then
    ok "没有 LUT 时退到 shader 后处理"
else
    bad "没有 LUT 时应退到 shader，日志是 $(grep -o '调色走 [^（]*' "$SHLOG" | tail -1)"
fi
read -r SR SG SB < <(shot sh "$BASE/sh.png")
stop_one "$BASE/sh"

if [ "$LR $LG $LB" = "$SR $SG $SB" ]; then
    ok "LUT 与 shader 两条路像素一致 ($LR,$LG,$LB)"
else
    bad "LUT ($LR,$LG,$LB) 与 shader ($SR,$SG,$SB) 不一致"
fi

# 剩下的用例用 LUT 那份（还开着）
start nos "VIDOW_TEST_GRADE_LUT_SIZE=4096" <<< "$CFG" || exit 1
NOSLOG="$BASE/nos/log"

# A4：LUT 模式下开饱和度 → 降级到 shader；关掉 → 回到 LUT
setcfg grade_saturation 0
if wait_mode "$NOSLOG" shader; then
    ok "饱和度 ≠ 1 → 降级到 shader 后处理（直通关闭）"
else
    bad "饱和度 ≠ 1 时日志没切到 shader：$(grep -o '调色走 [^（]*' "$NOSLOG" | tail -1)"
fi
setcfg grade_saturation 1
if wait_mode "$NOSLOG" kms; then
    ok "饱和度回 1 → 回到 KMS LUT"
else
    bad "饱和度回 1 时没回到 KMS LUT：$(grep -o '调色走 [^（]*' "$NOSLOG" | tail -1)"
fi
read -r R G B < <(shot nos "$BASE/back.png")
if [ "$R $G $B" = "$LR $LG $LB" ]; then
    ok "来回切换后像素回到原样 ($R,$G,$B)"
else
    bad "来回切换后像素应回到 ($LR,$LG,$LB)，实际 ($R,$G,$B)"
fi

# A5：色相 ≠ 0 同样降级
setcfg grade_hue 30
if wait_mode "$NOSLOG" shader; then
    ok "色相 ≠ 0 → 降级到 shader 后处理"
else
    bad "色相 ≠ 0 时日志没切到 shader"
fi
setcfg grade_hue 0

# A6：全中性 → 两条路都关
setcfg grade_contrast 1
setcfg grade_temperature 6500
setcfg grade_brightness 1
setcfg grade_gamma 1
if wait_mode "$NOSLOG" off; then
    ok "全中性 → 关掉调色（LUT 也撤掉，像素回原样）"
else
    bad "全中性时日志没回到「输出端调色 关」"
fi
read -r R G B < <(shot nos "$BASE/neutral.png")
if [ "$R" = "192" ] && [ "$G" = "64" ] && [ "$B" = "32" ]; then
    ok "全中性像素回到背景原色 ($R,$G,$B)"
else
    bad "全中性应为 (192,64,32)，实际 ($R,$G,$B)"
fi
stop_one "$BASE/nos"

if [ "$FAIL" -ne 0 ]; then
    echo "失败 $FAIL 项"
    exit 1
fi
echo "全部通过"
