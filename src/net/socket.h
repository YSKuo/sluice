#pragma once

#include <cstddef>
#include <span>
#include <utility>

#include "net/inet_address.h"
#include "net/unique_fd.h"

namespace sluice::net {

// Outcome of one non-blocking read or write. On success `error` is 0 and
// `bytes` is the number of bytes transferred (a read of 0 bytes means the
// peer closed its write side). On failure `error` holds the errno value,
// e.g. EAGAIN when the call would have blocked.
struct IoResult {
  size_t bytes = 0;
  int error = 0;
};

// Outcome of accept(). On success `fd` is a valid, non-blocking,
// close-on-exec connected socket; on failure `error` holds the errno value.
struct AcceptResult {
  UniqueFd fd;
  InetAddress peer;
  int error = 0;
};

// An IPv4 TCP socket. Owns its descriptor through UniqueFd and wraps the
// syscalls the gateway needs. Every method reports failure through its
// return value (0 / IoResult::error == 0 on success, otherwise errno);
// EINTR is retried internally and never reported.
class Socket {
 public:
  explicit Socket(UniqueFd fd) noexcept : fd_(std::move(fd)) {}

  // Creates a non-blocking, close-on-exec IPv4 TCP socket. On failure the
  // returned socket is invalid and `*error` holds errno.
  static Socket CreateNonBlockingTcp(int* error);

  [[nodiscard]] int fd() const noexcept { return fd_.get(); }
  [[nodiscard]] bool valid() const noexcept { return fd_.valid(); }

  [[nodiscard]] int SetReuseAddr(bool on);
  [[nodiscard]] int SetNoDelay(bool on);
  [[nodiscard]] int Bind(const InetAddress& addr);
  [[nodiscard]] int Listen(int backlog);
  [[nodiscard]] AcceptResult Accept();

  [[nodiscard]] IoResult Read(std::span<char> buf);
  // Uses MSG_NOSIGNAL, so writing to a peer that has gone away returns
  // EPIPE instead of killing the process with SIGPIPE.
  [[nodiscard]] IoResult Write(std::span<const char> data);

  // Address the socket is bound to; useful after binding to port 0.
  [[nodiscard]] InetAddress LocalAddress(int* error) const;
  // Reads and clears the pending socket error (SO_ERROR), e.g. after
  // epoll reports EPOLLERR. Returns 0 when no error is pending.
  [[nodiscard]] int TakeError() const;

 private:
  UniqueFd fd_;
};

}  // namespace sluice::net
