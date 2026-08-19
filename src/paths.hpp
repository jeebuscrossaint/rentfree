#pragma once
#include <string>

namespace paths {

// ~/.local/share/rentfree/todo.md  (or $RENTFREE_FILE)
std::string todo_file();
// ~/.config/rentfree/config  (or $RENTFREE_CONFIG)
std::string config_file();
// $XDG_RUNTIME_DIR/rentfree-$WAYLAND_DISPLAY.sock  (or $RENTFREE_SOCKET)
std::string socket_path();

// mkdir -p on the directory containing `file`.
bool ensure_parent_dir(const std::string &file);

} // namespace paths
