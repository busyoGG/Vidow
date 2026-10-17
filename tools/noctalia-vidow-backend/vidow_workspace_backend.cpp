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

    // vidow 的 IPC 是 JSON 行协议：请求 {"cmd":"<命令>"}\n，应答
    // {"ok":true,"result":…}。发裸命令名会收到
    // {"ok":false,"error":"第 1 个字符：认不出来的值"}。
    std::string line = "{\"cmd\":\"" + cmd + "\"}\n";
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
      /* vidow 回的是**多行** JSON（缩进过的），所以不能「读到换行就停」——
       * 那样只会拿到第一行，parse 失败，工作区/窗口全空（实测 workspaces=0
       * windows=0，dock 因此永远认为活动工作区是空的）。按花括号配平判断。 */
      int depth = 0;
      bool started = false;
      for (char ch : out) {
        if (ch == '{' || ch == '[') {
          ++depth;
          started = true;
        } else if (ch == '}' || ch == ']') {
          --depth;
        }
      }
      if (started && depth <= 0) {
        break;
      }
    }
    close(fd);
    return out;
  }

  // vidow 的 JSON 里名字这类字段可能是 **null**（动态工作区没名字），而
  // nlohmann 的 value("name", "") 遇到 null 会抛 type_error.306（实测直接
  // 把进程干掉）。统一用这个取值器。
  std::string str_or_empty(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key)) {
      return {};
    }
    const auto& v = obj[key];
    return v.is_string() ? v.get<std::string>() : std::string{};
  }

  long long int_or_zero(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key)) {
      return 0;
    }
    const auto& v = obj[key];
    if (v.is_number_integer()) {
      return v.get<long long>();
    }
    if (v.is_number()) {
      return static_cast<long long>(v.get<double>());
    }
    return 0;
  }

  bool bool_or_false(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key)) {
      return false;
    }
    const auto& v = obj[key];
    return v.is_boolean() && v.get<bool>();
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
  if (doc.is_discarded() || !doc.is_object() || !doc.value("ok", false) ||
      !doc.contains("result")) {
    return result;
  }
  doc = doc["result"];

  /* 数据在**顶层** workspaces[]，不在 outputs[].workspaces[] —— 后者只有名字
   * （动态工作区就是 ["", ""]）。顶层每条带：id / idx / name / active /
   * output / window_count / windows。 */
  if (!doc.contains("workspaces") || !doc["workspaces"].is_array()) {
    return result;
  }
  const std::string wantOutput = outputNameFor(output);

  for (const auto& ws : doc["workspaces"]) {
    if (bool_or_false(ws, "special")) {
      continue; // 特殊工作区（scratchpad 那类）不进 dock 的工作区列表
    }
    const std::string wsOutput = str_or_empty(ws, "output");
    if (!wantOutput.empty() && wsOutput != wantOutput) {
      continue;
    }
    Workspace w;
    w.id = std::to_string(int_or_zero(ws, "id"));
    w.name = str_or_empty(ws, "name");
    w.index = static_cast<std::uint32_t>(int_or_zero(ws, "idx"));
    w.active = bool_or_false(ws, "active");
    // window_count 直接就是「这块工作区里有几个窗口」。
    w.occupied = int_or_zero(ws, "window_count") > 0;
    if (!w.occupied && ws.contains("windows") && ws["windows"].is_array()) {
      w.occupied = !ws["windows"].empty();
    }
    w.urgent = false;
    result.push_back(std::move(w));
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
  if (doc.is_discarded() || !doc.is_object() || !doc.value("ok", false) ||
      !doc.contains("result")) {
    return result;
  }
  doc = doc["result"];
  if (!doc.is_array()) {
    return result;
  }
  const std::string wantOutput = outputNameFor(output);

  for (const auto& win : doc) {
    if (!bool_or_false(win, "mapped") || bool_or_false(win, "popup")) {
      continue;
    }
    const std::string outName = str_or_empty(win, "output");
    if (!wantOutput.empty() && outName != wantOutput) {
      continue;
    }
    WorkspaceWindow w;
    w.windowId = std::to_string(int_or_zero(win, "id"));
    // vidow 的 workspace 字段是工作区**名字**；空串 = 没名字的动态工作区，
    // 用 id 兜底（和 workspaces[] 的 id 对齐）。
    w.workspaceKey = str_or_empty(win, "workspace");
    if (w.workspaceKey.empty() && win.contains("workspace_id") &&
        win["workspace_id"].is_number()) {
      // 动态工作区没有名字，vidow 给一个稳定编号（和 workspaces[] 的 id 一致）。
      // noctalia 的 workspaceKeyMatchesAssignment() 见到空串直接 false。
      w.workspaceKey = std::to_string(int_or_zero(win, "workspace_id"));
    }
    w.appId = str_or_empty(win, "app_id");
    w.title = str_or_empty(win, "title");
    if (win.contains("geometry") && win["geometry"].is_object()) {
      w.x = static_cast<std::int32_t>(int_or_zero(win["geometry"], "x"));
      w.y = static_cast<std::int32_t>(int_or_zero(win["geometry"], "y"));
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
  (void)id; // 见下：activate 需要带参数，等接入时补
  // TODO: 请求形状是 {"cmd":"focus-workspace","name":<id>}，
  // 参数键名以 vidow 的 ipc.c 为准（先不动，避免发错请求）。
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
