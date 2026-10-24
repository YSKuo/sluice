#ifndef SLUICE_NET_EVENT_LOOP_H_
#define SLUICE_NET_EVENT_LOOP_H_

#include <sys/epoll.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "net/channel.h"
#include "net/unique_fd.h"
#include "result.h"

namespace sluice::net {

// Single-threaded epoll reactor. Waits for readiness events on registered
// Channels and dispatches them to the Channels' callbacks, then runs
// expired timers, then any tasks queued during that iteration.
//
// Timers use steady_clock (never system_clock, which can jump). The next
// timer's deadline bounds how long epoll_wait() may sleep, so timers need
// no descriptor of their own.
//
// Uses level-triggered epoll: a descriptor that is still readable or
// writable is reported again on the next iteration, so a callback may
// handle only part of the available work without losing events.
//
// Not thread-safe: every method must be called from the thread running the
// loop. The EventLoop must outlive every Channel registered with it.
class EventLoop {
 public:
  using Task = std::function<void()>;
  using Clock = std::chrono::steady_clock;
  using TimerId = uint64_t;

  // Restricts construction to Create() while still allowing
  // std::make_unique (which needs a public constructor).
  class PassKey {
    friend class EventLoop;
    PassKey() = default;
  };

  // Fails if the epoll instance cannot be created.
  static Result<std::unique_ptr<EventLoop>> Create();

  EventLoop(PassKey key, UniqueFd epoll_fd);

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop(EventLoop&&) = delete;
  EventLoop& operator=(EventLoop&&) = delete;

  ~EventLoop() = default;

  // Runs PollOnce() until Quit() is called.
  void Loop();
  // Waits up to `timeout_ms` (-1 = forever), or until the next timer is
  // due if that is sooner, for events; dispatches them, runs expired
  // timers, then queued tasks. Returns the number of events dispatched.
  // Tests use this to step the loop deterministically.
  int PollOnce(int timeout_ms);
  // Makes Loop() return after the current iteration. Typically called from
  // inside a callback (e.g. on SIGTERM delivered through a signalfd).
  void Quit() noexcept { quit_ = true; }

  // Runs `task` after every event of the current iteration has been
  // dispatched. If called outside a dispatch, it runs on the next
  // PollOnce(), which then does not block.
  void QueueInLoop(Task task);

  // Destroys `object` after the current iteration's dispatch. Objects that
  // own a Channel must be destroyed this way when they are torn down from
  // inside one of their own callbacks: the callback is still on the stack,
  // and epoll may have already reported another event for the same
  // Channel later in the current batch.
  template <typename T>
  void DeleteLater(std::unique_ptr<T> object) {
    // std::function requires a copyable callable, so the unique_ptr is
    // moved into a shared_ptr purely for type erasure. The task body is
    // empty: the object is destroyed together with the task, after it runs
    // (or when the loop itself is destroyed).
    QueueInLoop([holder = std::shared_ptr<T>(std::move(object))] {});
  }

  // Runs `task` once, `delay` from now, after that iteration's events.
  // Prefer the RAII net::Timer, which cancels itself when destroyed.
  TimerId RunAfter(Clock::duration delay, Task task);
  // Cancels a pending timer. Cancelling one that already ran is a no-op.
  void CancelTimer(TimerId id);
  [[nodiscard]] size_t timer_count() const noexcept { return timers_.size(); }

  // Number of Channels currently registered with epoll.
  [[nodiscard]] size_t registered_channel_count() const noexcept {
    return registered_count_;
  }

 private:
  friend class Channel;

  // Synchronizes the epoll registration with the Channel's interest set.
  // Fails only if registering a new descriptor failed (ENOMEM, ENOSPC). Any
  // other failure is a programming error and aborts.
  Result<void> UpdateChannel(Channel* channel);
  void RemoveChannel(Channel* channel);
  void RunPendingTasks();
  void RunExpiredTimers();
  // How long epoll_wait() may block: the caller's timeout, shortened so
  // that the next timer is not run late.
  int EffectiveTimeout(int timeout_ms) const;

  UniqueFd epoll_fd_;
  std::vector<epoll_event> events_;
  std::vector<Task> pending_tasks_;
  size_t registered_count_ = 0;
  bool quit_ = false;

  // Pending timers ordered by deadline (ties broken by id, i.e. creation
  // order), plus an index from id to deadline so cancelling is O(log n).
  std::map<std::pair<Clock::time_point, TimerId>, Task> timers_;
  std::unordered_map<TimerId, Clock::time_point> timer_deadlines_;
  TimerId next_timer_id_ = 1;
};

}  // namespace sluice::net

#endif  // SLUICE_NET_EVENT_LOOP_H_
