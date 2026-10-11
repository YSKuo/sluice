#ifndef SLUICE_NET_SOCKET_H_
#define SLUICE_NET_SOCKET_H_

#include <cstddef>
#include <span>
#include <system_error>
#include <utility>

#include "net/inet_address.h"
#include "net/unique_fd.h"
#include "result.h"

namespace sluice::net {

struct AcceptedSocket;

// An IPv4 TCP socket. Owns its descriptor through UniqueFd and wraps the
// syscalls the gateway needs. Failures are returned as errors in a Result;
// EINTR is retried internally and never reported. EAGAIN/EWOULDBLOCK is
// reported as an error (check it with IsWouldBlock()).
class Socket {
 public:
  // Creates a non-blocking, close-on-exec IPv4 TCP socket.
  static Result<Socket> CreateNonBlockingTcp();

  explicit Socket(UniqueFd fd) noexcept : fd_(std::move(fd)) {}

  [[nodiscard]] int fd() const noexcept { return fd_.get(); }

  [[nodiscard]] Result<void> SetReuseAddr(bool on);
  [[nodiscard]] Result<void> SetNoDelay(bool on);
  [[nodiscard]] Result<void> Bind(const InetAddress& addr);
  [[nodiscard]] Result<void> Listen(int backlog);
  // The accepted socket is non-blocking and close-on-exec.
  [[nodiscard]] Result<AcceptedSocket> Accept();

  // Returns the number of bytes read; 0 means the peer closed its write
  // side (EOF).
  [[nodiscard]] Result<size_t> Read(std::span<char> buf);
  // Returns the number of bytes written, which may be fewer than requested.
  // Uses MSG_NOSIGNAL, so writing to a peer that has gone away fails with
  // EPIPE instead of killing the process with SIGPIPE.
  [[nodiscard]] Result<size_t> Write(std::span<const char> data);

  // Address the socket is bound to; useful after binding to port 0.
  [[nodiscard]] Result<InetAddress> LocalAddress() const;
  // Reads and clears the pending socket error (SO_ERROR), e.g. after epoll
  // reports EPOLLERR. Returns an empty error_code when none is pending.
  [[nodiscard]] std::error_code TakeError() const;

 private:
  UniqueFd fd_;
};

// A connection returned by Socket::Accept().
struct AcceptedSocket {
  Socket socket;
  InetAddress peer;
};

}  // namespace sluice::net

#endif  // SLUICE_NET_SOCKET_H_
