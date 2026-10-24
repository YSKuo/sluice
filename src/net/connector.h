#ifndef SLUICE_NET_CONNECTOR_H_
#define SLUICE_NET_CONNECTOR_H_

#include <functional>
#include <memory>
#include <system_error>
#include <utility>

#include "net/channel.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/socket.h"
#include "result.h"

namespace sluice::net {

// One outgoing, non-blocking TCP connect.
//
// connect() on a non-blocking socket returns EINPROGRESS; the kernel then
// finishes the handshake in the background and reports the outcome by
// making the socket writable. At that point SO_ERROR says whether it
// worked (0) or why not (ECONNREFUSED, ETIMEDOUT, ...). Connector waits
// for that event, then hands the connected Socket to its owner, which
// usually wraps it in a TcpConnection.
//
// Timeouts are the owner's job (a net::Timer that calls Cancel()); the
// kernel's own SYN retry timeout is far too long for a gateway.
class Connector {
 public:
  using ConnectedCallback = std::function<void(Socket socket)>;
  using FailedCallback = std::function<void(std::error_code error)>;

  class PassKey {
    friend class Connector;
    PassKey() = default;
  };

  // Creates the socket and starts connecting. Fails only if the connect
  // fails immediately (e.g. ENETUNREACH); otherwise exactly one of the
  // callbacks runs later, unless Cancel() is called first.
  static Result<std::unique_ptr<Connector>> Start(
      EventLoop* loop, const InetAddress& addr, ConnectedCallback on_connected,
      FailedCallback on_failed);

  Connector(PassKey key, EventLoop* loop, Socket socket,
            ConnectedCallback on_connected, FailedCallback on_failed);

  // The Channel's callbacks capture `this`.
  Connector(const Connector&) = delete;
  Connector& operator=(const Connector&) = delete;
  Connector(Connector&&) = delete;
  Connector& operator=(Connector&&) = delete;

  ~Connector() { channel_.Remove(); }

  // Stops waiting; neither callback will run. Safe to call from inside
  // another callback on the loop.
  void Cancel() { channel_.Remove(); }

 private:
  // The socket became writable, or epoll reported an error or hang-up:
  // either way the handshake is over.
  void HandleConnectDone();

  Socket socket_;
  Channel channel_;
  ConnectedCallback on_connected_;
  FailedCallback on_failed_;
};

}  // namespace sluice::net

#endif  // SLUICE_NET_CONNECTOR_H_
