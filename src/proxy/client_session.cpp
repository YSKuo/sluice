#include "proxy/client_session.h"

#include <string>
#include <utility>

#include "http/response.h"
#include "net/buffer.h"
#include "proxy/forwarding.h"

namespace sluice::proxy {

ClientSession::ClientSession(net::EventLoop* loop, net::TcpConnection* client,
                             RouteTable* routes, const ProxyOptions* options,
                             ProxyStats* stats)
    : loop_(loop),
      client_(client),
      routes_(routes),
      options_(options),
      stats_(stats),
      parser_(options->limits),
      read_timer_(loop) {}

void ClientSession::Start() { UpdateReadTimer(); }

void ClientSession::OnInput() {
  // While proxying, client reading is paused; anything that was already
  // read stays buffered until the exchange ends.
  if (state_ == State::kReadingRequest) {
    ProcessRequests();
  }
}

void ClientSession::OnClientDrained() {
  if (state_ == State::kProxying) {
    if (upstream_paused_ && call_ != nullptr) {
      upstream_paused_ = false;
      call_->ResumeReading();
    }
    return;
  }
  if (state_ == State::kReadingRequest) {
    ProcessRequests();
  }
}

void ClientSession::OnClientClosed() {
  state_ = State::kClosed;
  read_timer_.Disarm();
  if (call_ != nullptr) {
    call_->Cancel();
  }
}

void ClientSession::ProcessRequests() {
  net::Buffer& input = client_->input();
  while (state_ == State::kReadingRequest && client_->connected()) {
    if (client_->PendingOutputBytes() > 0) {
      // The previous response is still flushing; OnClientDrained() resumes.
      client_->PauseReading();
      UpdateReadTimer();
      return;
    }
    const http::RequestParser::FeedResult result = parser_.Feed(input.Peek());
    input.Retrieve(result.consumed);
    switch (result.status) {
      case http::RequestParser::Status::kNeedMore:
        client_->ResumeReading();
        UpdateReadTimer();
        return;
      case http::RequestParser::Status::kError:
        SendLocalResponse(parser_.error_status(), /*keep_alive=*/false);
        return;
      case http::RequestParser::Status::kComplete:
        HandleRequest(parser_.TakeRequest());
        break;
    }
  }
}

void ClientSession::HandleRequest(const http::Request& request) {
  read_timer_.Disarm();
  read_timer_kind_ = ReadTimer::kNone;
  ++stats_->requests;
  keep_alive_ = http::IsKeepAlive(request);

  if (HasDotSegment(request.target)) {
    SendLocalResponse(http::StatusCode::kBadRequest, keep_alive_);
    return;
  }
  RouteTable::Route* route = routes_->Match(request.target);
  if (route == nullptr) {
    SendLocalResponse(http::StatusCode::kNotFound, keep_alive_);
    return;
  }
  Upstream* upstream = route->balancer.Pick(Clock::now());
  if (upstream == nullptr) {
    // Every upstream of the route is marked down.
    SendLocalResponse(http::StatusCode::kServiceUnavailable, keep_alive_);
    return;
  }

  Result<std::unique_ptr<UpstreamCall>> call = UpstreamCall::Start(
      loop_, upstream->address(),
      BuildUpstreamRequest(request, client_->peer(), upstream->name()),
      request.method,
      UpstreamCall::Options{
          .connect_timeout = options_->upstream_connect_timeout,
          .idle_timeout = options_->upstream_idle_timeout,
          .limits = options_->limits},
      UpstreamCall::Callbacks{
          .on_head =
              [this](const http::ResponseHead& head,
                     const http::BodyLength& length) {
                OnUpstreamHead(head, length);
              },
          .on_body = [this](std::string_view data) { OnUpstreamBody(data); },
          .on_complete = [this] { OnUpstreamComplete(); },
          .on_failure =
              [this](UpstreamCall::Failure failure) {
                OnUpstreamFailure(failure);
              },
      });
  if (!call) {
    ++stats_->upstream_failures;
    upstream->MarkDown(Clock::now(), options_->upstream_down_cooldown);
    SendLocalResponse(http::StatusCode::kBadGateway, keep_alive_);
    return;
  }
  call_ = *std::move(call);
  upstream_ = upstream;
  head_sent_ = false;
  upstream_paused_ = false;
  state_ = State::kProxying;
  client_->PauseReading();
}

void ClientSession::SendLocalResponse(http::StatusCode status,
                                      bool keep_alive) {
  client_->Send(http::Serialize(http::MakeErrorResponse(status), keep_alive));
  if (!keep_alive) {
    client_->Shutdown();
  }
}

void ClientSession::UpdateReadTimer() {
  if (state_ != State::kReadingRequest || !client_->connected()) {
    return;
  }
  if (client_->input().empty()) {
    if (read_timer_kind_ != ReadTimer::kIdle) {
      read_timer_kind_ = ReadTimer::kIdle;
      // Closing an idle keep-alive connection needs no response.
      read_timer_.Arm(options_->client_idle_timeout,
                      [this] { client_->Shutdown(); });
    }
    return;
  }
  if (read_timer_kind_ != ReadTimer::kRequest) {
    read_timer_kind_ = ReadTimer::kRequest;
    read_timer_.Arm(options_->client_request_timeout, [this] {
      SendLocalResponse(http::StatusCode::kRequestTimeout,
                        /*keep_alive=*/false);
    });
  }
}

void ClientSession::OnUpstreamHead(const http::ResponseHead& head,
                                   const http::BodyLength& length) {
  if (state_ != State::kProxying) {
    return;
  }
  // A body that ends when the upstream closes can only be passed on by
  // closing the client connection too.
  if (length.kind == http::BodyLength::Kind::kUntilClose) {
    keep_alive_ = false;
  }
  client_->Send(BuildClientResponseHead(head, keep_alive_));
  head_sent_ = true;
  ApplyBackpressure();
}

void ClientSession::OnUpstreamBody(std::string_view data) {
  if (state_ != State::kProxying) {
    return;
  }
  client_->Send(data);
  ApplyBackpressure();
}

void ClientSession::ApplyBackpressure() {
  if (!upstream_paused_ && call_ != nullptr &&
      client_->PendingOutputBytes() > options_->response_high_water_mark) {
    upstream_paused_ = true;
    ++stats_->upstream_read_pauses;
    call_->PauseReading();
  }
}

void ClientSession::OnUpstreamComplete() {
  if (state_ != State::kProxying) {
    return;
  }
  EndExchange();
  if (!keep_alive_) {
    client_->Shutdown();
    return;
  }
  ProcessRequests();
}

void ClientSession::OnUpstreamFailure(UpstreamCall::Failure failure) {
  if (state_ != State::kProxying) {
    return;
  }
  ++stats_->upstream_failures;
  if (failure == UpstreamCall::Failure::kConnectFailed ||
      failure == UpstreamCall::Failure::kConnectTimeout) {
    upstream_->MarkDown(Clock::now(), options_->upstream_down_cooldown);
  }
  const bool head_sent = head_sent_;
  EndExchange();
  if (head_sent) {
    // Part of the response is already on its way; the only honest signal
    // left is to close the connection (the client sees a short body).
    client_->Shutdown();
    return;
  }
  const bool timed_out = failure == UpstreamCall::Failure::kConnectTimeout ||
                         failure == UpstreamCall::Failure::kResponseTimeout;
  SendLocalResponse(timed_out ? http::StatusCode::kGatewayTimeout
                              : http::StatusCode::kBadGateway,
                    keep_alive_);
  if (keep_alive_) {
    ProcessRequests();
  }
}

void ClientSession::EndExchange() {
  state_ = State::kReadingRequest;
  upstream_ = nullptr;
  head_sent_ = false;
  upstream_paused_ = false;
  if (call_ != nullptr) {
    // We are inside one of the call's callbacks.
    loop_->DeleteLater(std::move(call_));
  }
}

}  // namespace sluice::proxy
