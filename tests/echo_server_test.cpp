#include "echo/echo_server.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "net/event_loop.h"
#include "net/inet_address.h"
#include "result.h"
#include "test_client.h"

namespace sluice::echo {
namespace {

using net::EventLoop;
using net::InetAddress;
using test::MakePayload;
using test::TestClient;

class EchoServerTest : public ::testing::Test {
 protected:
  void StartServer(size_t high_water_mark = EchoServer::kDefaultHighWaterMark) {
    Result<std::unique_ptr<EventLoop>> loop = EventLoop::Create();
    ASSERT_TRUE(loop.has_value()) << loop.error().message();
    loop_ = *std::move(loop);
    // Port 0: the kernel picks a free port, so tests never collide.
    Result<std::unique_ptr<EchoServer>> server = EchoServer::Create(
        loop_.get(), InetAddress::Loopback(0), high_water_mark);
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
    // The server must go before the loop it is registered with.
    server_.reset();
    loop_.reset();
  }

  std::unique_ptr<EventLoop> loop_;
  std::unique_ptr<EchoServer> server_;
  InetAddress address_;
};

TEST_F(EchoServerTest, EchoesMessage) {
  StartServer();
  TestClient client(address_);
  client.QueueSend("hello, sluice");

  EXPECT_TRUE(RunUntil({&client},
                       [&] { return client.received() == "hello, sluice"; }));
}

TEST_F(EchoServerTest, ClientsAreIndependent) {
  StartServer();
  TestClient a(address_);
  TestClient b(address_);
  TestClient c(address_);
  a.QueueSend("alpha");
  b.QueueSend("bravo");
  c.QueueSend("charlie");

  EXPECT_TRUE(RunUntil({&a, &b, &c}, [&] {
    return a.received() == "alpha" && b.received() == "bravo" &&
           c.received() == "charlie";
  }));
  EXPECT_EQ(server_->connection_count(), 3U);
}

TEST_F(EchoServerTest, ConnectionIsDestroyedAfterPeerCloses) {
  StartServer();
  TestClient client(address_);
  ASSERT_TRUE(
      RunUntil({&client}, [&] { return server_->connection_count() == 1; }));

  client.Close();
  EXPECT_TRUE(RunUntil({}, [&] { return server_->connection_count() == 0; }));
  // Only the listening socket remains registered with epoll.
  EXPECT_EQ(loop_->registered_channel_count(), 1U);
}

TEST_F(EchoServerTest, FlushesPendingOutputBeforeClosingOnHalfClose) {
  StartServer();
  const std::string payload = MakePayload(size_t{4} * 1024 * 1024);
  TestClient client(address_);
  client.QueueSend(payload);
  client.ShutdownAfterSend();

  // The server sees EOF while echo output may still be queued; it must
  // flush everything before closing its side.
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.eof(); }));
  EXPECT_EQ(client.received().size(), payload.size());
  EXPECT_TRUE(client.received() == payload);
  EXPECT_TRUE(RunUntil({}, [&] { return server_->connection_count() == 0; }));
}

TEST_F(EchoServerTest, PausesReadingWhenPeerDoesNotRead) {
  constexpr size_t kHighWaterMark = size_t{64} * 1024;
  StartServer(kHighWaterMark);
  const std::string payload = MakePayload(size_t{16} * 1024 * 1024);
  TestClient client(address_, /*recv_buffer_size=*/4096);
  client.set_reading(false);
  client.QueueSend(payload);

  // The client keeps sending but never reads, so the echo backs up until
  // the server's output buffer crosses the high-water mark.
  ASSERT_TRUE(RunUntil({&client}, [&] { return server_->pause_count() > 0; }));
  EXPECT_LT(client.sent(), payload.size());

  // Once the client reads again, the server resumes and every byte comes
  // back intact and in order.
  client.set_reading(true);
  ASSERT_TRUE(RunUntil(
      {&client}, [&] { return client.received().size() == payload.size(); }));
  EXPECT_TRUE(client.received() == payload);
}

}  // namespace
}  // namespace sluice::echo
