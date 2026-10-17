#!/bin/bash
# 把 vidow backend 装进一个 noctalia 源码树。
#
#   ./apply-to-noctalia.sh ~/src/noctalia-main
#
# 幂等：跑第二遍不会重复插。改五处，并把两个源文件拷进 src/compositors/vidow/。
set -euo pipefail
TREE=${1:?用法: $0 <noctalia 源码目录>}
[ -d "$TREE/src/compositors" ] || { echo "不是 noctalia 源码树: $TREE"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$TREE/src/compositors/vidow"
cp "$HERE/vidow_workspace_backend.h" "$HERE/vidow_workspace_backend.cpp" \
   "$TREE/src/compositors/vidow/"
echo "① 源文件拷好"

python3 - "$TREE" <<'PY'
import pathlib, re, sys
tree = pathlib.Path(sys.argv[1])

# ② 枚举 + isVidow()
h = tree / "src/compositors/compositor_detect.h"
s = h.read_text()
if "Vidow = 9" not in s:
    s = s.replace("    Umbriel = 8,\n", "    Umbriel = 8,\n    Vidow = 9,\n", 1)
    s = s.replace(
        "  [[nodiscard]] inline bool isUmbriel() { return detect() == CompositorKind::Umbriel; }",
        "  [[nodiscard]] inline bool isUmbriel() { return detect() == CompositorKind::Umbriel; }\n"
        "  [[nodiscard]] inline bool isVidow() { return detect() == CompositorKind::Vidow; }", 1)
    h.write_text(s); print("② compositor_detect.h ✓")
else:
    print("② compositor_detect.h 已经改过")

# ③ detectImpl 认 $VIDOW_SOCKET
c = tree / "src/compositors/compositor_detect.cpp"
s = c.read_text()
if "VIDOW_SOCKET" not in s:
    anchor = '      if (const char* v = std::getenv("UMBRIEL_SOCKET")'
    assert anchor in s, "compositor_detect.cpp 的锚点变了"
    add = ("      // vidow 给每个子进程设了 VIDOW_SOCKET，和 UMBRIEL_SOCKET /\n"
           "      // NIRI_SOCKET 是同一类信号。\n"
           "      if (const char* v = std::getenv(\"VIDOW_SOCKET\"); v != nullptr && v[0] != '\\0') {\n"
           "        return CompositorKind::Vidow;\n"
           "      }\n")
    s = s.replace(anchor, add + anchor, 1)
    s = s.replace('      if (StringUtils::containsInsensitive(hint, "umbriel")) {',
                  '      if (StringUtils::containsInsensitive(hint, "vidow")) {\n'
                  '        return CompositorKind::Vidow;\n'
                  '      }\n'
                  '      if (StringUtils::containsInsensitive(hint, "umbriel")) {', 1)
    c.write_text(s); print("③ compositor_detect.cpp ✓")
else:
    print("③ compositor_detect.cpp 已经改过")

# ④ meson.build 源列表
m = tree / "meson.build"
s = m.read_text()
if "vidow_workspace_backend.cpp" not in s:
    mm = re.search(r"( *)'src/compositors/[^']+\.cpp',\n", s)
    assert mm, "meson.build 里找不到 compositors 源列表"
    s = s[:mm.start()] + f"{mm.group(1)}'src/compositors/vidow/vidow_workspace_backend.cpp',\n" + s[mm.start():]
    m.write_text(s); print("④ meson.build ✓")
else:
    print("④ meson.build 已经改过")

# ⑤ wayland_workspaces.cpp：挂进 backend 链
w = tree / "src/wayland/wayland_workspaces.cpp"
s = w.read_text()
if "vidow_workspace_backend.h" not in s:
    inc = '#include "compositors/sway/sway_workspace_backend.h"'
    assert inc in s, "wayland_workspaces.cpp 的 include 锚点变了"
    s = s.replace(inc, inc + '\n#include "compositors/vidow/vidow_workspace_backend.h"', 1)
    anchor = "  auto kwinBackend = std::make_unique<KwinWorkspaceBackend>();"
    assert anchor in s, "wayland_workspaces.cpp 的构造锚点变了"
    add = (
        "\n"
        "  // vidow：走它自己的 IPC 拿工作区 + 窗口归属。ext-workspace-v1 没有\n"
        "  // 「哪个窗口在哪块工作区」，所以不挂这个的话 dock 的 smart_auto_hide\n"
        "  // 永远认为活动工作区是空的（= 永不隐藏）。没有 $VIDOW_SOCKET 就不挂。\n"
        "  auto vidowBackend = std::make_unique<compositors::vidow::VidowWorkspaceBackend>();\n"
        "  if (vidowBackend->available()) {\n"
        "    m_backends.push_back(std::move(vidowBackend));\n"
        "  }\n")
    s = s.replace(anchor, add + anchor, 1)
    w.write_text(s); print("⑤ wayland_workspaces.cpp ✓")
else:
    print("⑤ wayland_workspaces.cpp 已经改过")

# ⑥ 让 Vidow 成为 active backend（不然 tryFallback 里 ext-workspace 先赢，
#    我们的 backend 挂了却永远不被选中）
hh = tree / "src/wayland/wayland_workspaces.h"
s = hh.read_text()
if "m_vidowBackend" not in s:
    anchor = "  WorkspaceBackend* m_kwinBackend = nullptr;"
    assert anchor in s, "wayland_workspaces.h 的成员锚点变了"
    s = s.replace(anchor, anchor + "\n  WorkspaceBackend* m_vidowBackend = nullptr;", 1)
    hh.write_text(s); print("⑥ wayland_workspaces.h 成员 ✓")
else:
    print("⑥ wayland_workspaces.h 已经改过")

w = tree / "src/wayland/wayland_workspaces.cpp"
s = w.read_text()
if "m_vidowBackend = " not in s:
    anchor = "  if (vidowBackend->available()) {\n    m_backends.push_back(std::move(vidowBackend));\n  }"
    assert anchor in s, "vidowBackend 构造段变了"
    s = s.replace(anchor,
        "  m_vidowBackend = vidowBackend.get();\n"
        "  if (vidowBackend->available()) {\n"
        "    m_backends.push_back(std::move(vidowBackend));\n"
        "  }", 1)
    # initialize() 里加 case
    anchor2 = "  case compositors::CompositorKind::Mango:"
    assert anchor2 in s, "initialize() 的 switch 锚点变了"
    s = s.replace(anchor2,
        "  case compositors::CompositorKind::Vidow:\n"
        "    // vidow 自己那套（IPC）比 ext-workspace 知道得多：ext-workspace 只有\n"
        "    // 工作区列表，没有「哪个窗口在哪块工作区」，dock 的 smart_auto_hide\n"
        "    // 就永远认为活动工作区是空的。\n"
        "    if (m_vidowBackend != nullptr && m_vidowBackend->isAvailable()) {\n"
        "      setActiveBackend(m_vidowBackend);\n"
        "      return;\n"
        "    }\n"
        "    break;\n" + anchor2, 1)
    w.write_text(s); print("⑥ initialize() 加 Vidow case ✓")
else:
    print("⑥ initialize() 已经改过")
PY

echo "五处改完。下一步：cd $TREE && meson setup build && ninja -C build"
