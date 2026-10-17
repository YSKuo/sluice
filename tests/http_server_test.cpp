#include "http/http_server.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "http/request.h"
#include "http/request_parser.h"
#include "http/response.h"
#include "http/status.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "result.h"
#include "test_client.h"

namespace sluice::http {
namespace {

using net::EventLoop;
using net::InetAddress;
using test::TestClient;

// The response the test handler produces for `method` + `target` (+ body).
Response Describe(const std::string& method, const std::string& target,
                  const std::string& body = "") {
  Response response;
  response.body = method + " " + target + " " + body;
  return response;
}

std::string Expected(const std::string& method, const std::string& target,
                     bool keep_alive, const std::string& body = "") {
  return Serialize(Describe(method, target, body), keep_alive);
}

class HttpServerTest : public ::testing::Test {
 protected:
  void StartServer(ParserLimits limits = {}) {
    Result<std::unique_ptr<EventLoop>> loop = EventLoop::Create();
    ASSERT_TRUE(loop.has_value()) << loop.error().message();
    loop_ = *std::move(loop);
    Result<std::unique_ptr<HttpServer>> server = HttpServer::Create(
        loop_.get(), InetAddress::Loopback(0),
        [this](const Request& request) {
          ++handler_calls_;
          if (large_response_size_ > 0) {
            Response response;
            response.body = test::MakePayload(large_response_size_);
            return response;
          }
          return Describe(request.method, request.target, request.body);
        },
        limits);
    ASSERT_TRUE(server.has_value()) << server.error().message();
    server_ = *std::move(server);
    const Result<void> started = server_->Start();
    ASSERT_TRUE(started.has_value()) << started.error().message();
    const Result<InetAddress> address = server_->LocalAddress();
    ASSERT_TRUE(address.has_value()) << address.error().message();
    address_ = *address;
  }

  bool RunUntil(const std::vector<TestClient*>& clients,
                const std::function<bool()>& done) {
    return test::RunUntil(*loop_, clients, done);
  }

  void TearDown() override {
    server_.reset();
    loop_.reset();
  }

  std::unique_ptr<EventLoop> loop_;
  std::unique_ptr<HttpServer> server_;
  InetAddress address_;
  int handler_calls_ = 0;
  size_t large_response_size_ = 0;
};

TEST_F(HttpServerTest, KeepAliveServesSequentialRequests) {
  StartServer();
  TestClient client(address_);

  client.QueueSend("GET /one HTTP/1.1\r\nHost: a\r\n\r\n");
  const std::string first = Expected("GET", "/one", /*keep_alive=*/true);
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.received() == first; }));

  client.QueueSend("GET /two HTTP/1.1\r\nHost: a\r\n\r\n");
  const std::string both = first + Expected("GET", "/two", true);
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.received() == both; }));

  // Both requests used the same, still open, connection.
  EXPECT_EQ(server_->connection_count(), 1U);
  EXPECT_FALSE(client.eof());
}

TEST_F(HttpServerTest, PipelinedRequestsAreAnsweredInOrder) {
  StartServer();
  TestClient client(address_);
  client.QueueSend(
      "GET /a HTTP/1.1\r\nHost: a\r\n\r\n"
      "POST /b HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n\r\nbody"
      "GET /c HTTP/1.1\r\nHost: a\r\n\r\n");
  const std::string expected = Expected("GET", "/a", true) +
                               Expected("POST", "/b", true, "body") +
                               Expected("GET", "/c", true);
  EXPECT_TRUE(
      RunUntil({&client}, [&] { return client.received() == expected; }));
}

TEST_F(HttpServerTest, ConnectionCloseIsHonored) {
  StartServer();
  TestClient client(address_);
  client.QueueSend("GET /bye HTTP/1.1\r\nHost: a\r\nConnection: close\r\n\r\n");
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.eof(); }));
  EXPECT_EQ(client.received(), Expected("GET", "/bye", /*keep_alive=*/false));
  EXPECT_TRUE(RunUntil({}, [&] { return server_->connection_count() == 0; }));
}

TEST_F(HttpServerTest, Http10ClosesByDefault) {
  StartServer();
  TestClient client(address_);
  client.QueueSend("GET /old HTTP/1.0\r\n\r\n");
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.eof(); }));
  EXPECT_EQ(client.received(), Expected("GET", "/old", /*keep_alive=*/false));
}

TEST_F(HttpServerTest, MalformedRequestGets400AndClose) {
  StartServer();
  TestClient client(address_);
  // A valid request, then a broken one, then one that must be ignored.
  client.QueueSend(
      "GET /ok HTTP/1.1\r\nHost: a\r\n\r\n"
      "GET /bad HTTP/1.1\r\nHost a\r\n\r\n"
      "GET /ignored HTTP/1.1\r\nHost: a\r\n\r\n");
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.eof(); }));
  EXPECT_EQ(client.received(),
            Expected("GET", "/ok", true) +
                Serialize(MakeErrorResponse(StatusCode::kBadRequest),
                          /*keep_alive=*/false));
  EXPECT_EQ(handler_calls_, 1);
}

TEST_F(HttpServerTest, OversizedHeadersGet431) {
  ParserLimits limits;
  limits.max_header_bytes = 256;
  StartServer(limits);
  TestClient client(address_);
  client.QueueSend("GET / HTTP/1.1\r\nHost: a\r\nX-Big: " +
                   std::string(1000, 'x') + "\r\n\r\n");
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.eof(); }));
  EXPECT_EQ(
      client.received(),
      Serialize(MakeErrorResponse(StatusCode::kRequestHeaderFieldsTooLarge),
                false));
  EXPECT_EQ(handler_calls_, 0);
}

TEST_F(HttpServerTest, OneResponseInFlightAtATime) {
  // Each response is far larger than the kernel's socket buffers, so it
  // stays partly queued while the client is not reading.
  large_response_size_ = size_t{8} * 1024 * 1024;
  StartServer();
  TestClient client(address_, /*recv_buffer_size=*/4096);
  client.set_reading(false);
  for (int i = 0; i < 3; ++i) {
    client.QueueSend("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
  }

  ASSERT_TRUE(RunUntil({&client}, [&] { return handler_calls_ == 1; }));
  for (int i = 0; i < 50; ++i) {
    client.Pump();
    loop_->PollOnce(1);
  }
  // The first response is still being flushed, so the other two requests
  // must not have been handled (and their responses not buffered) yet.
  EXPECT_EQ(handler_calls_, 1);

  client.set_reading(true);
  Response big;
  big.body = test::MakePayload(large_response_size_);
  const std::string one = Serialize(big, /*keep_alive=*/true);
  ASSERT_TRUE(RunUntil(
      {&client}, [&] { return client.received().size() == 3 * one.size(); }));
  EXPECT_EQ(handler_calls_, 3);
  EXPECT_TRUE(client.received() == one + one + one);
}

TEST_F(HttpServerTest, ClosedConnectionsReleaseState) {
  StartServer();
  {
    TestClient client(address_);
    client.QueueSend("GET / HTTP/1.1\r\nHost: a\r\n\r\n");
    ASSERT_TRUE(
        RunUntil({&client}, [&] { return !client.received().empty(); }));
  }
  // The client socket closed when it went out of scope.
  EXPECT_TRUE(RunUntil({}, [&] { return server_->connection_count() == 0; }));
  EXPECT_EQ(loop_->registered_channel_count(), 1U);
}

}  // namespace
}  // namespace sluice::http
