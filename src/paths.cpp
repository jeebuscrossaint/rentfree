#include "paths.hpp"

#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/types.h>

namespace {

const char *env(const char *k) {
  const char *v = std::getenv(k);
  return (v && *v) ? v : nullptr;
}

std::string home() {
  if (const char *h = env("HOME")) return h;
  return ".";
}

std::string xdg(const char *var, const char *fallback) {
  if (const char *v = env(var)) return v;
  return home() + "/" + fallback;
}

} // namespace

namespace paths {

std::string todo_file() {
  if (const char *v = env("RENTFREE_FILE")) return v;
  return xdg("XDG_DATA_HOME", ".local/share") + "/rentfree/todo.md";
}

std::string config_file() {
  if (const char *v = env("RENTFREE_CONFIG")) return v;
  return xdg("XDG_CONFIG_HOME", ".config") + "/rentfree/config";
}

std::string socket_path() {
  if (const char *v = env("RENTFREE_SOCKET")) return v;
  std::string dir = env("XDG_RUNTIME_DIR") ? env("XDG_RUNTIME_DIR") : "/tmp";
  // One daemon per Wayland session, so nested compositors don't collide.
  std::string display = env("WAYLAND_DISPLAY") ? env("WAYLAND_DISPLAY") : "default";
  return dir + "/rentfree-" + display + ".sock";
}

bool ensure_parent_dir(const std::string &file) {
  auto slash = file.rfind('/');
  if (slash == std::string::npos || slash == 0) return true;
  std::string dir = file.substr(0, slash);

  // mkdir -p, one component at a time.
  for (size_t i = 1; i <= dir.size(); ++i) {
    if (i == dir.size() || dir[i] == '/') {
      std::string part = dir.substr(0, i);
      if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
  }
  return true;
}

} // namespace paths
