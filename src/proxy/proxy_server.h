#ifndef SLUICE_PROXY_PROXY_SERVER_H_
#define SLUICE_PROXY_PROXY_SERVER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/tcp_connection.h"
#include "net/tcp_server.h"
#include "net/timer.h"
#include "proxy/client_session.h"
#include "proxy/proxy_options.h"
#include "proxy/rate_limiter.h"
#include "proxy/route_table.h"
#include "result.h"

namespace sluice::proxy {

// The gateway: accepts client connections and runs one ClientSession per
// connection, which routes each request, forwards it to an upstream, and
// streams the response back.
class ProxyServer {
 public:
  class PassKey {
    friend class ProxyServer;
    PassKey() = default;
  };

  // Binds to `listen_addr` but does not listen yet.
  static Result<std::unique_ptr<ProxyServer>> Create(
      net::EventLoop* loop, const net::InetAddress& listen_addr,
      RouteTable routes, ProxyOptions options);

  ProxyServer(PassKey key, net::EventLoop* loop,
              std::unique_ptr<net::TcpServer> server, RouteTable routes,
              ProxyOptions options);

  // Callbacks registered on the server capture `this`, and sessions hold
  // pointers to routes_, options_, and stats_.
  ProxyServer(const ProxyServer&) = delete;
  ProxyServer& operator=(const ProxyServer&) = delete;
  ProxyServer(ProxyServer&&) = delete;
  ProxyServer& operator=(ProxyServer&&) = delete;

  ~ProxyServer() = default;

  // Starts accepting connections.
  [[nodiscard]] Result<void> Start() { return server_->Start(); }

  [[nodiscard]] Result<net::InetAddress> LocalAddress() const {
    return server_->LocalAddress();
  }
  [[nodiscard]] size_t connection_count() const noexcept {
    return server_->connection_count();
  }
  [[nodiscard]] const ProxyStats& stats() const noexcept { return stats_; }
  [[nodiscard]] RouteTable& routes() noexcept { return routes_; }
  // Null when rate limiting is disabled.
  [[nodiscard]] const RateLimiter* rate_limiter() const noexcept {
    return rate_limiter_.get();
  }

 private:
  void OnConnection(net::TcpConnection& conn);
  void OnClose(net::TcpConnection& conn);
  ClientSession* FindSession(const net::TcpConnection& conn);
  // Drops idle rate-limit buckets, then re-arms itself.
  void SweepRateLimiter();

  net::EventLoop* loop_;
  RouteTable routes_;
  ProxyOptions options_;
  ProxyStats stats_;
  std::unique_ptr<RateLimiter> rate_limiter_;
  net::Timer sweep_timer_;
  std::unique_ptr<net::TcpServer> server_;
  // Declared last so sessions (which point at the members above) are
  // destroyed first.
  std::unordered_map<uint64_t, std::unique_ptr<ClientSession>> sessions_;
};

}  // namespace sluice::proxy

#endif  // SLUICE_PROXY_PROXY_SERVER_H_
