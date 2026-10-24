#include "net/timer.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "net/event_loop.h"
#include "result.h"

namespace sluice::net {
namespace {

using std::chrono::milliseconds;
using Clock = EventLoop::Clock;

class TimerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    Result<std::unique_ptr<EventLoop>> loop = EventLoop::Create();
    ASSERT_TRUE(loop.has_value()) << loop.error().message();
    loop_ = *std::move(loop);
  }

  // Polls until `done` or a deadline; returns whether `done` became true.
  bool PollUntil(const std::function<bool()>& done) {
    const auto deadline = Clock::now() + std::chrono::seconds(5);
    while (Clock::now() < deadline) {
      loop_->PollOnce(-1);
      if (done()) {
        return true;
      }
    }
    return false;
  }

  std::unique_ptr<EventLoop> loop_;
};

TEST_F(TimerTest, FiresAfterDelay) {
  bool fired = false;
  const Clock::time_point start = Clock::now();
  loop_->RunAfter(milliseconds(30), [&fired] { fired = true; });
  // PollOnce(-1) must wake up for the timer even with no descriptors.
  ASSERT_TRUE(PollUntil([&fired] { return fired; }));
  EXPECT_GE(Clock::now() - start, milliseconds(30));
  EXPECT_EQ(loop_->timer_count(), 0U);
}

TEST_F(TimerTest, FiresInDeadlineOrder) {
  std::vector<std::string> order;
  loop_->RunAfter(milliseconds(40), [&order] { order.emplace_back("late"); });
  loop_->RunAfter(milliseconds(10), [&order] { order.emplace_back("early"); });
  ASSERT_TRUE(PollUntil([&order] { return order.size() == 2; }));
  EXPECT_EQ(order, (std::vector<std::string>{"early", "late"}));
}

TEST_F(TimerTest, CancelledTimerDoesNotFire) {
  bool fired = false;
  const EventLoop::TimerId id =
      loop_->RunAfter(milliseconds(10), [&fired] { fired = true; });
  loop_->CancelTimer(id);
  loop_->CancelTimer(id);  // Cancelling twice is harmless.
  bool sentinel = false;
  loop_->RunAfter(milliseconds(30), [&sentinel] { sentinel = true; });
  ASSERT_TRUE(PollUntil([&sentinel] { return sentinel; }));
  EXPECT_FALSE(fired);
}

TEST_F(TimerTest, TimerCanCancelAnotherExpiredTimer) {
  bool second_fired = false;
  EventLoop::TimerId second = 0;
  loop_->RunAfter(milliseconds(5),
                  [this, &second] { loop_->CancelTimer(second); });
  second = loop_->RunAfter(milliseconds(5),
                           [&second_fired] { second_fired = true; });
  bool sentinel = false;
  loop_->RunAfter(milliseconds(30), [&sentinel] { sentinel = true; });
  ASSERT_TRUE(PollUntil([&sentinel] { return sentinel; }));
  EXPECT_FALSE(second_fired);
}

TEST_F(TimerTest, RaiiTimerCancelsOnDestruction) {
  bool fired = false;
  {
    Timer timer(loop_.get());
    timer.Arm(milliseconds(10), [&fired] { fired = true; });
    EXPECT_TRUE(timer.armed());
  }
  EXPECT_EQ(loop_->timer_count(), 0U);
  bool sentinel = false;
  loop_->RunAfter(milliseconds(30), [&sentinel] { sentinel = true; });
  ASSERT_TRUE(PollUntil([&sentinel] { return sentinel; }));
  EXPECT_FALSE(fired);
}

TEST_F(TimerTest, RearmReplacesDeadline) {
  Timer timer(loop_.get());
  int fired = 0;
  timer.Arm(milliseconds(10), [&fired] { fired += 1; });
  timer.Arm(milliseconds(40), [&fired] { fired += 10; });
  EXPECT_EQ(loop_->timer_count(), 1U);
  ASSERT_TRUE(PollUntil([&fired] { return fired != 0; }));
  EXPECT_EQ(fired, 10);
  EXPECT_FALSE(timer.armed());
}

TEST_F(TimerTest, CallbackMayRearm) {
  Timer timer(loop_.get());
  int fired = 0;
  std::function<void()> tick = [&] {
    if (++fired < 3) {
      timer.Arm(milliseconds(5), tick);
    }
  };
  timer.Arm(milliseconds(5), tick);
  ASSERT_TRUE(PollUntil([&fired] { return fired == 3; }));
  EXPECT_FALSE(timer.armed());
}

}  // namespace
}  // namespace sluice::net
