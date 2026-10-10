#include "net/event_loop.h"

#include <gtest/gtest.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/channel.h"
#include "net/unique_fd.h"

namespace sluice::net {
namespace {

// An eventfd is the simplest pollable descriptor: readable while its
// counter is non-zero, and always writable.
UniqueFd MakeEventFd() { return UniqueFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)); }

void Signal(const UniqueFd& fd) {
  const uint64_t one = 1;
  ASSERT_EQ(::write(fd.get(), &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
}

void Drain(const UniqueFd& fd) {
  uint64_t value = 0;
  static_cast<void>(::read(fd.get(), &value, sizeof(value)));
}

class EventLoopTest : public ::testing::Test {
 protected:
  void SetUp() override {
    int err = 0;
    loop_ = EventLoop::Create(&err);
    ASSERT_NE(loop_, nullptr) << "epoll_create1 failed: " << err;
  }

  std::unique_ptr<EventLoop> loop_;
};

TEST_F(EventLoopTest, DispatchesReadEvent) {
  const UniqueFd efd = MakeEventFd();
  Channel channel(loop_.get(), efd.get());
  int calls = 0;
  channel.SetReadCallback([&] {
    ++calls;
    Drain(efd);
  });
  ASSERT_EQ(channel.EnableReading(), 0);

  EXPECT_EQ(loop_->PollOnce(0), 0);  // Not readable yet.
  Signal(efd);
  EXPECT_EQ(loop_->PollOnce(0), 1);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(loop_->PollOnce(0), 0);  // Drained: no further events.

  channel.Remove();
}

TEST_F(EventLoopTest, LevelTriggeredRepeatsUntilDrained) {
  const UniqueFd efd = MakeEventFd();
  Channel channel(loop_.get(), efd.get());
  int calls = 0;
  channel.SetReadCallback([&] { ++calls; });  // Deliberately does not drain.
  ASSERT_EQ(channel.EnableReading(), 0);

  Signal(efd);
  loop_->PollOnce(0);
  loop_->PollOnce(0);
  loop_->PollOnce(0);
  EXPECT_EQ(calls, 3);

  channel.Remove();
}

TEST_F(EventLoopTest, DispatchesWriteEvent) {
  const UniqueFd efd = MakeEventFd();
  Channel channel(loop_.get(), efd.get());
  int calls = 0;
  channel.SetWriteCallback([&] { ++calls; });
  ASSERT_EQ(channel.EnableWriting(), 0);

  loop_->PollOnce(0);
  EXPECT_EQ(calls, 1);

  channel.DisableWriting();
  loop_->PollOnce(0);
  EXPECT_EQ(calls, 1);
}

TEST_F(EventLoopTest, EmptyInterestSetUnregisters) {
  const UniqueFd efd = MakeEventFd();
  Channel channel(loop_.get(), efd.get());
  ASSERT_EQ(channel.EnableReading(), 0);
  EXPECT_TRUE(channel.registered());
  EXPECT_EQ(loop_->registered_channel_count(), 1U);

  channel.DisableAll();
  EXPECT_FALSE(channel.registered());
  EXPECT_EQ(loop_->registered_channel_count(), 0U);

  // Re-enabling registers again.
  ASSERT_EQ(channel.EnableReading(), 0);
  EXPECT_TRUE(channel.registered());
  channel.Remove();
  channel.Remove();  // Idempotent.
  EXPECT_EQ(loop_->registered_channel_count(), 0U);
}

TEST_F(EventLoopTest, ChannelRemovedEarlierInBatchIsSkipped) {
  // Both descriptors are ready in the same epoll_wait() batch. Whichever
  // callback runs first removes the other channel, which must then not be
  // dispatched even though its event is already in the batch.
  const UniqueFd a_fd = MakeEventFd();
  const UniqueFd b_fd = MakeEventFd();
  Channel a(loop_.get(), a_fd.get());
  Channel b(loop_.get(), b_fd.get());
  int calls = 0;
  a.SetReadCallback([&] {
    ++calls;
    b.Remove();
  });
  b.SetReadCallback([&] {
    ++calls;
    a.Remove();
  });
  ASSERT_EQ(a.EnableReading(), 0);
  ASSERT_EQ(b.EnableReading(), 0);
  Signal(a_fd);
  Signal(b_fd);

  loop_->PollOnce(0);
  EXPECT_EQ(calls, 1);

  a.Remove();
  b.Remove();
}

TEST_F(EventLoopTest, QueuedTasksRunAfterDispatch) {
  const UniqueFd efd = MakeEventFd();
  Channel channel(loop_.get(), efd.get());
  std::vector<std::string> order;
  channel.SetReadCallback([&] {
    loop_->QueueInLoop([&] { order.emplace_back("task"); });
    order.emplace_back("callback");
    Drain(efd);
  });
  ASSERT_EQ(channel.EnableReading(), 0);
  Signal(efd);

  loop_->PollOnce(0);
  EXPECT_EQ(order, (std::vector<std::string>{"callback", "task"}));

  channel.Remove();
}

TEST_F(EventLoopTest, PendingTaskPreventsBlocking) {
  bool ran = false;
  loop_->QueueInLoop([&] { ran = true; });
  // Would block forever if pending tasks did not force a zero timeout.
  loop_->PollOnce(-1);
  EXPECT_TRUE(ran);
}

TEST_F(EventLoopTest, QuitStopsLoop) {
  const UniqueFd efd = MakeEventFd();
  Channel channel(loop_.get(), efd.get());
  channel.SetReadCallback([&] {
    Drain(efd);
    loop_->Quit();
  });
  ASSERT_EQ(channel.EnableReading(), 0);
  Signal(efd);

  loop_->Loop();  // Returns only because the callback called Quit().

  channel.Remove();
}

TEST_F(EventLoopTest, DeleteLaterDestroysAfterDispatch) {
  struct Tracker {
    explicit Tracker(bool* destroyed) : destroyed_(destroyed) {}
    Tracker(const Tracker&) = delete;
    Tracker& operator=(const Tracker&) = delete;
    Tracker(Tracker&&) = delete;
    Tracker& operator=(Tracker&&) = delete;
    ~Tracker() { *destroyed_ = true; }
    bool* destroyed_;
  };

  bool destroyed = false;
  loop_->DeleteLater(std::make_unique<Tracker>(&destroyed));
  EXPECT_FALSE(destroyed);
  loop_->PollOnce(0);
  EXPECT_TRUE(destroyed);
}

}  // namespace
}  // namespace sluice::net
