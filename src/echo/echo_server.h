#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "net/inet_address.h"
#include "net/tcp_server.h"

namespace sluice::net {
class EventLoop;
}  // namespace sluice::net

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
  static constexpr size_t kDefaultHighWaterMark = size_t{1024} * 1024;

  class PassKey {
    friend class EchoServer;
    PassKey() = default;
  };

  // Binds to `listen_addr` but does not listen yet. Returns nullptr and
  // sets `*error` to errno on failure.
  static std::unique_ptr<EchoServer> Create(net::EventLoop* loop,
                                            const net::InetAddress& listen_addr, int* error,
                                            size_t high_water_mark = kDefaultHighWaterMark);
  EchoServer(PassKey key, std::unique_ptr<net::TcpServer> server, size_t high_water_mark);
  ~EchoServer() = default;

  // Callbacks registered on the server capture `this`.
  EchoServer(const EchoServer&) = delete;
  EchoServer& operator=(const EchoServer&) = delete;
  EchoServer(EchoServer&&) = delete;
  EchoServer& operator=(EchoServer&&) = delete;

  // Starts accepting connections. Returns 0 or errno.
  [[nodiscard]] int Start() { return server_->Start(); }

  [[nodiscard]] net::InetAddress LocalAddress(int* error) const {
    return server_->LocalAddress(error);
  }
  [[nodiscard]] size_t connection_count() const noexcept { return server_->connection_count(); }
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
