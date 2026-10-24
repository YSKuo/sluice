#ifndef SLUICE_NET_TIMER_H_
#define SLUICE_NET_TIMER_H_

#include <functional>
#include <optional>
#include <utility>

#include "net/event_loop.h"

namespace sluice::net {

// A one-shot timer on an EventLoop that cancels itself when destroyed, so
// its callback can safely capture the object that owns the Timer.
//
// Arm() replaces any pending deadline, which is how idle timeouts are
// reset on activity. Like everything on the loop, not thread-safe, and the
// EventLoop must outlive the Timer.
class Timer {
 public:
  explicit Timer(EventLoop* loop) noexcept : loop_(loop) {}

  // The pending callback captures `this`.
  Timer(const Timer&) = delete;
  Timer& operator=(const Timer&) = delete;
  Timer(Timer&&) = delete;
  Timer& operator=(Timer&&) = delete;

  ~Timer() { Disarm(); }

  // Runs `callback` once after `delay`, unless disarmed or re-armed first.
  void Arm(EventLoop::Clock::duration delay, std::function<void()> callback) {
    Disarm();
    id_ = loop_->RunAfter(delay, [this, cb = std::move(callback)] {
      // The loop has already dropped this timer; forget its id before
      // running the callback, which may re-arm.
      id_.reset();
      cb();
    });
  }

  void Disarm() {
    if (id_) {
      loop_->CancelTimer(*id_);
      id_.reset();
    }
  }

  [[nodiscard]] bool armed() const noexcept { return id_.has_value(); }

 private:
  EventLoop* loop_;
  std::optional<EventLoop::TimerId> id_;
};

}  // namespace sluice::net

#endif  // SLUICE_NET_TIMER_H_
