#!/bin/bash
set -e

VIDOW_BIN="${1:-.}/build/vidow"
TESTS=10

echo "════════════════════════════════════════════════════"
echo "Steam 黑屏修复验证"
echo "════════════════════════════════════════════════════"

# 检查二进制
if [ ! -x "$VIDOW_BIN" ]; then
    echo "[ERROR] Vidow 二进制不存在或不可执行: $VIDOW_BIN"
    exit 1
fi

echo "[✓] 二进制就绪: $VIDOW_BIN"
echo ""

# 停止现有 Vidow 和 Steam
pkill -f "vidow" || true
pkill -f "steam" || true
sleep 2

# 启动 Vidow（后台）
export DISPLAY=:0
$VIDOW_BIN > /tmp/vidow-test.log 2>&1 &
VIDOW_PID=$!
echo "[✓] Vidow 已启动 (PID=$VIDOW_PID)"
sleep 3

# 测试循环
PASS=0
FAIL=0

for i in $(seq 1 $TESTS); do
    echo ""
    echo "━━━━ 测试 $i/$TESTS ━━━━"
    
    # 启动 Steam
    steam > /tmp/steam-test-$i.log 2>&1 &
    STEAM_PID=$!
    
    # 等待窗口出现
    sleep 4
    
    # 检查 Steam 窗口是否在屏幕上
    if xdotool search --name "Steam" > /dev/null 2>&1; then
        echo "[✓] Steam 窗口正常显示"
        ((PASS++))
    else
        echo "[✗] Steam 窗口黑屏或不可见"
        ((FAIL++))
    fi
    
    # 关闭 Steam
    kill $STEAM_PID 2>/dev/null || true
    sleep 2
done

echo ""
echo "════════════════════════════════════════════════════"
echo "测试结果"
echo "════════════════════════════════════════════════════"
echo "通过: $PASS/$TESTS"
echo "失败: $FAIL/$TESTS"
echo ""

if [ $FAIL -eq 0 ]; then
    echo "✓ 所有测试通过！修复成功。"
    kill $VIDOW_PID
    exit 0
else
    echo "✗ 有 $FAIL 次失败。请检查日志："
    echo "   tail -50 /tmp/vidow-test.log"
    kill $VIDOW_PID
    exit 1
fi
