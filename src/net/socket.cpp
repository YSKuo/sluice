#include "net/socket.h"

#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace sluice::net {

namespace {

// Converts the 0 / -1 return convention of most syscalls into a Result.
Result<void> CheckSyscall(int rc) {
  if (rc != 0) {
    return LastErrnoError();
  }
  return {};
}

Result<void> SetIntOption(int fd, int level, int name, int value) {
  return CheckSyscall(::setsockopt(fd, level, name, &value, sizeof(value)));
}

// Converts a read()/send() style return value into a byte count.
Result<size_t> ToByteCount(ssize_t n) {
  if (n < 0) {
    return LastErrnoError();
  }
  return static_cast<size_t>(n);
}

}  // namespace

Result<Socket> Socket::CreateNonBlockingTcp() {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                          IPPROTO_TCP);
  if (fd < 0) {
    return LastErrnoError();
  }
  return Socket(UniqueFd(fd));
}

Result<void> Socket::SetReuseAddr(bool on) {
  return SetIntOption(fd(), SOL_SOCKET, SO_REUSEADDR, on ? 1 : 0);
}

Result<void> Socket::SetNoDelay(bool on) {
  return SetIntOption(fd(), IPPROTO_TCP, TCP_NODELAY, on ? 1 : 0);
}

Result<void> Socket::Bind(const InetAddress& addr) {
  return CheckSyscall(::bind(fd(), addr.sockaddr_ptr(), InetAddress::length()));
}

Result<void> Socket::Listen(int backlog) {
  return CheckSyscall(::listen(fd(), backlog));
}

Result<AcceptedSocket> Socket::Accept() {
  sockaddr_in peer{};
  socklen_t len = sizeof(peer);
  int conn_fd = -1;
  do {
    conn_fd = ::accept4(fd(), reinterpret_cast<sockaddr*>(&peer), &len,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);
  } while (conn_fd < 0 && errno == EINTR);

  if (conn_fd < 0) {
    return LastErrnoError();
  }
  return AcceptedSocket{.socket = Socket(UniqueFd(conn_fd)),
                        .peer = InetAddress(peer)};
}

Result<size_t> Socket::Read(std::span<char> buf) {
  ssize_t n = -1;
  do {
    n = ::read(fd(), buf.data(), buf.size());
  } while (n < 0 && errno == EINTR);
  return ToByteCount(n);
}

Result<size_t> Socket::Write(std::span<const char> data) {
  ssize_t n = -1;
  do {
    n = ::send(fd(), data.data(), data.size(), MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);
  return ToByteCount(n);
}

Result<InetAddress> Socket::LocalAddress() const {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    return LastErrnoError();
  }
  return InetAddress(addr);
}

std::error_code Socket::TakeError() const {
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd(), SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
    err = errno;
  }
  return {err, std::system_category()};
}

}  // namespace sluice::net
