#ifndef SLUICE_NET_TCP_CONNECTION_H_
#define SLUICE_NET_TCP_CONNECTION_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string_view>
#include <utility>

#include "net/buffer.h"
#include "net/channel.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/socket.h"
#include "result.h"

namespace sluice::net {

// One accepted TCP connection: owns the connected socket, its Channel, and
// an input and an output Buffer.
//
// Reads arrive in the input buffer and are handed to the MessageCallback.
// Send() writes directly when possible and queues whatever the kernel did
// not accept in the output buffer, which is flushed as the socket becomes
// writable again.
//
// Lifecycle: kConnected -> (kDraining) -> kClosed. Once closed, the
// CloseCallback runs exactly once; its owner must then destroy the
// connection with EventLoop::DeleteLater, since the close usually happens
// inside one of this connection's own callbacks.
class TcpConnection {
 public:
  using MessageCallback =
      std::function<void(TcpConnection& conn, Buffer& input)>;
  using WriteCompleteCallback = std::function<void(TcpConnection& conn)>;
  using CloseCallback = std::function<void(TcpConnection& conn)>;

  // Bytes requested from the kernel per read() call.
  static constexpr size_t kReadChunkSize = size_t{16} * 1024;

  TcpConnection(EventLoop* loop, uint64_t id, Socket socket,
                const InetAddress& peer);

  TcpConnection(const TcpConnection&) = delete;
  TcpConnection& operator=(const TcpConnection&) = delete;
  TcpConnection(TcpConnection&&) = delete;
  TcpConnection& operator=(TcpConnection&&) = delete;

  ~TcpConnection();

  void SetMessageCallback(MessageCallback cb) { message_cb_ = std::move(cb); }
  // Runs when output that had to be queued has been fully flushed. It does
  // not run when Send() wrote everything immediately.
  void SetWriteCompleteCallback(WriteCompleteCallback cb) {
    write_complete_cb_ = std::move(cb);
  }
  void SetCloseCallback(CloseCallback cb) { close_cb_ = std::move(cb); }

  // Registers for read events. On failure the caller should simply destroy
  // the connection (no callbacks have run).
  [[nodiscard]] Result<void> Start();

  // Queues `data` for sending. Ignored once the connection is closed. A
  // write error closes the connection (and runs the CloseCallback) before
  // Send() returns.
  void Send(std::string_view data);

  // Stops reading from the peer; pending output keeps flushing. This is the
  // backpressure lever: a peer that does not read our output must not be
  // able to make us buffer unbounded input on its behalf.
  void PauseReading();
  void ResumeReading();

  // Graceful close: stop reading, flush pending output, then close.
  void Shutdown();
  // Immediate close; pending output is discarded.
  void ForceClose();

  [[nodiscard]] uint64_t id() const noexcept { return id_; }
  [[nodiscard]] const InetAddress& peer() const noexcept { return peer_; }
  [[nodiscard]] bool connected() const noexcept {
    return state_ == State::kConnected;
  }
  [[nodiscard]] bool reading() const noexcept { return channel_.IsReading(); }
  [[nodiscard]] size_t PendingOutputBytes() const noexcept {
    return output_.ReadableBytes();
  }

 private:
  enum class State : uint8_t {
    kConnected,
    kDraining,  // No more reads; closes once the output buffer is empty.
    kClosed,
  };

  void HandleRead();
  void HandleWrite();
  void HandleClose();
  // Enters kDraining, or closes right away if nothing is left to flush.
  void BeginDrain();

  uint64_t id_;
  InetAddress peer_;
  Socket socket_;
  // Declared after socket_ so it is destroyed first; the destructor also
  // removes it explicitly before the socket is closed.
  Channel channel_;
  Buffer input_;
  Buffer output_;
  State state_ = State::kConnected;

  MessageCallback message_cb_;
  WriteCompleteCallback write_complete_cb_;
  CloseCallback close_cb_;
};

}  // namespace sluice::net

#endif  // SLUICE_NET_TCP_CONNECTION_H_
