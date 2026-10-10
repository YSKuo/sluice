#include "echo/echo_server.h"

#include <utility>

namespace sluice::echo {

std::unique_ptr<EchoServer> EchoServer::Create(net::EventLoop* loop,
                                               const net::InetAddress& listen_addr, int* error,
                                               size_t high_water_mark) {
  std::unique_ptr<net::TcpServer> server = net::TcpServer::Create(loop, listen_addr, error);
  if (!server) {
    return nullptr;
  }
  return std::make_unique<EchoServer>(PassKey(), std::move(server), high_water_mark);
}

EchoServer::EchoServer(PassKey /*key*/, std::unique_ptr<net::TcpServer> server,
                       size_t high_water_mark)
    : server_(std::move(server)), high_water_mark_(high_water_mark) {
  server_->SetMessageCallback(
      [this](net::TcpConnection& conn, net::Buffer& input) { OnMessage(conn, input); });
  server_->SetWriteCompleteCallback([this](net::TcpConnection& conn) { OnWriteComplete(conn); });
}

void EchoServer::OnMessage(net::TcpConnection& conn, net::Buffer& input) {
  conn.Send(input.Peek());
  input.RetrieveAll();
  if (conn.connected() && conn.reading() && conn.PendingOutputBytes() > high_water_mark_) {
    conn.PauseReading();
    ++pause_count_;
  }
}

void EchoServer::OnWriteComplete(net::TcpConnection& conn) {
  // The output buffer is empty again, so it is safe to accept more input.
  conn.ResumeReading();
}

}  // namespace sluice::echo
