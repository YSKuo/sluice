#ifndef SLUICE_RESULT_H_
#define SLUICE_RESULT_H_

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <system_error>
#include <utility>
#include <variant>

namespace sluice {

// The failure half of a Result: a non-zero system error code. Returning an
// Error from a function that returns Result<T> produces a failed Result.
class Error {
 public:
  explicit Error(std::error_code code) noexcept : code_(code) {}

  [[nodiscard]] const std::error_code& code() const noexcept { return code_; }

 private:
  std::error_code code_;
};

namespace internal {

[[noreturn]] inline void ResultMisuse(const char* what) {
  std::fprintf(stderr, "fatal: Result: %s\n", what);
  std::abort();
}

}  // namespace internal

// Either a value or the system error that prevented producing it. This is
// the project's error vocabulary: no exceptions on the hot path, and no
// output parameters.
//
// The interface is a small subset of C++23's std::expected<T,
// std::error_code> (has_value, operator bool, *, ->, error), so moving to
// std::expected later only changes this header. Accessing the value of a
// failed Result, or the error of a successful one, is a bug and aborts.
template <typename T>
class [[nodiscard]] Result {
 public:
  // Implicit, like std::expected, so a function can simply
  // `return value;` or `return Error(...);`.
  Result(T value)  // NOLINT(google-explicit-constructor)
      : storage_(std::in_place_index<0>, std::move(value)) {}
  Result(Error error)  // NOLINT(google-explicit-constructor)
      : storage_(std::in_place_index<1>, error.code()) {}

  [[nodiscard]] bool has_value() const noexcept {
    return storage_.index() == 0;
  }
  explicit operator bool() const noexcept { return has_value(); }

  T& operator*() & { return Value(); }
  const T& operator*() const& { return Value(); }
  T&& operator*() && { return std::move(Value()); }
  T* operator->() { return &Value(); }
  const T* operator->() const { return &Value(); }

  [[nodiscard]] const std::error_code& error() const {
    if (has_value()) {
      internal::ResultMisuse("error() called on a successful Result");
    }
    return std::get<1>(storage_);
  }

 private:
  T& Value() {
    if (!has_value()) {
      internal::ResultMisuse("value accessed on a failed Result");
    }
    return std::get<0>(storage_);
  }
  const T& Value() const {
    if (!has_value()) {
      internal::ResultMisuse("value accessed on a failed Result");
    }
    return std::get<0>(storage_);
  }

  std::variant<T, std::error_code> storage_;
};

// Success or an error, with no value.
template <>
class [[nodiscard]] Result<void> {
 public:
  // Success.
  Result() noexcept = default;
  Result(Error error) noexcept  // NOLINT(google-explicit-constructor)
      : error_(error.code()) {}

  [[nodiscard]] bool has_value() const noexcept { return !error_; }
  explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const std::error_code& error() const {
    if (has_value()) {
      internal::ResultMisuse("error() called on a successful Result");
    }
    return error_;
  }

 private:
  // An empty (zero) error_code means success.
  std::error_code error_;
};

// Wraps an errno value as an Error.
inline Error ErrnoError(int err) {
  return Error(std::error_code(err, std::system_category()));
}

// Wraps the current errno; call it right after the failing syscall, before
// anything else can overwrite errno.
inline Error LastErrnoError() { return ErrnoError(errno); }

// True for EAGAIN/EWOULDBLOCK: the non-blocking call would have blocked,
// which is not a failure.
inline bool IsWouldBlock(const std::error_code& error) {
  return error == std::errc::resource_unavailable_try_again ||
         error == std::errc::operation_would_block;
}

}  // namespace sluice

#endif  // SLUICE_RESULT_H_
