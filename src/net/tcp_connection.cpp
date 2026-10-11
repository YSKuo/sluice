#include "net/tcp_connection.h"

#include <span>
#include <utility>

namespace sluice::net {

TcpConnection::TcpConnection(EventLoop* loop, uint64_t id, Socket socket,
                             const InetAddress& peer)
    : id_(id),
      peer_(peer),
      socket_(std::move(socket)),
      channel_(loop, socket_.fd()) {
  channel_.SetReadCallback([this] { HandleRead(); });
  channel_.SetWriteCallback([this] { HandleWrite(); });
  channel_.SetCloseCallback([this] { HandleClose(); });
  // EPOLLERR: the socket is unusable, whatever the pending error is.
  channel_.SetErrorCallback([this] { HandleClose(); });
}

TcpConnection::~TcpConnection() { channel_.Remove(); }

Result<void> TcpConnection::Start() { return channel_.EnableReading(); }

void TcpConnection::Send(std::string_view data) {
  if (state_ == State::kClosed) {
    return;
  }

  size_t written = 0;
  // Only write directly when nothing is queued; otherwise the new bytes
  // would overtake the queued ones.
  if (!channel_.IsWriting() && output_.empty()) {
    const Result<size_t> result = socket_.Write(data);
    if (result) {
      written = *result;
    } else if (!IsWouldBlock(result.error())) {
      // EPIPE, ECONNRESET, ...: the peer is gone.
      HandleClose();
      return;
    }
  }

  if (written == data.size()) {
    return;
  }
  output_.Append(data.substr(written));
  if (!channel_.IsWriting() && !channel_.EnableWriting()) {
    HandleClose();
  }
}

void TcpConnection::PauseReading() {
  if (state_ == State::kConnected && channel_.IsReading()) {
    channel_.DisableReading();
  }
}

void TcpConnection::ResumeReading() {
  if (state_ == State::kConnected && !channel_.IsReading() &&
      !channel_.EnableReading()) {
    HandleClose();
  }
}

void TcpConnection::Shutdown() {
  if (state_ == State::kConnected) {
    BeginDrain();
  }
}

void TcpConnection::ForceClose() { HandleClose(); }

void TcpConnection::HandleRead() {
  const std::span<char> space = input_.PrepareWrite(kReadChunkSize);
  const Result<size_t> result = socket_.Read(space);
  if (!result) {
    if (!IsWouldBlock(result.error())) {
      HandleClose();  // ECONNRESET, ...
    }
    return;
  }
  if (*result == 0) {
    // The peer finished sending (EOF). Flush whatever we still owe it,
    // then close.
    BeginDrain();
    return;
  }
  input_.CommitWrite(*result);
  if (message_cb_) {
    message_cb_(*this, input_);
  }
}

void TcpConnection::HandleWrite() {
  const Result<size_t> result = socket_.Write(output_.Peek());
  if (!result) {
    if (!IsWouldBlock(result.error())) {
      HandleClose();
    }
    return;
  }
  output_.Retrieve(*result);
  if (!output_.empty()) {
    return;  // Still more to flush; wait for the next EPOLLOUT.
  }

  // Stop asking for EPOLLOUT: with level-triggered epoll an idle socket is
  // almost always writable, so leaving it enabled would wake the loop on
  // every iteration for nothing.
  channel_.DisableWriting();
  if (state_ == State::kDraining) {
    HandleClose();
    return;
  }
  if (write_complete_cb_) {
    write_complete_cb_(*this);
  }
}

void TcpConnection::HandleClose() {
  if (state_ == State::kClosed) {
    return;
  }
  state_ = State::kClosed;
  // Unregister now so no further events reach this connection; the socket
  // itself is closed when the owner destroys the connection.
  channel_.Remove();
  if (close_cb_) {
    close_cb_(*this);
  }
}

void TcpConnection::BeginDrain() {
  if (output_.empty()) {
    HandleClose();
    return;
  }
  state_ = State::kDraining;
  channel_.DisableReading();
}

}  // namespace sluice::net
