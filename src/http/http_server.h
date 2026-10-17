#ifndef SLUICE_HTTP_HTTP_SERVER_H_
#define SLUICE_HTTP_HTTP_SERVER_H_

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <unordered_map>

#include "http/request.h"
#include "http/request_parser.h"
#include "http/response.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/tcp_connection.h"
#include "net/tcp_server.h"
#include "result.h"

namespace sluice::http {

// HTTP/1.1 server on top of net::TcpServer: parses requests with one
// RequestParser per connection, passes each complete request to the
// Handler, and writes the serialized response back.
//
// Keep-alive: a connection serves requests until the client asks to close
// (see IsKeepAlive) or a request is invalid; error responses always close.
//
// Pipelining and backpressure: requests on one connection are answered
// strictly in order, one at a time. While a response is still being
// flushed, further requests stay in the input buffer and reading pauses,
// so a client that sends requests without reading responses cannot make
// the server queue an unbounded amount of output.
class HttpServer {
 public:
  // Produces the response for a request. Called synchronously, on the loop
  // thread. (Week 3's proxy will need an asynchronous variant.)
  using Handler = std::function<Response(const Request& request)>;

  class PassKey {
    friend class HttpServer;
    PassKey() = default;
  };

  // Binds to `listen_addr` but does not listen yet.
  static Result<std::unique_ptr<HttpServer>> Create(
      net::EventLoop* loop, const net::InetAddress& listen_addr,
      Handler handler, ParserLimits limits = {});

  HttpServer(PassKey key, net::EventLoop* loop,
             std::unique_ptr<net::TcpServer> server, Handler handler,
             ParserLimits limits);

  // Callbacks registered on the server capture `this`.
  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;
  HttpServer(HttpServer&&) = delete;
  HttpServer& operator=(HttpServer&&) = delete;

  ~HttpServer() = default;

  // Starts accepting connections.
  [[nodiscard]] Result<void> Start() { return server_->Start(); }

  [[nodiscard]] Result<net::InetAddress> LocalAddress() const {
    return server_->LocalAddress();
  }
  [[nodiscard]] size_t connection_count() const noexcept {
    return server_->connection_count();
  }

 private:
  void OnConnection(net::TcpConnection& conn);
  void OnClose(net::TcpConnection& conn);
  // Parses and answers as many buffered requests as the one-response-in-
  // flight rule allows. Runs on new input and when a response finishes
  // flushing.
  void ProcessInput(net::TcpConnection& conn);
  void Respond(net::TcpConnection& conn, const Response& response,
               bool keep_alive);

  net::EventLoop* loop_;
  // Destroying the server destroys its connections without running any of
  // their callbacks, so member order does not matter for parsers_.
  std::unique_ptr<net::TcpServer> server_;
  Handler handler_;
  ParserLimits limits_;
  // One parser per live connection, keyed by connection id. Held through
  // unique_ptr so a closed connection's parser can be handed to
  // EventLoop::DeleteLater: the close usually happens while ProcessInput()
  // still holds a reference to it.
  std::unordered_map<uint64_t, std::unique_ptr<RequestParser>> parsers_;
};

}  // namespace sluice::http

#endif  // SLUICE_HTTP_HTTP_SERVER_H_
