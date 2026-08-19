#pragma once
#include <functional>
#include <string>

namespace ipc {

// Control channel between the CLI and a running daemon. Note that todo
// *content* never travels over this socket -- the file is the source of truth
// and the daemon picks changes up via inotify. This carries only things that
// have no on-disk representation: show/hide, reload, quit.

class Server {
public:
  ~Server();

  // Binds the socket, clearing a stale one left by a crashed daemon.
  // Fails if another daemon is genuinely alive.
  bool listen(const std::string &path, std::string &err);

  int fd() const { return fd_; }

  // Call when fd() is readable. `handler` maps a request line to a reply.
  void accept_once(const std::function<std::string(const std::string &)> &handler);

  void close();

private:
  int fd_ = -1;
  std::string path_;
};

// Sends one command to a running daemon. Returns false if nothing is
// listening (i.e. no daemon), with the reply in `reply` on success.
bool request(const std::string &path, const std::string &cmd, std::string &reply);

// True if a daemon currently answers on `path`.
bool daemon_alive(const std::string &path);

} // namespace ipc
