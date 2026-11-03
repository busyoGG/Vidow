#!/usr/bin/env bash
# 迁移基线（docs/plan-dewlroots-vulkan.md 的第 0 步）：把「换渲染器 / 脱 wlroots」
# 之前的状态量下来，之后每一步都拿它做对照。
#
# 产出一份 `docs/migration-baseline/`：
#   env.txt            内核 / GPU / Mesa / wlroots / 编译器 / 提交号
#   inventory.tsv      117 个测试脚本 + 每个的第一行说明（MANUAL = 要真人会话）
#   globals.txt        headless Vidow 导出的 Wayland 全局（wayland-info）
#   state.json         `vidowmsg state` 全量（含 render_stats / shaders 清单）
#   state-summary.txt  从 state.json 抽出来的关键项
#   tests.tsv          每个自动测试的结果（PASS/FAIL/TIMEOUT + 耗时）
#   failures/<name>.log 只留失败 / 超时那几个的日志
#   summary.md         人看的总表
#
# 用法：
#   tools/baseline.sh              跑全部自动测试（串行，可能要十几分钟）
#   tools/baseline.sh --no-tests   只抓环境 / 全局 / 状态
#
# 注意：测试脚本大多用固定的 /tmp/vidow-* 路径，所以只能串行跑。
set -uo pipefail
cd "$(dirname "$0")/.."

OUT="docs/migration-baseline"
RUN_TESTS=1
[ "${1:-}" = "--no-tests" ] && RUN_TESTS=0

BIN=./build/vidow
MSG=./build/vidowmsg
[ -x "$BIN" ] || { echo "先构建：ninja -C build" >&2; exit 1; }

mkdir -p "$OUT/failures"
: > "$OUT/failures/.keep"

now() { date -Is; }
secs() { date +%s; }

# ---------------------------------------------------------------- 环境
{
    echo "# 迁移基线环境"
    echo "时间: $(now)"
    echo "分支: $(git rev-parse --abbrev-ref HEAD)"
    echo "提交: $(git rev-parse HEAD)"
    echo "工作区: $(git status --porcelain | wc -l) 个未提交文件"
    echo "内核: $(uname -r)"
    echo "发行版: $(sed -n 's/^PRETTY_NAME=//p' /etc/os-release | tr -d '\"')"
    echo "CPU: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')（$(nproc) 线程）"
    echo "GPU: $(lspci -nn 2>/dev/null | grep -E '\[03(00|02|80)\]:' | sed 's/^[0-9a-f:.]* //' | paste -sd '; ' -)"
    echo "wlroots: $(pkg-config --modversion wlroots-0.20 2>/dev/null || echo 无)"
    echo "wayland-server: $(pkg-config --modversion wayland-server 2>/dev/null)"
    echo "libdrm: $(pkg-config --modversion libdrm 2>/dev/null)"
    echo "Mesa: $(pacman -Q mesa 2>/dev/null | awk '{print $2}' || echo 未知)"
    echo "编译器: $(cc --version | head -1)"
    m=$(glxinfo -B 2>/dev/null | grep -E 'OpenGL renderer|OpenGL version' | paste -sd '; ' -)
    [ -n "$m" ] || m=未知
    echo "mesa 渲染器: $m"
    echo "Vulkan: $(vulkaninfo --summary 2>/dev/null | grep -m1 'deviceName' | cut -d= -f2 | sed 's/^ //' || echo 未知)"
} > "$OUT/env.txt"

# ---------------------------------------------------------------- 测试清单
{
    printf 'script\tdescription\tclass\n'
    for f in test-*.sh; do
        desc=$(sed -n '2,3p' "$f" | grep '^#' | sed 's/^# *//' | head -1)
        case "$f" in
            test-keys.sh|test-steam-fix.sh|test-steam-reopen-blackscreen.sh)
                cls=MANUAL ;;   # 要真人嵌套会话 / 真实 Steam，不进自动回归
            *) cls=auto ;;
        esac
        printf '%s\t%s\t%s\n' "$f" "$desc" "$cls"
    done
} > "$OUT/inventory.tsv"

# -------------------------------------------------------------- 代码度量
# 迁移第 1/2 步要知道「要搬多少」：GL 符号数、fx 层行数、内建程序清单。
{
    echo "# 迁移前的渲染层规模（$(now)）"
    echo
    echo "## 文件行数"
    wc -l src/fx-gl.c src/fx.c src/output.c src/capture.c 2>/dev/null
    echo
    echo "## 用到的 GL 符号（去重）"
    echo "count: $(grep -rho '\bgl[A-Z][A-Za-z0-9]*' src/*.c | sort -u | wc -l)"
    grep -rho '\bgl[A-Z][A-Za-z0-9]*' src/*.c | sort | uniq -c | sort -rn | head -40
    echo
    echo "## 内建着色器程序（fx-gl.c）"
    grep -n 'link_program_full\|link_program(&' src/fx-gl.c | sed 's/^/  /'
    echo
    echo "## 内联 GLSL 源（fx-gl.c）"
    grep -n '^static const char \*.*_src =' src/fx-gl.c | sed 's/^/  /'
    echo
    echo "## 直接摸 GLES2 内部的文件"
    grep -rln 'wlr_gles2_texture_get_attribs\|GLES2/gl2.h\|glUseProgram' src/*.c | sed 's/^/  /'
} > "$OUT/code-metrics.txt"

# ------------------------------------------------------------ 源码全局清单
{
    echo "# 源码里注册的全局 / 创建调用（迁移第 7 步的清单，$(now)）"
    echo
    grep -rhoE "[a-z0-9_]+_v[0-9]+_create\(|\bwlr_[a-z0-9_]+_create\(" src/*.c |
        sed 's/($//' | sort | uniq -c | sort -rn
    echo
    echo "## main.c 里的创建调用"
    grep -nE "_create\(&?server" src/main.c | sed 's/^/  /'
} > "$OUT/globals-source.txt"

# ---------------------------------------------------------------- 全局 + 状态
TMP=$(mktemp -d /tmp/opencode/vidow-baseline-XXXXXX)
mkdir -p "$TMP/home/.config/vidow" "$TMP/run"
chmod 700 "$TMP/run"
cat > "$TMP/home/.config/vidow/config.lua" << 'EOF'
background_color = "#202020ff"
EOF
XDG_RUNTIME_DIR="$TMP/run" WLR_BACKENDS=headless WLR_HEADLESS_OUTPUTS=2 \
    WLR_LIBINPUT_NO_DEVICES=1 HOME="$TMP/home" VIDOW_LOG=info VIDOW_RENDER_STATS=1 \
    setsid "$BIN" > "$TMP/log" 2>&1 &
BPID=$!
for _ in $(seq 1 80); do
    W=$(ls "$TMP/run" 2>/dev/null | grep -m1 '^wayland-[0-9]*$' || true)
    [ -n "${W:-}" ] && break
    sleep 0.1
done
if [ -n "${W:-}" ]; then
    WAYLAND_DISPLAY="$W" XDG_RUNTIME_DIR="$TMP/run" wayland-info > "$OUT/globals.txt" 2>&1
    WAYLAND_DISPLAY="$W" XDG_RUNTIME_DIR="$TMP/run" "$MSG" state > "$OUT/state.json" 2>/dev/null
    python3 - "$OUT/state.json" > "$OUT/state-summary.txt" << 'PY'
import json, sys
try:
    s = json.load(open(sys.argv[1]))
except Exception as e:
    print("state.json 解析失败:", e); raise SystemExit(0)
print("输出:", ", ".join(o.get("name", "?") for o in s.get("outputs", [])))
print("着色器 (%d): %s" % (len(s.get("shaders", [])), ", ".join(s.get("shaders", []))))
rs = s.get("render_stats", {})
print("render_stats:", " ".join("%s=%s" % (k, v) for k, v in rs.items()))
PY
else
    echo "headless 起不来，见 $TMP/log" > "$OUT/globals.txt"
fi
kill "$BPID" 2>/dev/null
sleep 0.3
cp "$TMP/log" "$OUT/headless.log" 2>/dev/null || true

# ---------------------------------------------------------------- 跑测试
if [ "$RUN_TESTS" = 1 ]; then
    printf 'script\tresult\tseconds\n' > "$OUT/tests.tsv"
    total=0; pass=0; fail=0; skip=0; timedout=0
    while IFS=$'\t' read -r t desc cls; do
        [ "$t" = "script" ] && continue
        total=$((total + 1))
        if [ "$cls" = MANUAL ]; then
            printf '%s\tMANUAL\t0\n' "$t" >> "$OUT/tests.tsv"
            skip=$((skip + 1))
            echo "  MANUAL $t"
            continue
        fi
        t0=$(secs)
        timeout 180 bash "./$t" > "$TMP/test.log" 2>&1
        rc=$?
        dt=$(( $(secs) - t0 ))
        if [ "$rc" = 0 ]; then
            printf '%s\tPASS\t%d\n' "$t" "$dt" >> "$OUT/tests.tsv"
            pass=$((pass + 1)); echo "  PASS  ($dt s) $t"
        elif [ "$rc" = 124 ]; then
            printf '%s\tTIMEOUT\t%d\n' "$t" "$dt" >> "$OUT/tests.tsv"
            cp "$TMP/test.log" "$OUT/failures/${t%.sh}.log"
            timedout=$((timedout + 1)); echo "  TIMEOUT $t"
        else
            printf '%s\tFAIL\t%d\n' "$t" "$dt" >> "$OUT/tests.tsv"
            cp "$TMP/test.log" "$OUT/failures/${t%.sh}.log"
            fail=$((fail + 1)); echo "  FAIL  ($dt s) $t"
        fi
    done < "$OUT/inventory.tsv"
    rm -f "$TMP/test.log"

    # ------------------------------------------------------------ 总结
    {
        echo "# 迁移基线（$(now)）"
        echo
        echo "- 分支 / 提交：\`$(git rev-parse --abbrev-ref HEAD)\` / \`$(git rev-parse --short HEAD)\`"
        echo "- 测试：$total 个，PASS $pass，FAIL $fail，TIMEOUT $timedout，MANUAL $skip"
        echo "- 全局：\`globals.txt\`（$(grep -c '^interface:' "$OUT/globals.txt" 2>/dev/null || echo 0) 个 interface）"
        echo "- 着色器基线：见 \`state-summary.txt\`"
        echo
        echo "## 必须守住的功能 ↔ 判据"
        echo
        echo "| 功能 | 测试 |"
        echo "| --- | --- |"
        echo "| 直通 / 合成像素 | test-scanout.sh |"
        echo "| 多屏 / 缩放 / 位深 / VRR | test-colordepth.sh · test-outputstate.sh · test-outputpos.sh · test-dpi.sh · test-mixdpi.sh |"
        echo "| 模糊全家 | test-blur*.sh · test-layerblur.sh · test-dragblur-*.sh |"
        echo "| 装饰 / 圆角 / 阴影 | test-corner.sh · test-openanim.sh · test-closeanim.sh |"
        echo "| 果冻 / 拖拽 | test-jelly.sh · test-drag*.sh |"
        echo "| 调色 / 色彩管理 | test-grade.sh · test-gradelut.sh |"
        echo "| HDR | (没有独立测试，靠 test-colordepth 的位深断言 + 实机) |"
        echo "| 抓帧 / 录屏 | test-capture-hide.sh · test-screencast.sh · test-portal*.sh · test-wincapture-*.sh |"
        echo "| XWayland | test-xwayland-*.sh · test-x11*.sh · test-child-coords.sh |"
        echo "| 会话 / 输入 | test-sessionlock.sh · test-pointer-lock.sh · test-keyboard-grab.sh · test-input-inject.sh |"
        echo "| 场景 / 布局 | test-workspace.sh · test-ws-dynamic.sh · test-overview.sh · test-fullscreen-*.sh |"
        echo
        echo "## 失败 / 超时（先判断是环境问题还是真回归）"
        echo
        echo "分类与初步原因见 \`known-failures.md\`。"
        echo
        if [ "$fail" = 0 ] && [ "$timedout" = 0 ]; then
            echo "无。"
        else
            awk -F'\t' 'NR>1 && $2!="PASS" && $2!="MANUAL" {n=$1; sub(/\.sh$/, "", n); printf "- %s — %s（%s s，日志 failures/%s.log）\n", $1, $2, $3, n}' "$OUT/tests.tsv"
        fi
    } > "$OUT/summary.md"
    echo
    echo "写出 $OUT/summary.md（PASS $pass / FAIL $fail / TIMEOUT $timedout / MANUAL $skip）"
else
    echo "跳过测试（--no-tests）"
fi
rm -rf "$TMP"
