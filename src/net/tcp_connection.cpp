#include "net/tcp_connection.h"

#include <cerrno>

namespace sluice::net {

namespace {

bool WouldBlock(int err) { return err == EAGAIN || err == EWOULDBLOCK; }

}  // namespace

TcpConnection::TcpConnection(EventLoop* loop, uint64_t id, Socket socket, const InetAddress& peer)
    : id_(id), peer_(peer), socket_(std::move(socket)), channel_(loop, socket_.fd()) {
  channel_.SetReadCallback([this] { HandleRead(); });
  channel_.SetWriteCallback([this] { HandleWrite(); });
  channel_.SetCloseCallback([this] { HandleClose(); });
  channel_.SetErrorCallback([this] { HandleError(); });
}

TcpConnection::~TcpConnection() { channel_.Remove(); }

int TcpConnection::Start() { return channel_.EnableReading(); }

void TcpConnection::Send(std::string_view data) {
  if (state_ == State::kClosed) {
    return;
  }

  size_t written = 0;
  // Only write directly when nothing is queued; otherwise the new bytes
  // would overtake the queued ones.
  if (!channel_.IsWriting() && output_.empty()) {
    const IoResult result = socket_.Write(data);
    if (result.error == 0) {
      written = result.bytes;
    } else if (!WouldBlock(result.error)) {
      // EPIPE, ECONNRESET, ...: the peer is gone.
      HandleClose();
      return;
    }
  }

  if (written == data.size()) {
    return;
  }
  output_.Append(data.substr(written));
  if (!channel_.IsWriting() && channel_.EnableWriting() != 0) {
    HandleClose();
  }
}

void TcpConnection::PauseReading() {
  if (state_ == State::kConnected && channel_.IsReading()) {
    channel_.DisableReading();
  }
}

void TcpConnection::ResumeReading() {
  if (state_ == State::kConnected && !channel_.IsReading() && channel_.EnableReading() != 0) {
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
  std::span<char> space = input_.PrepareWrite(kReadChunkSize);
  const IoResult result = socket_.Read(space);
  if (result.error != 0) {
    if (!WouldBlock(result.error)) {
      HandleClose();  // ECONNRESET, ...
    }
    return;
  }
  if (result.bytes == 0) {
    // The peer finished sending (EOF). Flush whatever we still owe it,
    // then close.
    BeginDrain();
    return;
  }
  input_.CommitWrite(result.bytes);
  if (message_cb_) {
    message_cb_(*this, input_);
  }
}

void TcpConnection::HandleWrite() {
  const IoResult result = socket_.Write(output_.Peek());
  if (result.error != 0) {
    if (!WouldBlock(result.error)) {
      HandleClose();
    }
    return;
  }
  output_.Retrieve(result.bytes);
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

void TcpConnection::HandleError() {
  // Consume SO_ERROR so it does not linger; the connection is unusable
  // either way.
  static_cast<void>(socket_.TakeError());
  HandleClose();
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
