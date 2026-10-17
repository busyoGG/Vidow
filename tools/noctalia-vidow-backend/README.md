# Noctalia ← vidow native backend（最小可用版）

## 为什么需要

Noctalia 的 dock 自动隐藏需要「当前工作区是否被占满」。它现在只有两条专属通道：

| 合成器 | 通道 | 代码 |
|---|---|---|
| Hyprland | `hyprland_toplevel_mapping_manager_v1` | `src/compositors/hyprland/` |
| niri | IPC（`NiriRuntime`） | `src/compositors/niri/` |
| 其它（vidow） | 只有 `ext-workspace-v1` + `zwlr_foreign_toplevel` | — |

而 `src/wayland/wayland_toplevels.h` 里的 `WlrToplevelSnapshot` 只记
`activated` / `minimized`，**不记 `maximized` / `fullscreen`** —— 所以 vidow 下
dock 永远拿不到「占满」，永远不隐藏。

## 这个 backend 做什么

走 vidow 自己的 IPC（`$VIDOW_SOCKET`，一行命令 + JSON 回复）算出同一个信息：
**当前活动工作区里有没有 fullscreen，或 column_width ≥ 1.0 的窗口**。

vidow 侧已经就绪（commit `09d9d90`）：`vidowmsg state` 的窗口 JSON 里有
`fullscreen` 和 `column_width`。

## 接入（三处）

1. **`src/compositors/compositor_detect.h`** 加一个探测：

   ```cpp
   inline bool isVidow() { return std::getenv("VIDOW_SOCKET") != nullptr; }
   ```

2. **`src/compositors/compositor_platform.h`** 加成员：

   ```cpp
   #include "compositors/vidow/vidow_runtime.h"
   ...
   std::unique_ptr<vidow::VidowRuntime> m_vidow;
   ```

   并在 `initialize()` 里 `m_vidow = std::make_unique<vidow::VidowRuntime>();`，
   加一个 `[[nodiscard]] bool isVidow() const noexcept { return m_vidow && m_vidow->available(); }`。

3. **dock 的自动隐藏判据**（`src/shell/dock/` 里那个算「要不要藏」的函数）加一条：

   ```cpp
   if (platform.isVidow() && platform.vidowWorkspaceFull(outputName)) {
     return true;  // 占满了 → 藏
   }
   ```

## 验证

```bash
# 在 vidow 会话里
./tools/vidow-workspace-full.py        # 0 = 没占满，1 = 占满
```

窗口半屏（默认 0.5）→ 0；把列拉满或全屏 → 1。backend 用的是同一套判据。

## 注意

- 失败一律返回 `false`（宁可让 dock 显示着，也不要因为查询失败把它藏起来）。
- `state` 是纯查询，不会阻塞；socket 带 `SOCK_CLOEXEC`。
