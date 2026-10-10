#pragma once

#include <functional>
#include <memory>
#include <utility>

#include "net/channel.h"
#include "net/inet_address.h"
#include "net/socket.h"
#include "net/unique_fd.h"

namespace sluice::net {

class EventLoop;

// Owns the listening socket. When the loop reports it readable, accepts
// pending connections and hands each new connected Socket to the
// NewConnectionCallback, which takes ownership of it.
class Acceptor {
 public:
  using NewConnectionCallback = std::function<void(Socket socket, const InetAddress& peer)>;

  // Listen backlog passed to listen(2); the kernel caps it at somaxconn.
  static constexpr int kBacklog = 1024;
  // Upper bound on accept() calls per readiness event, so that a burst of
  // new connections cannot starve the existing ones. Anything left over is
  // picked up on the next iteration (epoll is level-triggered).
  static constexpr int kMaxAcceptsPerEvent = 64;

  class PassKey {
    friend class Acceptor;
    PassKey() = default;
  };

  // Creates a socket bound to `listen_addr` (SO_REUSEADDR set) but does not
  // listen yet. Returns nullptr and sets `*error` to errno on failure.
  static std::unique_ptr<Acceptor> Create(EventLoop* loop, const InetAddress& listen_addr,
                                          int* error);
  Acceptor(PassKey key, EventLoop* loop, Socket socket, UniqueFd idle_fd);
  ~Acceptor();

  Acceptor(const Acceptor&) = delete;
  Acceptor& operator=(const Acceptor&) = delete;
  Acceptor(Acceptor&&) = delete;
  Acceptor& operator=(Acceptor&&) = delete;

  void SetNewConnectionCallback(NewConnectionCallback cb) { new_connection_cb_ = std::move(cb); }

  // Starts listening and registers for read events. Returns 0 or errno.
  [[nodiscard]] int Listen();

  // Bound address; differs from the requested one when port 0 was used.
  [[nodiscard]] InetAddress LocalAddress(int* error) const { return socket_.LocalAddress(error); }

 private:
  void HandleRead();
  void HandleDescriptorExhaustion();

  Socket socket_;
  // A spare descriptor (opened on /dev/null) reserved for EMFILE/ENFILE
  // handling; see HandleDescriptorExhaustion().
  UniqueFd idle_fd_;
  // Declared after socket_ so it is destroyed first; the destructor also
  // removes it explicitly before the socket is closed.
  Channel channel_;
  NewConnectionCallback new_connection_cb_;
};

}  // namespace sluice::net
