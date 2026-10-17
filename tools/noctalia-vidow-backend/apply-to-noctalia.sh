#!/bin/bash
# 把 vidow backend 装进一个 noctalia 源码树。
#
#   ./apply-to-noctalia.sh ~/src/noctalia
#
# 幂等：跑第二遍不会重复插。改三处（compositor_detect.h / .cpp / meson.build），
# 并把两个源文件拷进 src/compositors/vidow/。
set -euo pipefail
TREE=${1:?用法: $0 <noctalia 源码目录>}
[ -d "$TREE/src/compositors" ] || { echo "不是 noctalia 源码树: $TREE"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)

mkdir -p "$TREE/src/compositors/vidow"
cp "$HERE/vidow_workspace_backend.h" "$HERE/vidow_workspace_backend.cpp" \
   "$TREE/src/compositors/vidow/"
echo "① 源文件拷好"

python3 - "$TREE" <<'PY'
import pathlib, sys
tree = pathlib.Path(sys.argv[1])

# ② 枚举 + isVidow()
h = tree / "src/compositors/compositor_detect.h"
s = h.read_text()
if "Vidow = 9" not in s:
    s = s.replace("    Umbriel = 8,\n", "    Umbriel = 8,\n    Vidow = 9,\n", 1)
    s = s.replace("  [[nodiscard]] inline bool isUmbriel() { return detect() == CompositorKind::Umbriel; }",
                  "  [[nodiscard]] inline bool isUmbriel() { return detect() == CompositorKind::Umbriel; }\n"
                  "  [[nodiscard]] inline bool isVidow() { return detect() == CompositorKind::Vidow; }", 1)
    h.write_text(s)
    print("② compositor_detect.h ✓")
else:
    print("② compositor_detect.h 已经改过")

# ③ detectImpl 认 $VIDOW_SOCKET
c = tree / "src/compositors/compositor_detect.cpp"
s = c.read_text()
if "VIDOW_SOCKET" not in s:
    anchor = '      if (const char* v = std::getenv("UMBRIEL_SOCKET")'
    add = ('      // vidow 给每个子进程设了 VIDOW_SOCKET（见 vidow 的 src/env.c），\n'
           '      // 和 UMBRIEL_SOCKET / NIRI_SOCKET 是同一类信号。\n'
           '      if (const char* v = std::getenv("VIDOW_SOCKET"); v != nullptr && v[0] != \'\\0\') {\n'
           '        return CompositorKind::Vidow;\n'
           '      }\n')
    assert anchor in s, "compositor_detect.cpp 的锚点变了"
    s = s.replace(anchor, add + anchor, 1)
    # 桌面提示那条兜底
    s = s.replace('      if (StringUtils::containsInsensitive(hint, "umbriel")) {',
                  '      if (StringUtils::containsInsensitive(hint, "vidow")) {\n'
                  '        return CompositorKind::Vidow;\n'
                  '      }\n'
                  '      if (StringUtils::containsInsensitive(hint, "umbriel")) {', 1)
    c.write_text(s)
    print("③ compositor_detect.cpp ✓")
else:
    print("③ compositor_detect.cpp 已经改过")

# ④ meson.build 源列表
m = tree / "meson.build"
s = m.read_text()
if "vidow_workspace_backend.cpp" not in s:
    import re
    # 找第一个 compositors 源的写法：'src/compositors/...cpp'
    mm = re.search(r"( *)'src/compositors/[^']+\.cpp',\n", s)
    assert mm, "meson.build 里找不到 compositors 源列表"
    s = s[:mm.start()] + f"{mm.group(1)}'src/compositors/vidow/vidow_workspace_backend.cpp',\n" + s[mm.start():]
    m.write_text(s)
    print("④ meson.build ✓")
else:
    print("④ meson.build 已经改过")
PY

cat <<'EOT'

装好了。还差**最后一处**（注册），因为每个 runtime 的注册写法不同，需要手改：

  src/compositors/compositor_runtime_registry.cpp（或 compositor_platform.cpp）
  里照 niri / hyprland 那一段加：

    #include "compositors/vidow/vidow_workspace_backend.h"
    ...
    if (compositors::isVidow()) {
      auto backend = std::make_unique<compositors::vidow::VidowWorkspaceBackend>();
      if (backend->available()) {
        <注册 WorkspaceBackend 的那句>(std::move(backend));
      }
    }

然后：
  meson setup build && ninja -C build
EOT
