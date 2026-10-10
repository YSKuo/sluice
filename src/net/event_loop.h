#pragma once

#include <sys/epoll.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include "net/unique_fd.h"

namespace sluice::net {

class Channel;

// Single-threaded epoll reactor. Waits for readiness events on registered
// Channels and dispatches them to the Channels' callbacks, then runs any
// tasks queued during that dispatch.
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

  // Restricts construction to Create() while still allowing
  // std::make_unique (which needs a public constructor).
  class PassKey {
    friend class EventLoop;
    PassKey() = default;
  };

  // Returns nullptr and sets `*error` to errno if the epoll instance cannot
  // be created.
  static std::unique_ptr<EventLoop> Create(int* error);
  EventLoop(PassKey key, UniqueFd epoll_fd);

  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;
  EventLoop(EventLoop&&) = delete;
  EventLoop& operator=(EventLoop&&) = delete;
  ~EventLoop() = default;

  // Runs PollOnce() until Quit() is called.
  void Loop();
  // Waits up to `timeout_ms` (-1 = forever) for events, dispatches them,
  // then runs queued tasks. Returns the number of events dispatched.
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

  // Number of Channels currently registered with epoll.
  [[nodiscard]] size_t registered_channel_count() const noexcept { return registered_count_; }

 private:
  friend class Channel;

  // Synchronizes the epoll registration with the Channel's interest set.
  // Returns 0, or errno if registering a new descriptor failed (ENOMEM,
  // ENOSPC). Any other failure is a programming error and aborts.
  int UpdateChannel(Channel* channel);
  void RemoveChannel(Channel* channel);
  void RunPendingTasks();

  UniqueFd epoll_fd_;
  std::vector<epoll_event> events_;
  std::vector<Task> pending_tasks_;
  size_t registered_count_ = 0;
  bool quit_ = false;
};

}  // namespace sluice::net
