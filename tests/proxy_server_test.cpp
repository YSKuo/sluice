#include "proxy/proxy_server.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "http/http_server.h"
#include "http/request.h"
#include "http/response.h"
#include "http/response_parser.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/socket.h"
#include "net/unique_fd.h"
#include "proxy/proxy_options.h"
#include "proxy/rate_limiter.h"
#include "proxy/route_table.h"
#include "result.h"
#include "test_client.h"

namespace sluice::proxy {
namespace {

using std::chrono::milliseconds;
using test::TestClient;

// A response as the client sees it.
struct ClientResponse {
  http::ResponseHead head;
  std::string body;
};

// Splits everything a client received into responses. Stops at the first
// incomplete one, so callers compare the count they expect.
std::vector<ClientResponse> ParseResponses(std::string_view bytes,
                                           std::string_view method = "GET") {
  std::vector<ClientResponse> responses;
  while (!bytes.empty()) {
    http::ResponseParser parser;
    const http::ResponseParser::FeedResult result = parser.Feed(bytes);
    if (result.status != http::ResponseParser::Status::kComplete) {
      break;
    }
    bytes.remove_prefix(result.consumed);
    ClientResponse response{.head = parser.TakeHead(), .body = {}};
    const std::optional<http::BodyLength> length =
        http::GetBodyLength(response.head, method);
    if (!length || length->kind != http::BodyLength::Kind::kLength) {
      response.body = std::string(bytes);  // Until close.
      bytes = {};
    } else {
      if (bytes.size() < length->length) {
        break;
      }
      response.body = std::string(bytes.substr(0, length->length));
      bytes.remove_prefix(length->length);
    }
    responses.push_back(std::move(response));
  }
  return responses;
}

// A raw TCP upstream the test drives byte by byte, for behaviors a real
// server would not produce (silence, truncated or malformed responses).
class RawUpstream {
 public:
  RawUpstream() {
    Result<net::Socket> socket = net::Socket::CreateNonBlockingTcp();
    EXPECT_TRUE(socket.has_value());
    listener_.emplace(*std::move(socket));
    EXPECT_TRUE(listener_->Bind(net::InetAddress::Loopback(0)).has_value());
    EXPECT_TRUE(listener_->Listen(16).has_value());
    address_ = listener_->LocalAddress().value_or(net::InetAddress());
  }

  [[nodiscard]] const net::InetAddress& address() const { return address_; }

  // Accepts the proxy's connection once it has arrived.
  bool TryAccept() {
    if (conn_) {
      return true;
    }
    Result<net::AcceptedSocket> accepted = listener_->Accept();
    if (accepted) {
      conn_.emplace(std::move(accepted->socket));
    }
    return conn_.has_value();
  }

  // Reads whatever the proxy sent so far.
  const std::string& Received() {
    if (conn_) {
      std::array<char, 4096> buf{};
      while (true) {
        const Result<size_t> n = conn_->Read(buf);
        if (!n || *n == 0) {
          break;
        }
        received_.append(buf.data(), *n);
      }
    }
    return received_;
  }

  void Send(std::string_view data) {
    ASSERT_TRUE(conn_.has_value());
    const Result<size_t> n = conn_->Write(data);
    ASSERT_TRUE(n.has_value());
    ASSERT_EQ(*n, data.size());
  }

  // Closes the connection. Callers first read the whole request: closing a
  // socket with unread data makes the kernel send RST instead of FIN, and
  // the RST discards the response bytes the proxy has not read yet.
  void Close() { conn_.reset(); }

 private:
  std::optional<net::Socket> listener_;
  net::InetAddress address_;
  std::optional<net::Socket> conn_;
  std::string received_;
};

class ProxyServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Result<std::unique_ptr<net::EventLoop>> loop = net::EventLoop::Create();
    ASSERT_TRUE(loop.has_value()) << loop.error().message();
    loop_ = *std::move(loop);
    options_.client_idle_timeout = milliseconds(5'000);
    options_.client_request_timeout = milliseconds(5'000);
    options_.upstream_connect_timeout = milliseconds(1'000);
    options_.upstream_idle_timeout = milliseconds(5'000);
    options_.upstream_down_cooldown = milliseconds(60'000);
  }

  void TearDown() override {
    proxy_.reset();
    upstreams_.clear();
    loop_.reset();
  }

  // Starts a real HTTP upstream on the test's loop. Its handler answers
  // with `name`, the request line, every header it received, and the body,
  // or with `large_body_` bytes if that is set.
  net::InetAddress StartUpstream(const std::string& name) {
    Result<std::unique_ptr<http::HttpServer>> server = http::HttpServer::Create(
        loop_.get(), net::InetAddress::Loopback(0),
        [this, name](const http::Request& request) {
          http::Response response;
          if (large_body_ > 0) {
            response.body = test::MakePayload(large_body_);
            return response;
          }
          response.headers.Add("X-Upstream", name);
          response.body =
              name + "\n" + request.method + " " + request.target + "\n";
          for (const http::Headers::Field& field : request.headers.fields()) {
            response.body += field.name + ": " + field.value + "\n";
          }
          response.body += "body: " + request.body;
          return response;
        });
    EXPECT_TRUE(server.has_value());
    EXPECT_TRUE((*server)->Start().has_value());
    const net::InetAddress address =
        (*server)->LocalAddress().value_or(net::InetAddress());
    upstreams_.push_back(*std::move(server));
    return address;
  }

  // An address nothing listens on: bound once to get a free port, then
  // closed, so connecting to it is refused.
  static net::InetAddress DeadAddress() {
    Result<net::Socket> socket = net::Socket::CreateNonBlockingTcp();
    EXPECT_TRUE(socket.has_value());
    EXPECT_TRUE(socket->Bind(net::InetAddress::Loopback(0)).has_value());
    return socket->LocalAddress().value_or(net::InetAddress());
  }

  void StartProxy(
      const std::vector<std::pair<std::string, std::vector<net::InetAddress>>>&
          routes) {
    RouteTable table;
    for (const auto& [prefix, addresses] : routes) {
      std::vector<std::pair<std::string, net::InetAddress>> targets;
      for (const net::InetAddress& address : addresses) {
        targets.emplace_back(address.ToString(), address);
      }
      ASSERT_TRUE(table.AddRoute(prefix, targets).has_value());
    }
    Result<std::unique_ptr<ProxyServer>> proxy = ProxyServer::Create(
        loop_.get(), net::InetAddress::Loopback(0), std::move(table), options_);
    ASSERT_TRUE(proxy.has_value()) << proxy.error().message();
    proxy_ = *std::move(proxy);
    ASSERT_TRUE(proxy_->Start().has_value());
    proxy_address_ = proxy_->LocalAddress().value_or(net::InetAddress());
  }

  bool RunUntil(const std::vector<TestClient*>& clients,
                const std::function<bool()>& done) {
    return test::RunUntil(*loop_, clients, done);
  }

  // Sends one request on a fresh connection and waits for the response.
  ClientResponse Fetch(const std::string& request) {
    TestClient client(proxy_address_);
    client.QueueSend(request);
    std::vector<ClientResponse> responses;
    EXPECT_TRUE(RunUntil({&client}, [&] {
      responses = ParseResponses(client.received());
      return !responses.empty();
    }));
    return responses.empty() ? ClientResponse{} : responses[0];
  }

  // Waits until `upstream` has accepted the proxy's connection and read the
  // whole (bodiless) request.
  bool AwaitRequest(TestClient& client, RawUpstream& upstream) {
    return RunUntil({&client}, [&upstream] {
      return upstream.TryAccept() && upstream.Received().ends_with("\r\n\r\n");
    });
  }

  static std::string Get(const std::string& target,
                         const std::string& extra_headers = "") {
    return "GET " + target + " HTTP/1.1\r\nHost: gateway.test\r\n" +
           extra_headers + "\r\n";
  }

  std::unique_ptr<net::EventLoop> loop_;
  ProxyOptions options_;
  std::vector<std::unique_ptr<http::HttpServer>> upstreams_;
  std::unique_ptr<ProxyServer> proxy_;
  net::InetAddress proxy_address_;
  size_t large_body_ = 0;
};

TEST_F(ProxyServerTest, ForwardsRequestAndResponse) {
  StartProxy({{"/", {StartUpstream("a")}}});
  const ClientResponse response = Fetch(Get("/hello?x=1"));
  EXPECT_EQ(response.head.status, 200);
  EXPECT_EQ(response.head.headers.Get("X-Upstream").value_or(""), "a");
  EXPECT_EQ(response.head.headers.Get("Connection").value_or(""), "keep-alive");
  EXPECT_TRUE(response.body.starts_with("a\nGET /hello?x=1\n"))
      << response.body;
}

TEST_F(ProxyServerTest, RewritesHeadersOnTheWayUpstream) {
  StartProxy({{"/", {StartUpstream("a")}}});
  const ClientResponse response =
      Fetch(Get("/",
                "Connection: keep-alive, X-Secret\r\nX-Secret: s\r\n"
                "Keep-Alive: timeout=5\r\nX-Custom: kept\r\n"));
  const std::string& seen = response.body;  // Headers the upstream got.
  EXPECT_NE(seen.find("Host: gateway.test\n"), std::string::npos) << seen;
  EXPECT_NE(seen.find("X-Custom: kept\n"), std::string::npos) << seen;
  EXPECT_NE(seen.find("X-Forwarded-For: 127.0.0.1\n"), std::string::npos)
      << seen;
  EXPECT_NE(seen.find("Connection: close\n"), std::string::npos) << seen;
  EXPECT_EQ(seen.find("X-Secret"), std::string::npos) << seen;
  EXPECT_EQ(seen.find("Keep-Alive"), std::string::npos) << seen;
}

TEST_F(ProxyServerTest, ForwardsRequestBody) {
  StartProxy({{"/", {StartUpstream("a")}}});
  const ClientResponse response = Fetch(
      "POST /submit HTTP/1.1\r\nHost: g\r\nContent-Length: 5\r\n\r\nhello");
  EXPECT_TRUE(response.body.starts_with("a\nPOST /submit\n"));
  EXPECT_TRUE(response.body.ends_with("body: hello")) << response.body;
}

TEST_F(ProxyServerTest, KeepAliveAndPipeliningKeepOrder) {
  StartProxy({{"/", {StartUpstream("a")}}});
  TestClient client(proxy_address_);
  client.QueueSend(Get("/one") + Get("/two") + Get("/three"));
  std::vector<ClientResponse> responses;
  ASSERT_TRUE(RunUntil({&client}, [&] {
    responses = ParseResponses(client.received());
    return responses.size() == 3;
  }));
  EXPECT_TRUE(responses[0].body.starts_with("a\nGET /one\n"));
  EXPECT_TRUE(responses[1].body.starts_with("a\nGET /two\n"));
  EXPECT_TRUE(responses[2].body.starts_with("a\nGET /three\n"));
  EXPECT_EQ(proxy_->connection_count(), 1U);
}

TEST_F(ProxyServerTest, RoundRobinAcrossUpstreams) {
  StartProxy({{"/", {StartUpstream("a"), StartUpstream("b")}}});
  std::string order;
  for (int i = 0; i < 4; ++i) {
    order += Fetch(Get("/")).head.headers.Get("X-Upstream").value_or("?");
  }
  EXPECT_EQ(order, "abab");
}

TEST_F(ProxyServerTest, RoutesByLongestPrefix) {
  StartProxy(
      {{"/", {StartUpstream("root")}}, {"/api/", {StartUpstream("api")}}});
  const auto upstream_for = [this](const std::string& target) {
    return std::string(
        Fetch(Get(target)).head.headers.Get("X-Upstream").value_or("?"));
  };
  EXPECT_EQ(upstream_for("/api/users"), "api");
  EXPECT_EQ(upstream_for("/apikey"), "root");
  EXPECT_EQ(upstream_for("/index.html"), "root");
}

TEST_F(ProxyServerTest, LocalErrors) {
  StartProxy({{"/api", {StartUpstream("a")}}});
  EXPECT_EQ(Fetch(Get("/other")).head.status, 404);
  EXPECT_EQ(Fetch(Get("/api/../admin")).head.status, 400);
}

TEST_F(ProxyServerTest, FailedUpstreamIsMarkedDownAndSkipped) {
  StartProxy({{"/", {DeadAddress(), StartUpstream("b")}}});
  EXPECT_EQ(Fetch(Get("/")).head.status, 502);  // Round robin tried dead.
  EXPECT_EQ(proxy_->stats().upstream_failures, 1U);
  // The dead upstream is now skipped: every request reaches b.
  for (int i = 0; i < 3; ++i) {
    const ClientResponse response = Fetch(Get("/"));
    EXPECT_EQ(response.head.status, 200);
    EXPECT_EQ(response.head.headers.Get("X-Upstream").value_or(""), "b");
  }
  EXPECT_EQ(proxy_->stats().upstream_failures, 1U);
}

TEST_F(ProxyServerTest, AllUpstreamsDownGives503) {
  StartProxy({{"/", {DeadAddress()}}});
  EXPECT_EQ(Fetch(Get("/")).head.status, 502);
  EXPECT_EQ(Fetch(Get("/")).head.status, 503);
}

TEST_F(ProxyServerTest, SilentUpstreamGives504) {
  options_.upstream_idle_timeout = milliseconds(100);
  RawUpstream upstream;
  StartProxy({{"/", {upstream.address()}}});
  TestClient client(proxy_address_);
  client.QueueSend(Get("/"));
  // The upstream accepts and reads the request but never answers.
  ASSERT_TRUE(AwaitRequest(client, upstream));
  std::vector<ClientResponse> responses;
  ASSERT_TRUE(RunUntil({&client}, [&] {
    responses = ParseResponses(client.received());
    return !responses.empty();
  }));
  EXPECT_EQ(responses[0].head.status, 504);
  // A silent upstream is not a connect failure: it stays in rotation.
  EXPECT_TRUE(
      proxy_->routes().routes()[0].balancer.upstreams()[0]->IsUp(Clock::now()));
}

TEST_F(ProxyServerTest, MalformedUpstreamResponseGives502) {
  for (const std::string_view reply :
       {std::string_view("garbage\r\n\r\n"),
        std::string_view(
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n")}) {
    RawUpstream upstream;
    StartProxy({{"/", {upstream.address()}}});
    TestClient client(proxy_address_);
    client.QueueSend(Get("/"));
    ASSERT_TRUE(AwaitRequest(client, upstream));
    upstream.Send(reply);
    std::vector<ClientResponse> responses;
    ASSERT_TRUE(RunUntil({&client}, [&] {
      responses = ParseResponses(client.received());
      return !responses.empty();
    }));
    EXPECT_EQ(responses[0].head.status, 502) << reply;
  }
}

TEST_F(ProxyServerTest, CloseDelimitedBodyClosesClient) {
  RawUpstream upstream;
  StartProxy({{"/", {upstream.address()}}});
  TestClient client(proxy_address_);
  client.QueueSend(Get("/"));
  ASSERT_TRUE(AwaitRequest(client, upstream));
  upstream.Send("HTTP/1.0 200 OK\r\nX-Kind: raw\r\n\r\nhello");
  upstream.Close();
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.eof(); }));
  const std::vector<ClientResponse> responses =
      ParseResponses(client.received());
  ASSERT_EQ(responses.size(), 1U);
  EXPECT_EQ(responses[0].head.headers.Get("Connection").value_or(""), "close");
  EXPECT_EQ(responses[0].body, "hello");
}

TEST_F(ProxyServerTest, TruncatedUpstreamBodyClosesClient) {
  RawUpstream upstream;
  StartProxy({{"/", {upstream.address()}}});
  TestClient client(proxy_address_);
  client.QueueSend(Get("/"));
  ASSERT_TRUE(AwaitRequest(client, upstream));
  upstream.Send("HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\n0123456789");
  upstream.Close();
  // The head and the 10 bytes are passed on, then the connection closes:
  // the short body tells the client the response is incomplete.
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.eof(); }));
  EXPECT_TRUE(client.received().ends_with("\r\n\r\n0123456789"))
      << client.received();
}

TEST_F(ProxyServerTest, HeadRequestHasNoBody) {
  StartProxy({{"/", {StartUpstream("a")}}});
  TestClient client(proxy_address_);
  // The test upstream sends a body even for HEAD; the proxy must drop it
  // and keep the client connection in sync for the next request.
  client.QueueSend("HEAD / HTTP/1.1\r\nHost: g\r\n\r\n" + Get("/next"));
  ASSERT_TRUE(RunUntil({&client}, [&] {
    const std::vector<ClientResponse> head_only =
        ParseResponses(client.received(), "HEAD");
    return !head_only.empty() &&
           client.received().find("GET /next") != std::string::npos;
  }));
  const std::string& bytes = client.received();
  const size_t second = bytes.find("HTTP/1.1", 1);
  ASSERT_NE(second, std::string::npos);
  EXPECT_TRUE(bytes.substr(0, second).ends_with("\r\n\r\n"))
      << "HEAD response carried a body";
}

TEST_F(ProxyServerTest, RequestTimeoutGives408) {
  options_.client_request_timeout = milliseconds(100);
  StartProxy({{"/", {StartUpstream("a")}}});
  TestClient client(proxy_address_);
  client.QueueSend("GET / HTTP/1.1\r\nHost:");  // Never finishes.
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.eof(); }));
  EXPECT_TRUE(client.received().starts_with("HTTP/1.1 408 Request Timeout"));
}

TEST_F(ProxyServerTest, IdleKeepAliveConnectionIsClosed) {
  options_.client_idle_timeout = milliseconds(100);
  StartProxy({{"/", {StartUpstream("a")}}});
  TestClient client(proxy_address_);
  client.QueueSend(Get("/"));
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.eof(); }));
  // One response, then a silent close once the connection sat idle.
  EXPECT_EQ(ParseResponses(client.received()).size(), 1U);
}

TEST_F(ProxyServerTest, SlowClientPausesUpstream) {
  large_body_ = size_t{8} * 1024 * 1024;
  StartProxy({{"/", {StartUpstream("big")}}});
  TestClient client(proxy_address_, /*recv_buffer_size=*/4096);
  client.set_reading(false);
  client.QueueSend(Get("/"));
  ASSERT_TRUE(RunUntil(
      {&client}, [&] { return proxy_->stats().upstream_read_pauses > 0; }));

  client.set_reading(true);
  std::vector<ClientResponse> responses;
  ASSERT_TRUE(RunUntil({&client}, [&] {
    responses = ParseResponses(client.received());
    return !responses.empty();
  }));
  EXPECT_TRUE(responses[0].body == test::MakePayload(large_body_));
}

TEST_F(ProxyServerTest, RateLimitRejectsExcessRequestsWith429) {
  // Two requests at once, then one every 10 s: the third is rejected.
  options_.rate_limit = RateLimit{.requests_per_second = 0.1, .burst = 2};
  StartProxy({{"/", {StartUpstream("a")}}});
  TestClient client(proxy_address_);
  client.QueueSend(Get("/1") + Get("/2") + Get("/3"));
  std::vector<ClientResponse> responses;
  ASSERT_TRUE(RunUntil({&client}, [&] {
    responses = ParseResponses(client.received());
    return responses.size() == 3;
  }));
  EXPECT_EQ(responses[0].head.status, 200);
  EXPECT_EQ(responses[1].head.status, 200);
  EXPECT_EQ(responses[2].head.status, 429);
  EXPECT_EQ(responses[2].head.headers.Get("Retry-After").value_or(""), "10");
  // The connection stays usable, so the client can retry later.
  EXPECT_EQ(responses[2].head.headers.Get("Connection").value_or(""),
            "keep-alive");
  EXPECT_EQ(proxy_->stats().rate_limited, 1U);
  ASSERT_NE(proxy_->rate_limiter(), nullptr);
  EXPECT_EQ(proxy_->rate_limiter()->tracked_clients(), 1U);
}

TEST_F(ProxyServerTest, ClientDisconnectCancelsUpstream) {
  large_body_ = size_t{8} * 1024 * 1024;
  StartProxy({{"/", {StartUpstream("big")}}});
  {
    TestClient client(proxy_address_, /*recv_buffer_size=*/4096);
    client.set_reading(false);
    client.QueueSend(Get("/"));
    ASSERT_TRUE(RunUntil(
        {&client}, [&] { return proxy_->stats().upstream_read_pauses > 0; }));
  }
  // The client is gone: its session and the upstream connection go too.
  EXPECT_TRUE(RunUntil({}, [&] {
    return proxy_->connection_count() == 0 &&
           upstreams_[0]->connection_count() == 0;
  }));
}

}  // namespace
}  // namespace sluice::proxy
