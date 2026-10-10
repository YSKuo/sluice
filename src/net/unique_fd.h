#pragma once

#include <unistd.h>

#include <utility>

namespace sluice::net {

// Owns a single POSIX file descriptor and closes it on destruction.
// Move-only: exactly one UniqueFd owns a given descriptor at any time, so a
// descriptor can be neither leaked nor closed twice.
class UniqueFd {
 public:
  UniqueFd() noexcept = default;
  explicit UniqueFd(int fd) noexcept : fd_(fd) {}
  ~UniqueFd() { Reset(); }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  UniqueFd(UniqueFd&& other) noexcept : fd_(other.Release()) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      Reset(other.Release());
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }

  // Gives up ownership without closing; the caller becomes responsible for
  // the returned descriptor.
  [[nodiscard]] int Release() noexcept { return std::exchange(fd_, -1); }

  // Closes the owned descriptor (if any) and takes ownership of `fd`.
  void Reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      // The result of close() is deliberately ignored: on Linux the
      // descriptor is released even when close() fails (including EINTR),
      // so retrying could close an unrelated descriptor that has already
      // reused the same number.
      static_cast<void>(::close(fd_));
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace sluice::net
