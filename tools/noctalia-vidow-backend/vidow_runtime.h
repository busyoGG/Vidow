#pragma once
// vidow native backend for Noctalia —— 最小可用版。
//
// 为什么需要：Noctalia 的 dock 自动隐藏在 Hyprland 走 hyprland_toplevel_mapping_manager_v1、
// 在 niri 走 NiriRuntime IPC；通用路径（zwlr_foreign_toplevel）的 WlrToplevelSnapshot
// 只记 activated / minimized，**不记 maximized / fullscreen**，所以 vidow 下 dock 永远
// 不知道工作区被占满、永远不隐藏。
//
// 这个 backend 走 vidow 自己的 IPC（$VIDOW_SOCKET，一行命令 + JSON 回复）把同一个信息
// 算出来：当前活动工作区里有没有 fullscreen 或 column_width >= 1.0 的窗口。
//
// 接入点（Noctalia 侧）：
//   1. compositors/compositor_detect.h  加 isVidow()（认 $VIDOW_SOCKET）
//   2. compositor_platform.h            加 std::unique_ptr<VidowRuntime> m_vidow;
//   3. dock 的「占满就隐藏」判据里，platform.isVidow() 时用 vidow.workspaceFull(output)

#include <string>
#include <string_view>

struct wl_output;

namespace compositors::vidow {

class VidowRuntime {
public:
  VidowRuntime();
  ~VidowRuntime();
  VidowRuntime(const VidowRuntime&) = delete;
  VidowRuntime& operator=(const VidowRuntime&) = delete;

  // $VIDOW_SOCKET 存在且能连上 = 在 vidow 会话里。
  [[nodiscard]] bool available() const noexcept { return m_socketPath[0] != '\0'; }
  [[nodiscard]] const std::string& socketPath() const noexcept { return m_socketPath; }

  // 当前活动工作区是否被占满（有全屏窗口，或列宽 >= 1.0）。
  // outputName 为空 = 任意一块输出占满就算。
  // 失败（连不上 / 回复不是 JSON）一律返回 false —— 宁可让 dock 显示着，
  // 也不要因为合成器查询失败把 dock 藏起来。
  [[nodiscard]] bool workspaceFull(std::string_view outputName = {}) const;

  // 原始 state 查询（调试用；返回空串表示失败）。
  [[nodiscard]] std::string queryState() const;

private:
  std::string m_socketPath;
};

} // namespace compositors::vidow
