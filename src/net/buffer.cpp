#include "net/buffer.h"

#include <algorithm>

namespace sluice::net {

std::string_view Buffer::Peek() const noexcept {
  return {data_.data() + read_index_, ReadableBytes()};
}

void Buffer::Retrieve(size_t n) noexcept {
  if (n >= ReadableBytes()) {
    RetrieveAll();
    return;
  }
  read_index_ += n;
}

void Buffer::RetrieveAll() noexcept {
  // Rewinding to the start keeps the buffer from creeping forward and
  // makes the next EnsureWritable() a no-op in the common case.
  read_index_ = 0;
  write_index_ = 0;
}

std::string Buffer::RetrieveAllAsString() {
  std::string out(Peek());
  RetrieveAll();
  return out;
}

void Buffer::Append(std::string_view data) {
  if (data.empty()) {
    return;
  }
  std::ranges::copy(data, PrepareWrite(data.size()).begin());
  CommitWrite(data.size());
}

std::span<char> Buffer::PrepareWrite(size_t min_bytes) {
  EnsureWritable(min_bytes);
  return std::span(data_).subspan(write_index_);
}

void Buffer::CommitWrite(size_t n) noexcept {
  write_index_ = std::min(write_index_ + n, data_.size());
}

void Buffer::EnsureWritable(size_t n) {
  const size_t writable = data_.size() - write_index_;
  if (writable >= n) {
    return;
  }
  if (read_index_ + writable >= n) {
    // Enough total space once the consumed prefix is reclaimed. Copying
    // down to a lower address is safe for overlapping ranges.
    const auto readable =
        std::span(data_).subspan(read_index_, ReadableBytes());
    std::ranges::copy(readable, data_.begin());
    write_index_ = readable.size();
    read_index_ = 0;
    return;
  }
  data_.resize(write_index_ + n);
}

}  // namespace sluice::net
