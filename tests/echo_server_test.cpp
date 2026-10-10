#include "echo/echo_server.h"

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/unique_fd.h"

namespace sluice::echo {
namespace {

using net::EventLoop;
using net::InetAddress;
using net::UniqueFd;

// A non-blocking test client driven by the test thread. The server runs on
// the same thread, so the client must never block: Pump() sends and
// receives only what the kernel accepts right now, and the test alternates
// it with EventLoop::PollOnce().
class Client {
 public:
  // `recv_buffer_size` > 0 shrinks SO_RCVBUF before connecting, which makes
  // the kernel stop accepting echoed data early (used by backpressure tests).
  explicit Client(const InetAddress& server, int recv_buffer_size = 0)
      : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    EXPECT_TRUE(fd_.valid());
    if (recv_buffer_size > 0) {
      EXPECT_EQ(::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF, &recv_buffer_size,
                             sizeof(recv_buffer_size)),
                0);
    }
    // Connecting while blocking is fine: the kernel completes the handshake
    // from the listen backlog without the server calling accept().
    EXPECT_EQ(::connect(fd_.get(), server.sockaddr_ptr(), InetAddress::length()), 0);
    EXPECT_EQ(::fcntl(fd_.get(), F_SETFL, O_NONBLOCK), 0);
  }

  void QueueSend(const std::string& data) { to_send_ += data; }
  void ShutdownAfterSend() { shutdown_after_send_ = true; }
  void set_reading(bool on) { reading_ = on; }
  void Close() { fd_.Reset(); }

  void Pump() {
    while (sent_ < to_send_.size()) {
      const ssize_t n =
          ::send(fd_.get(), to_send_.data() + sent_, to_send_.size() - sent_, MSG_NOSIGNAL);
      if (n <= 0) {
        break;
      }
      sent_ += static_cast<size_t>(n);
    }
    if (sent_ == to_send_.size() && shutdown_after_send_ && !write_shut_) {
      EXPECT_EQ(::shutdown(fd_.get(), SHUT_WR), 0);
      write_shut_ = true;
    }
    while (reading_ && !eof_) {
      std::array<char, size_t{64} * 1024> buf{};
      const ssize_t n = ::recv(fd_.get(), buf.data(), buf.size(), 0);
      if (n == 0) {
        eof_ = true;
      } else if (n > 0) {
        received_.append(buf.data(), static_cast<size_t>(n));
      } else {
        break;  // EAGAIN, or an error the test will notice as missing data.
      }
    }
  }

  [[nodiscard]] size_t sent() const { return sent_; }
  [[nodiscard]] const std::string& received() const { return received_; }
  [[nodiscard]] bool eof() const { return eof_; }

 private:
  UniqueFd fd_;
  std::string to_send_;
  size_t sent_ = 0;
  std::string received_;
  bool shutdown_after_send_ = false;
  bool write_shut_ = false;
  bool reading_ = true;
  bool eof_ = false;
};

std::string MakePayload(size_t size) {
  std::string data(size, '\0');
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>(i % 251);
  }
  return data;
}

class EchoServerTest : public ::testing::Test {
 protected:
  void StartServer(size_t high_water_mark = EchoServer::kDefaultHighWaterMark) {
    int err = 0;
    loop_ = EventLoop::Create(&err);
    ASSERT_NE(loop_, nullptr);
    // Port 0: the kernel picks a free port, so tests never collide.
    server_ = EchoServer::Create(loop_.get(), InetAddress::Loopback(0), &err, high_water_mark);
    ASSERT_NE(server_, nullptr) << "bind failed: " << err;
    ASSERT_EQ(server_->Start(), 0);
    address_ = server_->LocalAddress(&err);
    ASSERT_EQ(err, 0);
  }

  // Alternates client I/O with loop iterations until `done` holds or a
  // deadline passes. Returns whether `done` became true.
  bool RunUntil(const std::vector<Client*>& clients, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      for (Client* client : clients) {
        client->Pump();
      }
      loop_->PollOnce(1);
      if (done()) {
        return true;
      }
    }
    return false;
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
  Client client(address_);
  client.QueueSend("hello, sluice");

  EXPECT_TRUE(RunUntil({&client}, [&] { return client.received() == "hello, sluice"; }));
}

TEST_F(EchoServerTest, ClientsAreIndependent) {
  StartServer();
  Client a(address_);
  Client b(address_);
  Client c(address_);
  a.QueueSend("alpha");
  b.QueueSend("bravo");
  c.QueueSend("charlie");

  EXPECT_TRUE(RunUntil({&a, &b, &c}, [&] {
    return a.received() == "alpha" && b.received() == "bravo" && c.received() == "charlie";
  }));
  EXPECT_EQ(server_->connection_count(), 3U);
}

TEST_F(EchoServerTest, ConnectionIsDestroyedAfterPeerCloses) {
  StartServer();
  Client client(address_);
  ASSERT_TRUE(RunUntil({&client}, [&] { return server_->connection_count() == 1; }));

  client.Close();
  EXPECT_TRUE(RunUntil({}, [&] { return server_->connection_count() == 0; }));
  // Only the listening socket remains registered with epoll.
  EXPECT_EQ(loop_->registered_channel_count(), 1U);
}

TEST_F(EchoServerTest, FlushesPendingOutputBeforeClosingOnHalfClose) {
  StartServer();
  const std::string payload = MakePayload(size_t{4} * 1024 * 1024);
  Client client(address_);
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
  Client client(address_, /*recv_buffer_size=*/4096);
  client.set_reading(false);
  client.QueueSend(payload);

  // The client keeps sending but never reads, so the echo backs up until
  // the server's output buffer crosses the high-water mark.
  ASSERT_TRUE(RunUntil({&client}, [&] { return server_->pause_count() > 0; }));
  EXPECT_LT(client.sent(), payload.size());

  // Once the client reads again, the server resumes and every byte comes
  // back intact and in order.
  client.set_reading(true);
  ASSERT_TRUE(RunUntil({&client}, [&] { return client.received().size() == payload.size(); }));
  EXPECT_TRUE(client.received() == payload);
}

}  // namespace
}  // namespace sluice::echo
