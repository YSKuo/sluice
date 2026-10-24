#include "net/connector.h"

namespace sluice::net {

Result<std::unique_ptr<Connector>> Connector::Start(
    EventLoop* loop, const InetAddress& addr, ConnectedCallback on_connected,
    FailedCallback on_failed) {
  Result<Socket> socket = Socket::CreateNonBlockingTcp();
  if (!socket) {
    return Error(socket.error());
  }
  // Loopback connects may succeed at once; EINPROGRESS is the usual case.
  // Both are completed through the writable event below.
  if (Result<void> started = socket->Connect(addr);
      !started && started.error() != std::errc::operation_in_progress) {
    return Error(started.error());
  }
  auto connector = std::make_unique<Connector>(
      PassKey(), loop, *std::move(socket), std::move(on_connected),
      std::move(on_failed));
  if (Result<void> watching = connector->channel_.EnableWriting(); !watching) {
    return Error(watching.error());
  }
  return connector;
}

Connector::Connector(PassKey /*key*/, EventLoop* loop, Socket socket,
                     ConnectedCallback on_connected, FailedCallback on_failed)
    : socket_(std::move(socket)),
      channel_(loop, socket_.fd()),
      on_connected_(std::move(on_connected)),
      on_failed_(std::move(on_failed)) {
  channel_.SetWriteCallback([this] { HandleConnectDone(); });
  channel_.SetErrorCallback([this] { HandleConnectDone(); });
  channel_.SetCloseCallback([this] { HandleConnectDone(); });
}

void Connector::HandleConnectDone() {
  // Unregister before handing the socket on: its new owner registers the
  // same descriptor with its own Channel.
  channel_.Remove();
  if (const std::error_code error = socket_.TakeError()) {
    on_failed_(error);
    return;
  }
  on_connected_(std::move(socket_));
}

}  // namespace sluice::net
