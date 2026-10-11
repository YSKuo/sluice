#ifndef SLUICE_ECHO_ECHO_SERVER_H_
#define SLUICE_ECHO_ECHO_SERVER_H_

#include <cstddef>
#include <cstdint>
#include <memory>

#include "net/buffer.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/tcp_connection.h"
#include "net/tcp_server.h"
#include "result.h"

namespace sluice::echo {

// TCP echo server: every byte received on a connection is written back on
// the same connection. The Week 1 milestone that exercises the event loop,
// socket RAII, and the connection lifecycle end to end, before any HTTP.
//
// Backpressure: when a connection's unsent output exceeds the high-water
// mark (the peer sends faster than it reads), reading from that connection
// is paused until the output has been fully flushed. Memory per connection
// therefore stays bounded by roughly high_water_mark + one read chunk.
class EchoServer {
 public:
  class PassKey {
    friend class EchoServer;
    PassKey() = default;
  };

  static constexpr size_t kDefaultHighWaterMark = size_t{1024} * 1024;

  // Binds to `listen_addr` but does not listen yet.
  static Result<std::unique_ptr<EchoServer>> Create(
      net::EventLoop* loop, const net::InetAddress& listen_addr,
      size_t high_water_mark = kDefaultHighWaterMark);

  EchoServer(PassKey key, std::unique_ptr<net::TcpServer> server,
             size_t high_water_mark);

  // Callbacks registered on the server capture `this`.
  EchoServer(const EchoServer&) = delete;
  EchoServer& operator=(const EchoServer&) = delete;
  EchoServer(EchoServer&&) = delete;
  EchoServer& operator=(EchoServer&&) = delete;

  ~EchoServer() = default;

  // Starts accepting connections.
  [[nodiscard]] Result<void> Start() { return server_->Start(); }

  [[nodiscard]] Result<net::InetAddress> LocalAddress() const {
    return server_->LocalAddress();
  }
  [[nodiscard]] size_t connection_count() const noexcept {
    return server_->connection_count();
  }
  // How many times any connection was paused for backpressure.
  [[nodiscard]] uint64_t pause_count() const noexcept { return pause_count_; }

 private:
  void OnMessage(net::TcpConnection& conn, net::Buffer& input);
  void OnWriteComplete(net::TcpConnection& conn);

  std::unique_ptr<net::TcpServer> server_;
  size_t high_water_mark_;
  uint64_t pause_count_ = 0;
};

}  // namespace sluice::echo

#endif  // SLUICE_ECHO_ECHO_SERVER_H_
