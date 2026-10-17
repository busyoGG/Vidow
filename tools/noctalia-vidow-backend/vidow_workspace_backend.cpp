#include "vidow_workspace_backend.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace compositors::vidow {

namespace {

  constexpr const char* kLogTag = "[vidow]";

  // vidow IPC：发一行命令，读到 EOF。state / windows 都是纯查询，不会卡住。
  std::string vidowQuery(const std::string& socketPath, const std::string& cmd) {
    if (socketPath.empty()) {
      return {};
    }
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return {};
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(addr.sun_path)) {
      close(fd);
      return {};
    }
    std::memcpy(addr.sun_path, socketPath.c_str(), socketPath.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close(fd);
      return {};
    }
    // **必须有超时**：vidow 的 IPC 回完 JSON 不关连接（它是个长连接服务），
    // 裸 read() 会一直阻塞 —— 实测把 noctalia 主线程卡死在
    // unix_stream_read_generic，初始化走不完，连自己的 Wayland socket 都建不
    // 出来（症状：「noctalia is already running」但其实没起来）。
    struct timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 250000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string line = cmd + "\n";
    if (write(fd, line.data(), line.size()) < 0) {
      close(fd);
      return {};
    }
    std::string out;
    char buf[16384];
    for (;;) {
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n <= 0) {
        break; // EOF 或超时（超时也算「回复读完了」）
      }
      out.append(buf, static_cast<std::size_t>(n));
      // 一行 JSON 读完了就不等连接关闭。
      if (out.find('\n') != std::string::npos) {
        break;
      }
    }
    close(fd);
    return out;
  }

} // namespace

VidowWorkspaceBackend::VidowWorkspaceBackend() {
  const char* path = std::getenv("VIDOW_SOCKET");
  if (path != nullptr && path[0] != '\0') {
    m_socketPath = path;
  }
}

VidowWorkspaceBackend::~VidowWorkspaceBackend() = default;

void VidowWorkspaceBackend::setChangeCallback(ChangeCallback callback) {
  m_changeCallback = std::move(callback);
}

std::string VidowWorkspaceBackend::queryState() const {
  // **缓存**：dock / bar 每帧都会问工作区，每次一个 unix socket 往返（哪怕有
  // 超时）也会把帧时间拖长。50ms 的窗口足够跟上切换（人眼级别），又不会让
  // 每帧都进内核。
  const auto now = std::chrono::steady_clock::now();
  if (!m_stateCache.empty() &&
      now - m_stateStamp < std::chrono::milliseconds(50)) {
    return m_stateCache;
  }
  m_stateCache = vidowQuery(m_socketPath, "state");
  m_stateStamp = now;
  return m_stateCache;
}

std::string VidowWorkspaceBackend::outputNameFor(wl_output* output) const {
  // Noctalia 的 WaylandConnection 有 wl_output → 名字的映射，但它不在这个类的
  // 头文件依赖里。留空表示「不限输出」，调用方按需接上（见 README 的接入说明）。
  (void)output;
  return {};
}

std::vector<Workspace> VidowWorkspaceBackend::all() const {
  return forOutput(nullptr);
}

std::vector<Workspace> VidowWorkspaceBackend::forOutput(wl_output* output) const {
  std::vector<Workspace> result;
  const std::string raw = queryState();
  if (raw.empty()) {
    return result;
  }
  nlohmann::json doc = nlohmann::json::parse(raw, nullptr, false);
  if (doc.is_discarded() || !doc.contains("outputs")) {
    return result;
  }
  const std::string wantOutput = outputNameFor(output);

  // state 的形状：{"outputs":[{"name":"DP-3","workspaces":[{...}]}]}
  uint32_t index = 0;
  for (const auto& out : doc["outputs"]) {
    const std::string outName = out.value("name", "");
    if (!wantOutput.empty() && outName != wantOutput) {
      continue;
    }
    if (!out.contains("workspaces")) {
      continue;
    }
    for (const auto& ws : out["workspaces"]) {
      Workspace w;
      w.id = std::to_string(ws.value("id", static_cast<std::uint64_t>(0)));
      w.name = ws.value("name", "");
      w.index = index++;
      w.active = ws.value("active", false);
      // occupied = 这块工作区里有窗口。vidow 的 workspaces[] 带 windows 数组。
      w.occupied = ws.contains("windows") && !ws["windows"].empty();
      w.urgent = false;
      result.push_back(std::move(w));
    }
  }
  return result;
}

std::vector<WorkspaceWindow> VidowWorkspaceBackend::workspaceWindows(wl_output* output) const {
  std::vector<WorkspaceWindow> result;
  const std::string raw = vidowQuery(m_socketPath, "windows");
  if (raw.empty()) {
    return result;
  }
  nlohmann::json doc = nlohmann::json::parse(raw, nullptr, false);
  if (doc.is_discarded() || !doc.is_array()) {
    return result;
  }
  const std::string wantOutput = outputNameFor(output);

  for (const auto& win : doc) {
    if (!win.value("mapped", false) || win.value("popup", false)) {
      continue;
    }
    const std::string outName = win.value("output", "");
    if (!wantOutput.empty() && outName != wantOutput) {
      continue;
    }
    WorkspaceWindow w;
    w.windowId = std::to_string(win.value("id", static_cast<std::uint64_t>(0)));
    // vidow 的 workspace 字段是工作区**名字**；空串 = 没名字的动态工作区，
    // 用 id 兜底（和 workspaces[] 的 id 对齐）。
    w.workspaceKey = win.value("workspace", "");
    if (w.workspaceKey.empty() && win.contains("workspace_id") &&
        win["workspace_id"].is_number()) {
      // 动态工作区没有名字，vidow 给一个稳定编号（和 workspaces[] 的 id 一致）。
      // noctalia 的 workspaceKeyMatchesAssignment() 见到空串直接 false。
      w.workspaceKey = std::to_string(win["workspace_id"].get<long long>());
    }
    w.appId = win.value("app_id", "");
    w.title = win.value("title", "");
    if (win.contains("geometry") && win["geometry"].is_object()) {
      w.x = win["geometry"].value("x", 0);
      w.y = win["geometry"].value("y", 0);
    }
    w.outputName = outName;
    result.push_back(std::move(w));
  }
  return result;
}

std::unordered_map<std::string, std::vector<std::string>>
VidowWorkspaceBackend::appIdsByWorkspace(wl_output* output) const {
  std::unordered_map<std::string, std::vector<std::string>> result;
  for (const auto& w : workspaceWindows(output)) {
    result[w.workspaceKey].push_back(w.appId);
  }
  return result;
}

std::unordered_map<std::uintptr_t, WorkspaceWindow> VidowWorkspaceBackend::assignTaskbarWindows(
    const std::vector<TaskbarWindowCandidate>& windows, wl_output* output) const {
  std::unordered_map<std::uintptr_t, WorkspaceWindow> result;
  const auto byWorkspace = workspaceWindows(output);
  for (const auto& cand : windows) {
    for (const auto& w : byWorkspace) {
      bool match = false;
      for (const auto& appId : cand.appIds) {
        if (!appId.empty() && appId == w.appId) {
          match = true;
          break;
        }
      }
      if (match) {
        result[cand.handleKey] = w;
        break;
      }
    }
  }
  return result;
}

void VidowWorkspaceBackend::activate(const std::string& id) {
  if (id.empty()) {
    return;
  }
  // vidowmsg focus-workspace 认序号 / 名字；id 是稳定编号，两种都试。
  vidowQuery(m_socketPath, "focus-workspace " + id);
  if (m_changeCallback) {
    m_changeCallback();
  }
}

void VidowWorkspaceBackend::activateForOutput(wl_output* output, const std::string& id) {
  (void)output;
  activate(id);
}

void VidowWorkspaceBackend::activateForOutput(wl_output* output, const Workspace& workspace) {
  (void)output;
  activate(workspace.id);
}

void VidowWorkspaceBackend::cleanup() {
  m_changeCallback = nullptr;
}

} // namespace compositors::vidow
