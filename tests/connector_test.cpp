#include "net/connector.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>

#include "gtest/gtest.h"

#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/socket.h"
#include "result.h"

namespace sluice::net {
namespace {

class ConnectorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Result<std::unique_ptr<EventLoop>> loop = EventLoop::Create();
    ASSERT_TRUE(loop.has_value()) << loop.error().message();
    loop_ = *std::move(loop);
  }

  // A socket bound to a free loopback port; listening if `listen` is set.
  static std::pair<Socket, InetAddress> BoundSocket(bool listen) {
    Result<Socket> socket = Socket::CreateNonBlockingTcp();
    EXPECT_TRUE(socket.has_value());
    EXPECT_TRUE(socket->Bind(InetAddress::Loopback(0)).has_value());
    if (listen) {
      EXPECT_TRUE(socket->Listen(16).has_value());
    }
    const Result<InetAddress> address = socket->LocalAddress();
    EXPECT_TRUE(address.has_value());
    return {*std::move(socket), *address};
  }

  void PollUntil(const std::function<bool()>& done) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      loop_->PollOnce(10);
    }
  }

  std::unique_ptr<EventLoop> loop_;
};

TEST_F(ConnectorTest, ConnectsToListener) {
  auto [listener, address] = BoundSocket(/*listen=*/true);
  std::optional<Socket> connected;
  bool failed = false;
  Result<std::unique_ptr<Connector>> connector = Connector::Start(
      loop_.get(), address,
      [&connected](Socket socket) { connected.emplace(std::move(socket)); },
      [&failed](std::error_code /*error*/) { failed = true; });
  ASSERT_TRUE(connector.has_value()) << connector.error().message();
  PollUntil([&] { return connected.has_value() || failed; });
  EXPECT_TRUE(connected.has_value());
  EXPECT_FALSE(failed);
  // The connector no longer watches the descriptor it handed over.
  EXPECT_EQ(loop_->registered_channel_count(), 0U);
}

TEST_F(ConnectorTest, ReportsRefusedConnection) {
  // Bound but not listening: the kernel refuses connections to it.
  auto [not_listening, address] = BoundSocket(/*listen=*/false);
  std::optional<std::error_code> failure;
  Result<std::unique_ptr<Connector>> connector = Connector::Start(
      loop_.get(), address, [](Socket /*socket*/) {},
      [&failure](std::error_code error) { failure = error; });
  if (!connector) {
    // Some kernels refuse loopback connects synchronously.
    EXPECT_EQ(connector.error(), std::errc::connection_refused);
    return;
  }
  PollUntil([&failure] { return failure.has_value(); });
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(*failure, std::errc::connection_refused);
}

TEST_F(ConnectorTest, CancelSuppressesCallbacks) {
  auto [listener, address] = BoundSocket(/*listen=*/true);
  bool called = false;
  Result<std::unique_ptr<Connector>> connector = Connector::Start(
      loop_.get(), address, [&called](Socket /*socket*/) { called = true; },
      [&called](std::error_code /*error*/) { called = true; });
  ASSERT_TRUE(connector.has_value());
  (*connector)->Cancel();
  for (int i = 0; i < 5; ++i) {
    loop_->PollOnce(10);
  }
  EXPECT_FALSE(called);
}

}  // namespace
}  // namespace sluice::net
