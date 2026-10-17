#include "vidow_runtime.h"

#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace compositors::vidow {

namespace {

  // 把整段 JSON 里指定 key 后面紧跟的字符串值取出来（只用于我们自己的 state 格式，
  // 不做通用解析：字段名都是 ASCII，值里不会有转义引号）。
  bool findString(const std::string& json, std::string_view key, std::size_t from, std::size_t& valuePos,
                  std::size_t& valueLen) {
    std::string needle = "\"" + std::string(key) + "\":\"";
    std::size_t p = json.find(needle, from);
    if (p == std::string::npos) {
      return false;
    }
    std::size_t start = p + needle.size();
    std::size_t end = json.find('"', start);
    if (end == std::string::npos) {
      return false;
    }
    valuePos = start;
    valueLen = end - start;
    return true;
  }

} // namespace

VidowRuntime::VidowRuntime() {
  const char* path = std::getenv("VIDOW_SOCKET");
  if (path != nullptr && path[0] != '\0') {
    m_socketPath = path;
  }
}

VidowRuntime::~VidowRuntime() = default;

std::string VidowRuntime::queryState() const {
  if (m_socketPath.empty()) {
    return {};
  }
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    return {};
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (m_socketPath.size() >= sizeof(addr.sun_path)) {
    close(fd);
    return {};
  }
  std::memcpy(addr.sun_path, m_socketPath.c_str(), m_socketPath.size() + 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    close(fd);
    return {};
  }
  // 同步查询：发一行命令，读到 EOF 为止。state 是纯查询，不会卡住。
  std::string cmd = "state\n";
  if (write(fd, cmd.data(), cmd.size()) < 0) {
    close(fd);
    return {};
  }
  std::string out;
  char buf[8192];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    out.append(buf, static_cast<std::size_t>(n));
  }
  close(fd);
  return out;
}

bool VidowRuntime::workspaceFull(std::string_view outputName) const {
  std::string json = queryState();
  if (json.empty()) {
    return false;
  }

  // state 的形状（src/ipcstate.c）：
  //   {"outputs":[{"name":"DP-3","workspaces":[{"active":true,
  //       "windows":[{"fullscreen":false,"column_width":0.5,...}]}]}]}
  //
  // 判据：活动工作区里存在 fullscreen==true 或 column_width>=0.999 的窗口。
  // vidow 没有单独的 maximize，niri 也是拿整列宽度当的。
  const std::size_t activePos = json.find("\"active\":true");
  if (activePos == std::string::npos) {
    return false;
  }
  if (!outputName.empty()) {
    std::size_t namePos = 0, nameLen = 0;
    if (!findString(json, "name", 0, namePos, nameLen)) {
      return false;
    }
    if (json.compare(namePos, nameLen, outputName) != 0) {
      return false;
    }
  }

  // 从活动工作区往后扫：遇到下一个 "active": 说明出了这块工作区。
  std::size_t pos = activePos;
  for (;;) {
    std::size_t fs = json.find("\"fullscreen\":true", pos);
    std::size_t cw = json.find("\"column_width\":", pos);
    std::size_t nextWs = json.find("\"active\":", pos + 1);
    if (fs != std::string::npos && (nextWs == std::string::npos || fs < nextWs)) {
      return true;
    }
    if (cw != std::string::npos && (nextWs == std::string::npos || cw < nextWs)) {
      std::size_t v = cw + std::strlen("\"column_width\":");
      double width = std::strtod(json.c_str() + v, nullptr);
      if (width >= 0.999) {
        return true;
      }
      pos = v;
      continue;
    }
    return false;
  }
}

} // namespace compositors::vidow
