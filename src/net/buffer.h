#ifndef SLUICE_NET_BUFFER_H_
#define SLUICE_NET_BUFFER_H_

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sluice::net {

// A growable byte queue used for per-connection input and output.
//
//   +-------------------+------------------+------------------+
//   | already consumed  |  readable bytes  |  writable space  |
//   +-------------------+------------------+------------------+
//   0               read_index_      write_index_        data_.size()
//
// Bytes are appended at write_index_ and consumed from read_index_. Space
// freed at the front is reclaimed by moving the readable bytes down before
// the buffer is ever grown.
class Buffer {
 public:
  [[nodiscard]] size_t ReadableBytes() const noexcept {
    return write_index_ - read_index_;
  }
  [[nodiscard]] bool empty() const noexcept { return ReadableBytes() == 0; }

  // View of the readable bytes. Invalidated by any non-const call.
  [[nodiscard]] std::string_view Peek() const noexcept;

  // Consumes `n` readable bytes (clamped to ReadableBytes()).
  void Retrieve(size_t n) noexcept;
  void RetrieveAll() noexcept;
  std::string RetrieveAllAsString();

  void Append(std::string_view data);

  // Two-step append for reading straight from a socket without an extra
  // copy: PrepareWrite() returns at least `min_bytes` of writable space,
  // the caller fills a prefix of it, then CommitWrite() publishes how many
  // bytes were actually written.
  [[nodiscard]] std::span<char> PrepareWrite(size_t min_bytes);
  void CommitWrite(size_t n) noexcept;

 private:
  void EnsureWritable(size_t n);

  std::vector<char> data_;
  size_t read_index_ = 0;
  size_t write_index_ = 0;
};

}  // namespace sluice::net

#endif  // SLUICE_NET_BUFFER_H_
