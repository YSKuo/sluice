#ifndef SLUICE_NET_TCP_SERVER_H_
#define SLUICE_NET_TCP_SERVER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>
#include <utility>

#include "net/acceptor.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/socket.h"
#include "net/tcp_connection.h"
#include "result.h"

namespace sluice::net {

// Accepts connections through an Acceptor and owns every live
// TcpConnection. Protocol logic is supplied through callbacks, which are
// installed on each new connection.
//
// Closed connections are removed from the table immediately and destroyed
// after the current loop iteration (EventLoop::DeleteLater).
class TcpServer {
 public:
  // Runs once per accepted connection, after the message/write-complete
  // callbacks are installed and before any data is read.
  using ConnectionCallback = std::function<void(TcpConnection& conn)>;

  class PassKey {
    friend class TcpServer;
    PassKey() = default;
  };

  // Binds to `listen_addr` but does not listen yet.
  static Result<std::unique_ptr<TcpServer>> Create(
      EventLoop* loop, const InetAddress& listen_addr);

  TcpServer(PassKey key, EventLoop* loop, std::unique_ptr<Acceptor> acceptor);

  TcpServer(const TcpServer&) = delete;
  TcpServer& operator=(const TcpServer&) = delete;
  TcpServer(TcpServer&&) = delete;
  TcpServer& operator=(TcpServer&&) = delete;

  ~TcpServer() = default;

  void SetConnectionCallback(ConnectionCallback cb) {
    connection_cb_ = std::move(cb);
  }
  void SetMessageCallback(TcpConnection::MessageCallback cb) {
    message_cb_ = std::move(cb);
  }
  void SetWriteCompleteCallback(TcpConnection::WriteCompleteCallback cb) {
    write_complete_cb_ = std::move(cb);
  }
  // Runs once when a connection closes, before it is handed to
  // EventLoop::DeleteLater, so per-connection protocol state can be
  // released. The connection is still valid during the call.
  void SetCloseCallback(ConnectionCallback cb) { close_cb_ = std::move(cb); }

  // Starts accepting connections.
  [[nodiscard]] Result<void> Start() { return acceptor_->Listen(); }

  [[nodiscard]] Result<InetAddress> LocalAddress() const {
    return acceptor_->LocalAddress();
  }
  [[nodiscard]] size_t connection_count() const noexcept {
    return connections_.size();
  }

 private:
  void HandleNewConnection(Socket socket, const InetAddress& peer);
  void HandleConnectionClosed(TcpConnection& conn);

  EventLoop* loop_;
  std::unique_ptr<Acceptor> acceptor_;
  uint64_t next_connection_id_ = 1;
  std::unordered_map<uint64_t, std::unique_ptr<TcpConnection>> connections_;

  ConnectionCallback connection_cb_;
  ConnectionCallback close_cb_;
  TcpConnection::MessageCallback message_cb_;
  TcpConnection::WriteCompleteCallback write_complete_cb_;
};

}  // namespace sluice::net

#endif  // SLUICE_NET_TCP_SERVER_H_
