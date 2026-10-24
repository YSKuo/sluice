#ifndef SLUICE_PROXY_UPSTREAM_CALL_H_
#define SLUICE_PROXY_UPSTREAM_CALL_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "http/request_parser.h"
#include "http/response_parser.h"
#include "net/buffer.h"
#include "net/connector.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/socket.h"
#include "net/tcp_connection.h"
#include "net/timer.h"
#include "result.h"

namespace sluice::proxy {

// One request forwarded to one upstream, over a connection opened for it:
//
//   connecting --> sending request / reading head --> streaming body --> done
//
// It reports what happens through Callbacks and never touches the client;
// ClientSession decides what the client sees. After on_complete or
// on_failure, or after Cancel(), no callback runs again and all I/O has
// stopped, but the object must still be destroyed with
// EventLoop::DeleteLater, since those calls usually happen inside its own
// event handlers.
class UpstreamCall {
 public:
  enum class Failure : uint8_t {
    kConnectFailed,    // Refused, unreachable, ...
    kConnectTimeout,   // Handshake did not finish in time.
    kResponseTimeout,  // No bytes from the upstream for idle_timeout.
    kBadResponse,      // Malformed head, chunked, or invalid framing.
    kClosedEarly,      // Upstream closed before the response was complete.
  };

  struct Options {
    net::EventLoop::Clock::duration connect_timeout;
    // Maximum silence from the upstream once connected. It also bounds how
    // long a client that stops reading can hold the upstream: while reading
    // is paused for backpressure, no bytes arrive and this timer runs out.
    net::EventLoop::Clock::duration idle_timeout;
    http::ParserLimits limits;
  };

  struct Callbacks {
    std::function<void(const http::ResponseHead& head,
                       const http::BodyLength& length)>
        on_head;
    // A piece of the body. The view is valid only during the call.
    std::function<void(std::string_view data)> on_body;
    std::function<void()> on_complete;
    std::function<void(Failure failure)> on_failure;
  };

  class PassKey {
    friend class UpstreamCall;
    PassKey() = default;
  };

  // Starts connecting to `address` and will send `request` (already
  // serialized). `request_method` decides whether a body can follow (HEAD).
  // Fails only if the connect fails immediately.
  static Result<std::unique_ptr<UpstreamCall>> Start(
      net::EventLoop* loop, const net::InetAddress& address,
      std::string request, std::string request_method, Options options,
      Callbacks callbacks);

  UpstreamCall(PassKey key, net::EventLoop* loop,
               const net::InetAddress& address, std::string request,
               std::string request_method, Options options,
               Callbacks callbacks);

  // Callbacks registered on the connector, connection, and timer capture
  // `this`.
  UpstreamCall(const UpstreamCall&) = delete;
  UpstreamCall& operator=(const UpstreamCall&) = delete;
  UpstreamCall(UpstreamCall&&) = delete;
  UpstreamCall& operator=(UpstreamCall&&) = delete;

  ~UpstreamCall() = default;

  // Backpressure from the client side: stop/restart reading the upstream.
  void PauseReading();
  void ResumeReading();

  // Abandons the call (e.g. the client went away). No callback runs.
  void Cancel() { Stop(); }

 private:
  void OnConnected(net::Socket socket);
  void OnUpstreamData(net::Buffer& input);
  void OnUpstreamClosed();
  // Parses the head from `input`; returns false if the call ended.
  bool ReadHead(net::Buffer& input);
  void ForwardBody(net::Buffer& input, http::BodyLength body);
  void ArmIdleTimer();
  void Finish();
  void Fail(Failure failure);
  // Ends all I/O and timers; afterwards no callback runs.
  void Stop();

  net::EventLoop* loop_;
  net::InetAddress address_;
  std::string request_;
  std::string request_method_;
  Options options_;
  Callbacks callbacks_;

  std::unique_ptr<net::Connector> connector_;
  std::unique_ptr<net::TcpConnection> conn_;
  http::ResponseParser parser_;
  // Set once the head is parsed.
  std::optional<http::BodyLength> body_;
  uint64_t body_remaining_ = 0;
  bool done_ = false;
  // Declared last so it is destroyed (and cancelled) first.
  net::Timer timer_;
};

}  // namespace sluice::proxy

#endif  // SLUICE_PROXY_UPSTREAM_CALL_H_
