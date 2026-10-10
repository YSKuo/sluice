#include "net/buffer.h"

#include <algorithm>
#include <cstring>

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
    // An empty, never-grown buffer has a null data pointer, and passing
    // null to memcpy is undefined even for a length of zero.
    return;
  }
  std::span<char> space = PrepareWrite(data.size());
  std::memcpy(space.data(), data.data(), data.size());
  CommitWrite(data.size());
}

std::span<char> Buffer::PrepareWrite(size_t min_bytes) {
  EnsureWritable(min_bytes);
  return {data_.data() + write_index_, data_.size() - write_index_};
}

void Buffer::CommitWrite(size_t n) noexcept {
  write_index_ = std::min(write_index_ + n, data_.size());
}

void Buffer::EnsureWritable(size_t n) {
  const size_t writable = data_.size() - write_index_;
  if (writable >= n) {
    return;
  }
  const size_t readable = ReadableBytes();
  if (read_index_ + writable >= n) {
    // Enough total space once the consumed prefix is reclaimed.
    std::memmove(data_.data(), data_.data() + read_index_, readable);
    read_index_ = 0;
    write_index_ = readable;
    return;
  }
  data_.resize(write_index_ + n);
}

}  // namespace sluice::net
