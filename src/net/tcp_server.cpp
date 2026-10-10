#include "net/tcp_server.h"

#include <cstdio>
#include <cstring>

#include "net/event_loop.h"

namespace sluice::net {

std::unique_ptr<TcpServer> TcpServer::Create(EventLoop* loop, const InetAddress& listen_addr,
                                             int* error) {
  std::unique_ptr<Acceptor> acceptor = Acceptor::Create(loop, listen_addr, error);
  if (!acceptor) {
    return nullptr;
  }
  return std::make_unique<TcpServer>(PassKey(), loop, std::move(acceptor));
}

TcpServer::TcpServer(PassKey /*key*/, EventLoop* loop, std::unique_ptr<Acceptor> acceptor)
    : loop_(loop), acceptor_(std::move(acceptor)) {
  acceptor_->SetNewConnectionCallback([this](Socket socket, const InetAddress& peer) {
    HandleNewConnection(std::move(socket), peer);
  });
}

void TcpServer::HandleNewConnection(Socket socket, const InetAddress& peer) {
  // Echo-style traffic is many small writes; Nagle's algorithm would hold
  // them back waiting for ACKs. Failure only costs latency, so it is not
  // treated as fatal for the connection.
  static_cast<void>(socket.SetNoDelay(true));

  const uint64_t id = next_connection_id_++;
  auto conn = std::make_unique<TcpConnection>(loop_, id, std::move(socket), peer);
  conn->SetMessageCallback(message_cb_);
  conn->SetWriteCompleteCallback(write_complete_cb_);
  conn->SetCloseCallback([this](TcpConnection& c) { HandleConnectionClosed(c); });

  if (const int err = conn->Start(); err != 0) {
    // Could not register with epoll (ENOMEM/ENOSPC). Nothing has run on
    // this connection yet, so destroying it right away is safe.
    std::fprintf(stderr, "dropping connection from %s: %s\n", peer.ToString().c_str(),
                 std::strerror(err));
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
