#include "http/http_server.h"

#include <utility>

#include "net/buffer.h"

namespace sluice::http {

Result<std::unique_ptr<HttpServer>> HttpServer::Create(
    net::EventLoop* loop, const net::InetAddress& listen_addr, Handler handler,
    ParserLimits limits) {
  Result<std::unique_ptr<net::TcpServer>> server =
      net::TcpServer::Create(loop, listen_addr);
  if (!server) {
    return Error(server.error());
  }
  return std::make_unique<HttpServer>(PassKey(), loop, *std::move(server),
                                      std::move(handler), limits);
}

HttpServer::HttpServer(PassKey /*key*/, net::EventLoop* loop,
                       std::unique_ptr<net::TcpServer> server, Handler handler,
                       ParserLimits limits)
    : loop_(loop),
      server_(std::move(server)),
      handler_(std::move(handler)),
      limits_(limits) {
  server_->SetConnectionCallback(
      [this](net::TcpConnection& conn) { OnConnection(conn); });
  server_->SetCloseCallback(
      [this](net::TcpConnection& conn) { OnClose(conn); });
  server_->SetMessageCallback(
      [this](net::TcpConnection& conn, net::Buffer& /*input*/) {
        ProcessInput(conn);
      });
  // A response finished flushing: answer the next pipelined request, if
  // one is already buffered, and resume reading.
  server_->SetWriteCompleteCallback(
      [this](net::TcpConnection& conn) { ProcessInput(conn); });
}

void HttpServer::OnConnection(net::TcpConnection& conn) {
  parsers_.emplace(conn.id(), std::make_unique<RequestParser>(limits_));
}

void HttpServer::OnClose(net::TcpConnection& conn) {
  auto node = parsers_.extract(conn.id());
  if (!node.empty()) {
    loop_->DeleteLater(std::move(node.mapped()));
  }
}

void HttpServer::ProcessInput(net::TcpConnection& conn) {
  const auto it = parsers_.find(conn.id());
  if (it == parsers_.end()) {
    return;
  }
  RequestParser& parser = *it->second;
  net::Buffer& input = conn.input();

  // Each Send() below may close the connection (write error), and each
  // Shutdown() ends it; connected() turns false in both cases.
  while (conn.connected()) {
    if (conn.PendingOutputBytes() > 0) {
      // One response in flight at a time. The write-complete callback
      // calls back into ProcessInput() once it has been flushed.
      conn.PauseReading();
      return;
    }

    const RequestParser::FeedResult result = parser.Feed(input.Peek());
    input.Retrieve(result.consumed);

    switch (result.status) {
      case RequestParser::Status::kNeedMore:
        conn.ResumeReading();
        return;
      case RequestParser::Status::kError:
        Respond(conn, MakeErrorResponse(parser.error_status()),
                /*keep_alive=*/false);
        return;
      case RequestParser::Status::kComplete: {
        const Request request = parser.TakeRequest();
        Respond(conn, handler_(request), IsKeepAlive(request));
        break;
      }
    }
  }
}

void HttpServer::Respond(net::TcpConnection& conn, const Response& response,
                         bool keep_alive) {
  conn.Send(Serialize(response, keep_alive));
  if (!keep_alive) {
    // Flush the response, then close. Anything else the client pipelined
    // is discarded.
    conn.Shutdown();
  }
}

}  // namespace sluice::http
