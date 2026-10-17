#pragma once
// vidow 的 WorkspaceBackend：让 Noctalia 看见 vidow 的工作区和窗口。
//
// 为什么需要：Noctalia 的工作区 / 窗口识别按合成器分别适配
// （compositors/hyprland/、compositors/niri/ …）。通用层只有 ext-workspace-v1
// 和 zwlr-foreign-toplevel，而这两个协议**都没有「哪个窗口在哪块工作区」**，
// 于是 vidow 下：
//   - 工作区列表是空的（ext-workspace 我实现了，但没人用它的 group 结构）
//   - dock 的 smart_auto_hide 永远认为「活动工作区是空的」→ 永不隐藏
//   - 任务栏窗口归属猜不出来
//
// 这个 backend 走 vidow 自己的 IPC（$VIDOW_SOCKET，一行命令 + JSON 回复）拿全部
// 信息。vidow 侧的 IPC 已经有：workspaces（name / active / id）、windows
// （app_id / title / workspace / geometry / output）。

#include "compositors/workspace_backend.h"

#include <string>
#include <vector>

namespace compositors::vidow {

class VidowWorkspaceBackend final : public WorkspaceBackend {
public:
  VidowWorkspaceBackend();
  ~VidowWorkspaceBackend() override;

  VidowWorkspaceBackend(const VidowWorkspaceBackend&) = delete;
  VidowWorkspaceBackend& operator=(const VidowWorkspaceBackend&) = delete;

  [[nodiscard]] bool available() const noexcept { return !m_socketPath.empty(); }
  [[nodiscard]] const std::string& socketPath() const noexcept { return m_socketPath; }

  [[nodiscard]] const char* backendName() const override { return "vidow"; }
  [[nodiscard]] bool isAvailable() const noexcept override { return available(); }
  void setChangeCallback(ChangeCallback callback) override;

  // 激活工作区：vidowmsg focus-workspace <id 或名字>
  void activate(const std::string& id) override;
  void activateForOutput(wl_output* output, const std::string& id) override;
  void activateForOutput(wl_output* output, const Workspace& workspace) override;

  [[nodiscard]] std::vector<Workspace> all() const override;
  [[nodiscard]] std::vector<Workspace> forOutput(wl_output* output) const override;

  [[nodiscard]] std::unordered_map<std::string, std::vector<std::string>>
  appIdsByWorkspace(wl_output* output) const override;
  [[nodiscard]] std::vector<WorkspaceWindow> workspaceWindows(wl_output* output) const override;
  [[nodiscard]] std::unordered_map<std::uintptr_t, WorkspaceWindow>
  assignTaskbarWindows(const std::vector<TaskbarWindowCandidate>& windows, wl_output* output) const override;

  void cleanup() override;

private:
  // 一次 state 查询 + 解析。失败返回空。
  [[nodiscard]] std::string queryState() const;
  // vidow 的输出名（"DP-3"）→ wl_output*，靠 Noctalia 的 WaylandConnection 反查。
  [[nodiscard]] std::string outputNameFor(wl_output* output) const;

  std::string m_socketPath;
  ChangeCallback m_changeCallback;
};

} // namespace compositors::vidow
