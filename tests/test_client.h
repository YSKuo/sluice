#ifndef SLUICE_TESTS_TEST_CLIENT_H_
#define SLUICE_TESTS_TEST_CLIENT_H_

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/unique_fd.h"

namespace sluice::test {

// A non-blocking TCP client driven by the test thread. The server under
// test runs on the same thread, so the client must never block: Pump()
// sends and receives only what the kernel accepts right now, and the test
// alternates it with EventLoop::PollOnce() (see RunUntil).
class TestClient {
 public:
  // `recv_buffer_size` > 0 shrinks SO_RCVBUF before connecting, which makes
  // the kernel stop accepting the server's output early (used by
  // backpressure tests).
  explicit TestClient(const net::InetAddress& server, int recv_buffer_size = 0)
      : fd_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)) {
    EXPECT_TRUE(fd_.valid());
    if (recv_buffer_size > 0) {
      EXPECT_EQ(::setsockopt(fd_.get(), SOL_SOCKET, SO_RCVBUF,
                             &recv_buffer_size, sizeof(recv_buffer_size)),
                0);
    }
    // Connecting while blocking is fine: the kernel completes the handshake
    // from the listen backlog without the server calling accept().
    EXPECT_EQ(
        ::connect(fd_.get(), server.sockaddr_ptr(), net::InetAddress::length()),
        0);
    EXPECT_EQ(::fcntl(fd_.get(), F_SETFL, O_NONBLOCK), 0);
  }

  void QueueSend(const std::string& data) { to_send_ += data; }
  void ShutdownAfterSend() { shutdown_after_send_ = true; }
  void set_reading(bool on) { reading_ = on; }
  void Close() { fd_.Reset(); }

  void Pump() {
    while (sent_ < to_send_.size()) {
      const ssize_t n = ::send(fd_.get(), to_send_.data() + sent_,
                               to_send_.size() - sent_, MSG_NOSIGNAL);
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
  net::UniqueFd fd_;
  std::string to_send_;
  size_t sent_ = 0;
  std::string received_;
  bool shutdown_after_send_ = false;
  bool write_shut_ = false;
  bool reading_ = true;
  bool eof_ = false;
};

// Alternates client I/O with loop iterations until `done` holds or ten
// seconds pass. Returns whether `done` became true.
inline bool RunUntil(net::EventLoop& loop,
                     const std::vector<TestClient*>& clients,
                     const std::function<bool()>& done) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    for (TestClient* client : clients) {
      client->Pump();
    }
    loop.PollOnce(1);
    if (done()) {
      return true;
    }
  }
  return false;
}

// `size` bytes of a repeating, non-text pattern, so that misordered or
// dropped bytes are detected.
inline std::string MakePayload(size_t size) {
  std::string data(size, '\0');
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<char>(i % 251);
  }
  return data;
}

}  // namespace sluice::test

#endif  // SLUICE_TESTS_TEST_CLIENT_H_
