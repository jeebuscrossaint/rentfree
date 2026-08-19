#pragma once
#include <string>

namespace note {

struct Options {
  std::string todo_path;
  std::string config_path;
  std::string socket_path;
  bool foreground = false;
};

// Runs the note until told to quit. Returns a process exit code.
int run(const Options &opts);

} // namespace note
