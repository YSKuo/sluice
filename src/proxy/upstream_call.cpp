#include "proxy/upstream_call.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace sluice::proxy {

Result<std::unique_ptr<UpstreamCall>> UpstreamCall::Start(
    net::EventLoop* loop, const net::InetAddress& address, std::string request,
    std::string request_method, Options options, Callbacks callbacks) {
  auto call = std::make_unique<UpstreamCall>(
      PassKey(), loop, address, std::move(request), std::move(request_method),
      options, std::move(callbacks));
  UpstreamCall* self = call.get();
  Result<std::unique_ptr<net::Connector>> connector = net::Connector::Start(
      loop, address,
      [self](net::Socket socket) { self->OnConnected(std::move(socket)); },
      [self](std::error_code /*error*/) {
        self->Fail(Failure::kConnectFailed);
      });
  if (!connector) {
    return Error(connector.error());
  }
  call->connector_ = *std::move(connector);
  call->timer_.Arm(options.connect_timeout,
                   [self] { self->Fail(Failure::kConnectTimeout); });
  return call;
}

UpstreamCall::UpstreamCall(PassKey /*key*/, net::EventLoop* loop,
                           const net::InetAddress& address, std::string request,
                           std::string request_method, Options options,
                           Callbacks callbacks)
    : loop_(loop),
      address_(address),
      request_(std::move(request)),
      request_method_(std::move(request_method)),
      options_(options),
      callbacks_(std::move(callbacks)),
      parser_(options.limits),
      timer_(loop) {}

void UpstreamCall::PauseReading() {
  if (conn_ != nullptr) {
    conn_->PauseReading();
  }
}

void UpstreamCall::ResumeReading() {
  if (conn_ != nullptr && !done_) {
    conn_->ResumeReading();
  }
}

void UpstreamCall::OnConnected(net::Socket socket) {
  if (done_) {
    return;
  }
  if (Result<void> r = socket.SetNoDelay(true); !r) {
    std::fprintf(stderr, "TCP_NODELAY for upstream %s: %s\n",
                 address_.ToString().c_str(), r.error().message().c_str());
  }
  // The id only matters to TcpServer, which does not own this connection.
  conn_ = std::make_unique<net::TcpConnection>(loop_, /*id=*/0,
                                               std::move(socket), address_);
  conn_->SetMessageCallback(
      [this](net::TcpConnection& /*conn*/, net::Buffer& input) {
        OnUpstreamData(input);
      });
  conn_->SetCloseCallback(
      [this](net::TcpConnection& /*conn*/) { OnUpstreamClosed(); });
  if (Result<void> started = conn_->Start(); !started) {
    Fail(Failure::kConnectFailed);
    return;
  }
  ArmIdleTimer();
  conn_->Send(request_);
  // Free the copy; a write error has already closed the connection and
  // reported kClosedEarly through OnUpstreamClosed().
  request_ = std::string();
}

void UpstreamCall::OnUpstreamData(net::Buffer& input) {
  if (done_) {
    return;
  }
  ArmIdleTimer();
  if (!body_ && !ReadHead(input)) {
    return;
  }
  if (body_) {
    ForwardBody(input, *body_);
  }
}

bool UpstreamCall::ReadHead(net::Buffer& input) {
  const http::ResponseParser::FeedResult result = parser_.Feed(input.Peek());
  input.Retrieve(result.consumed);
  if (result.status == http::ResponseParser::Status::kNeedMore) {
    return false;
  }
  if (result.status == http::ResponseParser::Status::kError) {
    Fail(Failure::kBadResponse);
    return false;
  }
  const http::ResponseHead head = parser_.TakeHead();
  // 1xx interim responses are not supported (sluice strips Expect, so a
  // well-behaved upstream does not send them).
  const std::optional<http::BodyLength> length =
      head.status >= 200 ? http::GetBodyLength(head, request_method_)
                         : std::nullopt;
  if (!length) {
    Fail(Failure::kBadResponse);
    return false;
  }
  body_ = *length;
  body_remaining_ = length->length;
  callbacks_.on_head(head, *length);
  return !done_;
}

void UpstreamCall::ForwardBody(net::Buffer& input, http::BodyLength body) {
  const std::string_view data = input.Peek();
  if (body.kind == http::BodyLength::Kind::kUntilClose) {
    if (!data.empty()) {
      callbacks_.on_body(data);
    }
    if (!done_) {
      input.RetrieveAll();
    }
    return;  // The body ends in OnUpstreamClosed().
  }
  // Bytes beyond Content-Length are not part of this response; dropping
  // them is safe because the connection is never reused.
  const auto n =
      static_cast<size_t>(std::min<uint64_t>(body_remaining_, data.size()));
  body_remaining_ -= n;
  if (n > 0) {
    callbacks_.on_body(data.substr(0, n));
  }
  if (done_) {
    return;
  }
  input.RetrieveAll();
  if (body_remaining_ == 0) {
    Finish();
  }
}

void UpstreamCall::OnUpstreamClosed() {
  if (done_) {
    return;
  }
  if (body_ && (body_->kind == http::BodyLength::Kind::kUntilClose ||
                body_remaining_ == 0)) {
    Finish();
    return;
  }
  Fail(Failure::kClosedEarly);
}

void UpstreamCall::ArmIdleTimer() {
  timer_.Arm(options_.idle_timeout,
             [this] { Fail(Failure::kResponseTimeout); });
}

void UpstreamCall::Finish() {
  Stop();
  callbacks_.on_complete();
}

void UpstreamCall::Fail(Failure failure) {
  if (done_) {
    return;
  }
  Stop();
  callbacks_.on_failure(failure);
}

void UpstreamCall::Stop() {
  done_ = true;
  timer_.Disarm();
  if (connector_ != nullptr) {
    connector_->Cancel();
  }
  if (conn_ != nullptr) {
    // Runs OnUpstreamClosed(), which ignores it because done_ is set.
    conn_->ForceClose();
  }
}

}  // namespace sluice::proxy
