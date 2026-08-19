#include "ipc.hpp"

#include <cerrno>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

bool fill_addr(sockaddr_un &addr, const std::string &path, std::string &err) {
  std::memset(&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) {
    err = "socket path too long: " + path;
    return false;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
  return true;
}

bool write_all(int fd, const std::string &s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::write(fd, s.data() + off, s.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += static_cast<size_t>(n);
  }
  return true;
}

std::string read_all(int fd) {
  std::string out;
  char buf[512];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) break;
    out.append(buf, static_cast<size_t>(n));
    if (out.size() > 64 * 1024) break; // control messages are tiny
  }
  return out;
}

} // namespace

namespace ipc {

Server::~Server() { close(); }

void Server::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  if (!path_.empty()) {
    ::unlink(path_.c_str());
    path_.clear();
  }
}

bool Server::listen(const std::string &path, std::string &err) {
  // A leftover socket file from a killed daemon would block bind(), but so
  // would a live daemon -- tell them apart by trying to talk to it.
  if (daemon_alive(path)) {
    err = "a rentfree daemon is already running";
    return false;
  }
  ::unlink(path.c_str());

  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    err = std::string("socket: ") + std::strerror(errno);
    return false;
  }

  sockaddr_un addr;
  if (!fill_addr(addr, path, err)) {
    ::close(fd);
    return false;
  }

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
    err = std::string("bind ") + path + ": " + std::strerror(errno);
    ::close(fd);
    return false;
  }
  if (::listen(fd, 8) != 0) {
    err = std::string("listen: ") + std::strerror(errno);
    ::close(fd);
    ::unlink(path.c_str());
    return false;
  }

  fd_ = fd;
  path_ = path;
  return true;
}

void Server::accept_once(const std::function<std::string(const std::string &)> &handler) {
  int c = ::accept(fd_, nullptr, nullptr);
  if (c < 0) return;

  std::string req = read_all(c);
  while (!req.empty() && (req.back() == '\n' || req.back() == '\r')) req.pop_back();

  std::string reply = handler(req);
  if (reply.empty() || reply.back() != '\n') reply += "\n";
  write_all(c, reply);

  ::shutdown(c, SHUT_RDWR);
  ::close(c);
}

bool request(const std::string &path, const std::string &cmd, std::string &reply) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;

  sockaddr_un addr;
  std::string err;
  if (!fill_addr(addr, path, err)) {
    ::close(fd);
    return false;
  }

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return false;
  }

  if (!write_all(fd, cmd + "\n")) {
    ::close(fd);
    return false;
  }
  ::shutdown(fd, SHUT_WR); // let the daemon see EOF and answer

  reply = read_all(fd);
  while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
  ::close(fd);
  return true;
}

bool daemon_alive(const std::string &path) {
  std::string reply;
  return request(path, "ping", reply);
}

} // namespace ipc
