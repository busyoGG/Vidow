#!/usr/bin/env bash
# 测试脚本：Steam 首次正常、二次黑屏精准复现
# 输出：差异矩阵 + 完整观测日志

set -euo pipefail

# ==================== 配置 ====================
VIDOW_BIN="${VIDOW_BIN:-./build/vidow}"
VIDOWMSG="${VIDOWMSG:-./build/vidowmsg}"
STEAM_CMD="${STEAM_CMD:-steam}"
LOG_DIR="${LOG_DIR:-./test-logs}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
TEST_LOG="$LOG_DIR/steam_reopen_$TIMESTAMP"

# 超时配置
VIDOW_STARTUP_TIMEOUT=5
STEAM_STARTUP_TIMEOUT=15
OBSERVATION_DELAY=3

# GPU 类型检测
GPU_TYPE="unknown"
if command -v nvidia-smi &>/dev/null; then
    GPU_TYPE="nvidia"
elif command -v radeontop &>/dev/null; then
    GPU_TYPE="amd"
fi

# ==================== 初始化 ====================
mkdir -p "$LOG_DIR"
exec > >(tee "${TEST_LOG}.main.log") 2>&1

echo "=================================================="
echo "Steam 重启黑屏精准复现测试"
echo "=================================================="
echo "时间戳: $TIMESTAMP"
echo "GPU 类型: $GPU_TYPE"
echo "Vidow: $VIDOW_BIN"
echo "日志目录: $TEST_LOG.*"
echo ""

# ==================== 清理函数 ====================
cleanup() {
    echo "[清理] 终止所有测试进程..."
    pkill -9 -f "$VIDOW_BIN" 2>/dev/null || true
    pkill -9 steam 2>/dev/null || true
    sleep 1
}
trap cleanup EXIT INT TERM

# ==================== 观测函数 ====================

# 1. XWayland 窗口状态
observe_xwayland() {
    local phase=$1
    local output="${TEST_LOG}.${phase}.xwayland.log"
    
    echo "[观测] XWayland 窗口状态 (Phase: $phase)"
    
    {
        echo "=== xwininfo 窗口树 ==="
        DISPLAY=:0 xwininfo -root -tree 2>/dev/null || echo "FAIL: xwininfo"
        
        echo ""
        echo "=== xdotool 窗口列表 ==="
        DISPLAY=:0 xdotool search --class "steam" 2>/dev/null | while read wid; do
            echo "Window ID: $wid"
            DISPLAY=:0 xwininfo -id "$wid" 2>/dev/null || echo "FAIL: xwininfo $wid"
            DISPLAY=:0 xprop -id "$wid" 2>/dev/null || echo "FAIL: xprop $wid"
            echo "---"
        done
        
        echo ""
        echo "=== xlsclients ==="
        DISPLAY=:0 xlsclients -l 2>/dev/null || echo "FAIL: xlsclients"
        
    } > "$output" 2>&1
    
    echo "  -> $output"
}

# 2. wlr_surface 和 Vidow 内部状态
observe_vidow_state() {
    local phase=$1
    local output="${TEST_LOG}.${phase}.vidow.log"
    
    echo "[观测] Vidow 内部状态 (Phase: $phase)"
    
    {
        echo "=== vidowmsg surfaces ==="
        "$VIDOWMSG" surfaces 2>&1 || echo "FAIL: vidowmsg surfaces"
        
        echo ""
        echo "=== vidowmsg views ==="
        "$VIDOWMSG" views 2>&1 || echo "FAIL: vidowmsg views"
        
        echo ""
        echo "=== vidowmsg outputs ==="
        "$VIDOWMSG" outputs 2>&1 || echo "FAIL: vidowmsg outputs"
        
        echo ""
        echo "=== vidowmsg xwayland ==="
        "$VIDOWMSG" xwayland 2>&1 || echo "FAIL: vidowmsg xwayland"
        
    } > "$output" 2>&1
    
    echo "  -> $output"
}

# 3. GPU 渲染状态
observe_gpu() {
    local phase=$1
    local output="${TEST_LOG}.${phase}.gpu.log"
    
    echo "[观测] GPU 渲染状态 (Phase: $phase)"
    
    {
        echo "=== glxinfo ==="
        DISPLAY=:0 glxinfo 2>&1 | head -100 || echo "FAIL: glxinfo"
        
        echo ""
        if [[ "$GPU_TYPE" == "nvidia" ]]; then
            echo "=== nvidia-smi ==="
            nvidia-smi 2>&1 || echo "FAIL: nvidia-smi"
            
            echo ""
            echo "=== nvidia-smi processes ==="
            nvidia-smi pmon -c 1 2>&1 || echo "FAIL: nvidia-smi pmon"
            
        elif [[ "$GPU_TYPE" == "amd" ]]; then
            echo "=== radeontop (1秒采样) ==="
            timeout 1s radeontop -d - -l 1 2>&1 || echo "FAIL: radeontop"
        fi
        
        echo ""
        echo "=== DRI 渲染节点 ==="
        ls -la /dev/dri/ 2>&1 || echo "FAIL: ls /dev/dri"
        
    } > "$output" 2>&1
    
    echo "  -> $output"
}

# 4. 帧缓冲对比测试（glxgears 基准）
observe_framebuffer() {
    local phase=$1
    local output="${TEST_LOG}.${phase}.framebuffer.log"
    
    echo "[观测] 帧缓冲有效性 (Phase: $phase)"
    
    {
        echo "=== glxgears 5秒测试 ==="
        DISPLAY=:0 timeout 5s glxgears 2>&1 || echo "FAIL: glxgears timeout"
        
        echo ""
        echo "=== EGL 信息 ==="
        DISPLAY=:0 eglinfo 2>&1 | head -50 || echo "FAIL: eglinfo"
        
    } > "$output" 2>&1
    
    echo "  -> $output"
}

# 5. 综合观测调度器
run_observations() {
    local phase=$1
    echo ""
    echo "========================================"
    echo "阶段: $phase - 开始全量观测"
    echo "========================================"
    
    observe_xwayland "$phase"
    observe_vidow_state "$phase"
    observe_gpu "$phase"
    observe_framebuffer "$phase"
    
    echo ""
}

# ==================== 测试流程 ====================

echo "[步骤 1/7] 清理旧环境"
cleanup
sleep 2

echo ""
echo "[步骤 2/7] 启动 Vidow"
export WAYLAND_DISPLAY=wayland-vidow-test
"$VIDOW_BIN" > "${TEST_LOG}.vidow.stdout.log" 2>&1 &
VIDOW_PID=$!

echo "  等待 Vidow 启动 (${VIDOW_STARTUP_TIMEOUT}s)..."
sleep "$VIDOW_STARTUP_TIMEOUT"

if ! kill -0 "$VIDOW_PID" 2>/dev/null; then
    echo "ERROR: Vidow 启动失败"
    exit 1
fi
echo "  Vidow PID: $VIDOW_PID"

echo ""
echo "[步骤 3/7] 基线观测（无窗口状态）"
run_observations "0_baseline"
sleep "$OBSERVATION_DELAY"

echo ""
echo "[步骤 4/7] 首次启动 Steam"
DISPLAY=:0 "$STEAM_CMD" > "${TEST_LOG}.steam.first.log" 2>&1 &
STEAM_PID_1=$!

echo "  等待 Steam 启动 (${STEAM_STARTUP_TIMEOUT}s)..."
sleep "$STEAM_STARTUP_TIMEOUT"

if ! kill -0 "$STEAM_PID_1" 2>/dev/null; then
    echo "WARNING: Steam 首次启动进程已退出"
fi

run_observations "1_first_open"
sleep "$OBSERVATION_DELAY"

echo ""
echo "[步骤 5/7] 关闭首次 Steam"
pkill -TERM steam
sleep 3

# 确保完全关闭
pkill -9 steam 2>/dev/null || true
sleep 2

run_observations "2_after_close"
sleep "$OBSERVATION_DELAY"

echo ""
echo "[步骤 6/7] 二次启动 Steam"
DISPLAY=:0 "$STEAM_CMD" > "${TEST_LOG}.steam.second.log" 2>&1 &
STEAM_PID_2=$!

echo "  等待 Steam 二次启动 (${STEAM_STARTUP_TIMEOUT}s)..."
sleep "$STEAM_STARTUP_TIMEOUT"

if ! kill -0 "$STEAM_PID_2" 2>/dev/null; then
    echo "WARNING: Steam 二次启动进程已退出"
fi

run_observations "3_second_open"
sleep "$OBSERVATION_DELAY"

echo ""
echo "[步骤 7/7] 生成差异矩阵"
DIFF_OUTPUT="${TEST_LOG}.diff_matrix.txt"

{
    echo "=================================================="
    echo "差异矩阵：首次正常 vs 二次黑屏"
    echo "=================================================="
    echo ""
    
    echo "【XWayland 窗口差异】"
    echo "-------------------------------------------"
    diff -u "${TEST_LOG}.1_first_open.xwayland.log" \
            "${TEST_LOG}.3_second_open.xwayland.log" \
        || echo "(存在差异)"
    echo ""
    
    echo "【Vidow 内部状态差异】"
    echo "-------------------------------------------"
    diff -u "${TEST_LOG}.1_first_open.vidow.log" \
            "${TEST_LOG}.3_second_open.vidow.log" \
        || echo "(存在差异)"
    echo ""
    
    echo "【GPU 渲染状态差异】"
    echo "-------------------------------------------"
    diff -u "${TEST_LOG}.1_first_open.gpu.log" \
            "${TEST_LOG}.3_second_open.gpu.log" \
        || echo "(存在差异)"
    echo ""
    
    echo "【帧缓冲有效性差异】"
    echo "-------------------------------------------"
    diff -u "${TEST_LOG}.1_first_open.framebuffer.log" \
            "${TEST_LOG}.3_second_open.framebuffer.log" \
        || echo "(存在差异)"
    echo ""
    
} > "$DIFF_OUTPUT" 2>&1

echo ""
echo "=================================================="
echo "测试完成"
echo "=================================================="
echo "差异矩阵: $DIFF_OUTPUT"
echo "完整日志前缀: $TEST_LOG.*"
echo ""
echo "关键日志文件："
ls -lh "${TEST_LOG}".* 2>/dev/null | awk '{print "  " $9 " (" $5 ")"}'
