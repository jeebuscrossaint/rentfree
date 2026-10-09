#pragma once
#include "config.hpp"

#include <string>
#include <vector>

namespace note {

struct Options {
  std::string todo_path;
  std::string config_path;
  std::string socket_path;
  bool foreground = false;
  // --key value pairs from the command line, applied over the config file.
  std::vector<Override> overrides;
};

// Runs the note until told to quit. Returns a process exit code.
int run(const Options &opts);

} // namespace note
