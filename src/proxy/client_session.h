#ifndef SLUICE_PROXY_CLIENT_SESSION_H_
#define SLUICE_PROXY_CLIENT_SESSION_H_

#include <cstdint>
#include <memory>
#include <string_view>

#include "http/request.h"
#include "http/request_parser.h"
#include "http/response_parser.h"
#include "http/status.h"
#include "net/event_loop.h"
#include "net/tcp_connection.h"
#include "net/timer.h"
#include "proxy/proxy_options.h"
#include "proxy/route_table.h"
#include "proxy/upstream.h"
#include "proxy/upstream_call.h"

namespace sluice::proxy {

// Everything the gateway does for one client connection:
//
//   reading request --(complete)--> proxying --(response done)--> reading
//         |                             |
//         +-- invalid / timeout ---- local error response (4xx/5xx)
//
// One request is proxied at a time: while it is in flight, client reading
// is paused and later pipelined requests wait in the input buffer.
//
// Backpressure, response direction: upstream bytes are forwarded to the
// client as they arrive; when more than response_high_water_mark bytes are
// queued for the client, the upstream is paused until the client drains
// them. The request direction is bounded by the parser's body limit.
//
// Owned by ProxyServer; the client TcpConnection is owned by TcpServer and
// outlives the session.
class ClientSession {
 public:
  ClientSession(net::EventLoop* loop, net::TcpConnection* client,
                RouteTable* routes, const ProxyOptions* options,
                ProxyStats* stats);

  // Callbacks on the upstream call and the timer capture `this`.
  ClientSession(const ClientSession&) = delete;
  ClientSession& operator=(const ClientSession&) = delete;
  ClientSession(ClientSession&&) = delete;
  ClientSession& operator=(ClientSession&&) = delete;

  ~ClientSession() = default;

  // Events forwarded by ProxyServer.
  void Start();
  void OnInput();
  // The client's output buffer has been fully flushed.
  void OnClientDrained();
  // The client connection closed: abandon any upstream call. The session is
  // destroyed (DeleteLater) right after.
  void OnClientClosed();

 private:
  enum class State : uint8_t {
    kReadingRequest,
    kProxying,
    kClosed,
  };
  enum class ReadTimer : uint8_t {
    kNone,
    kIdle,     // Waiting for the first byte of the next request.
    kRequest,  // A request has started; it must complete by the deadline.
  };

  // Parses and handles buffered requests until one is being proxied, more
  // bytes are needed, or a response is still flushing.
  void ProcessRequests();
  void HandleRequest(const http::Request& request);
  void SendLocalResponse(http::StatusCode status, bool keep_alive);
  void UpdateReadTimer();

  void OnUpstreamHead(const http::ResponseHead& head,
                      const http::BodyLength& length);
  void OnUpstreamBody(std::string_view data);
  void OnUpstreamComplete();
  void OnUpstreamFailure(UpstreamCall::Failure failure);
  void ApplyBackpressure();
  // Back to reading requests; hands the finished call to DeleteLater.
  void EndExchange();

  net::EventLoop* loop_;
  net::TcpConnection* client_;
  RouteTable* routes_;
  const ProxyOptions* options_;
  ProxyStats* stats_;

  http::RequestParser parser_;
  State state_ = State::kReadingRequest;

  // The exchange in flight (state kProxying).
  std::unique_ptr<UpstreamCall> call_;
  Upstream* upstream_ = nullptr;
  bool keep_alive_ = false;
  bool head_sent_ = false;
  bool upstream_paused_ = false;

  ReadTimer read_timer_kind_ = ReadTimer::kNone;
  // Declared last so it is destroyed (and cancelled) first.
  net::Timer read_timer_;
};

}  // namespace sluice::proxy

#endif  // SLUICE_PROXY_CLIENT_SESSION_H_
