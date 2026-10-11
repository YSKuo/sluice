#include "net/tcp_server.h"

#include <cstdio>

namespace sluice::net {

Result<std::unique_ptr<TcpServer>> TcpServer::Create(
    EventLoop* loop, const InetAddress& listen_addr) {
  Result<std::unique_ptr<Acceptor>> acceptor =
      Acceptor::Create(loop, listen_addr);
  if (!acceptor) {
    return Error(acceptor.error());
  }
  return std::make_unique<TcpServer>(PassKey(), loop, *std::move(acceptor));
}

TcpServer::TcpServer(PassKey /*key*/, EventLoop* loop,
                     std::unique_ptr<Acceptor> acceptor)
    : loop_(loop), acceptor_(std::move(acceptor)) {
  acceptor_->SetNewConnectionCallback(
      [this](Socket socket, const InetAddress& peer) {
        HandleNewConnection(std::move(socket), peer);
      });
}

void TcpServer::HandleNewConnection(Socket socket, const InetAddress& peer) {
  // Echo-style traffic is many small writes; Nagle's algorithm would hold
  // them back waiting for ACKs. Failure only costs latency, so it is
  // reported but not fatal for the connection.
  if (Result<void> r = socket.SetNoDelay(true); !r) {
    std::fprintf(stderr, "TCP_NODELAY for %s: %s\n", peer.ToString().c_str(),
                 r.error().message().c_str());
  }

  const uint64_t id = next_connection_id_++;
  auto conn =
      std::make_unique<TcpConnection>(loop_, id, std::move(socket), peer);
  conn->SetMessageCallback(message_cb_);
  conn->SetWriteCompleteCallback(write_complete_cb_);
  conn->SetCloseCallback(
      [this](TcpConnection& closed) { HandleConnectionClosed(closed); });

  if (Result<void> started = conn->Start(); !started) {
    // Could not register with epoll (ENOMEM/ENOSPC). Nothing has run on
    // this connection yet, so destroying it right away is safe.
    std::fprintf(stderr, "dropping connection from %s: %s\n",
                 peer.ToString().c_str(), started.error().message().c_str());
    return;
  }

  TcpConnection& ref = *conn;
  connections_.emplace(id, std::move(conn));
  if (connection_cb_) {
    connection_cb_(ref);
  }
}

void TcpServer::HandleConnectionClosed(TcpConnection& conn) {
  auto node = connections_.extract(conn.id());
  if (node.empty()) {
    return;
  }
  // We are inside one of `conn`'s own callbacks, so it must outlive this
  // call stack: hand ownership to the loop for destruction after dispatch.
  loop_->DeleteLater(std::move(node.mapped()));
}

}  // namespace sluice::net
