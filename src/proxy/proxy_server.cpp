#include "proxy/proxy_server.h"

#include <utility>

#include "net/buffer.h"

namespace sluice::proxy {

Result<std::unique_ptr<ProxyServer>> ProxyServer::Create(
    net::EventLoop* loop, const net::InetAddress& listen_addr,
    RouteTable routes, ProxyOptions options) {
  Result<std::unique_ptr<net::TcpServer>> server =
      net::TcpServer::Create(loop, listen_addr);
  if (!server) {
    return Error(server.error());
  }
  return std::make_unique<ProxyServer>(PassKey(), loop, *std::move(server),
                                       std::move(routes), options);
}

ProxyServer::ProxyServer(PassKey /*key*/, net::EventLoop* loop,
                         std::unique_ptr<net::TcpServer> server,
                         RouteTable routes, ProxyOptions options)
    : loop_(loop),
      routes_(std::move(routes)),
      options_(options),
      sweep_timer_(loop),
      server_(std::move(server)) {
  if (options_.rate_limit) {
    rate_limiter_ = std::make_unique<RateLimiter>(
        *options_.rate_limit, options_.rate_limit_max_clients);
    SweepRateLimiter();
  }
  server_->SetConnectionCallback(
      [this](net::TcpConnection& conn) { OnConnection(conn); });
  server_->SetCloseCallback(
      [this](net::TcpConnection& conn) { OnClose(conn); });
  server_->SetMessageCallback(
      [this](net::TcpConnection& conn, net::Buffer& /*input*/) {
        if (ClientSession* session = FindSession(conn)) {
          session->OnInput();
        }
      });
  server_->SetWriteCompleteCallback([this](net::TcpConnection& conn) {
    if (ClientSession* session = FindSession(conn)) {
      session->OnClientDrained();
    }
  });
}

void ProxyServer::OnConnection(net::TcpConnection& conn) {
  auto session = std::make_unique<ClientSession>(
      loop_, &conn, &routes_, rate_limiter_.get(), &options_, &stats_);
  ClientSession& ref = *session;
  sessions_.emplace(conn.id(), std::move(session));
  ref.Start();
}

void ProxyServer::OnClose(net::TcpConnection& conn) {
  auto node = sessions_.extract(conn.id());
  if (node.empty()) {
    return;
  }
  node.mapped()->OnClientClosed();
  // The close usually happens inside the session's own call stack.
  loop_->DeleteLater(std::move(node.mapped()));
}

void ProxyServer::SweepRateLimiter() {
  rate_limiter_->EvictIdle(Clock::now());
  sweep_timer_.Arm(options_.rate_limit_sweep_interval,
                   [this] { SweepRateLimiter(); });
}

ClientSession* ProxyServer::FindSession(const net::TcpConnection& conn) {
  const auto it = sessions_.find(conn.id());
  return it == sessions_.end() ? nullptr : it->second.get();
}

}  // namespace sluice::proxy
