#include "net/socket.h"

#include <netinet/tcp.h>
#include <sys/socket.h>

#include <cerrno>

namespace sluice::net {

namespace {

int SetIntOption(int fd, int level, int name, int value) {
  if (::setsockopt(fd, level, name, &value, sizeof(value)) != 0) {
    return errno;
  }
  return 0;
}

}  // namespace

Socket Socket::CreateNonBlockingTcp(int* error) {
  const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);
  *error = fd < 0 ? errno : 0;
  return Socket(UniqueFd(fd));
}

int Socket::SetReuseAddr(bool on) {
  return SetIntOption(fd(), SOL_SOCKET, SO_REUSEADDR, on ? 1 : 0);
}

int Socket::SetNoDelay(bool on) { return SetIntOption(fd(), IPPROTO_TCP, TCP_NODELAY, on ? 1 : 0); }

int Socket::Bind(const InetAddress& addr) {
  if (::bind(fd(), addr.sockaddr_ptr(), InetAddress::length()) != 0) {
    return errno;
  }
  return 0;
}

int Socket::Listen(int backlog) {
  if (::listen(fd(), backlog) != 0) {
    return errno;
  }
  return 0;
}

AcceptResult Socket::Accept() {
  AcceptResult result;
  sockaddr_in peer{};
  socklen_t len = sizeof(peer);
  int conn_fd = -1;
  do {
    conn_fd =
        ::accept4(fd(), reinterpret_cast<sockaddr*>(&peer), &len, SOCK_NONBLOCK | SOCK_CLOEXEC);
  } while (conn_fd < 0 && errno == EINTR);

  if (conn_fd < 0) {
    result.error = errno;
    return result;
  }
  result.fd.Reset(conn_fd);
  result.peer = InetAddress(peer);
  return result;
}

IoResult Socket::Read(std::span<char> buf) {
  ssize_t n = -1;
  do {
    n = ::read(fd(), buf.data(), buf.size());
  } while (n < 0 && errno == EINTR);

  if (n < 0) {
    return IoResult{.bytes = 0, .error = errno};
  }
  return IoResult{.bytes = static_cast<size_t>(n), .error = 0};
}

IoResult Socket::Write(std::span<const char> data) {
  ssize_t n = -1;
  do {
    n = ::send(fd(), data.data(), data.size(), MSG_NOSIGNAL);
  } while (n < 0 && errno == EINTR);

  if (n < 0) {
    return IoResult{.bytes = 0, .error = errno};
  }
  return IoResult{.bytes = static_cast<size_t>(n), .error = 0};
}

InetAddress Socket::LocalAddress(int* error) const {
  sockaddr_in addr{};
  socklen_t len = sizeof(addr);
  if (::getsockname(fd(), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
    *error = errno;
    return InetAddress();
  }
  *error = 0;
  return InetAddress(addr);
}

int Socket::TakeError() const {
  int err = 0;
  socklen_t len = sizeof(err);
  if (::getsockopt(fd(), SOL_SOCKET, SO_ERROR, &err, &len) != 0) {
    return errno;
  }
  return err;
}

}  // namespace sluice::net
