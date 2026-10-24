#include "net/event_loop.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace sluice::net {

namespace {

constexpr size_t kInitialEventCapacity = 64;
// Upper bound for one epoll_wait() when a timer is far away; keeps the
// millisecond count well inside int.
constexpr int64_t kMaxWaitMs = int64_t{24} * 3600 * 1000;

[[noreturn]] void FatalErrno(const char* what, int err) {
  std::fprintf(stderr, "fatal: %s: %s\n", what, std::strerror(err));
  std::abort();
}

}  // namespace

Result<std::unique_ptr<EventLoop>> EventLoop::Create() {
  UniqueFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd.valid()) {
    return LastErrnoError();
  }
  return std::make_unique<EventLoop>(PassKey(), std::move(epoll_fd));
}

EventLoop::EventLoop(PassKey /*key*/, UniqueFd epoll_fd)
    : epoll_fd_(std::move(epoll_fd)), events_(kInitialEventCapacity) {}

void EventLoop::Loop() {
  while (!quit_) {
    PollOnce(-1);
  }
  // Allow the loop to be run again after Quit().
  quit_ = false;
}

int EventLoop::PollOnce(int timeout_ms) {
  const int timeout = EffectiveTimeout(timeout_ms);
  int n = ::epoll_wait(epoll_fd_.get(), events_.data(),
                       static_cast<int>(events_.size()), timeout);
  if (n < 0) {
    if (errno != EINTR) {
      // EBADF, EFAULT, EINVAL: all indicate a bug in this class.
      FatalErrno("epoll_wait", errno);
    }
    n = 0;
  }

  const auto count = static_cast<size_t>(n);
  for (size_t i = 0; i < count; ++i) {
    auto* channel = static_cast<Channel*>(events_[i].data.ptr);
    // A callback earlier in this batch may have removed this channel. Its
    // memory is still valid (owners defer destruction via DeleteLater), but
    // it must not receive any more events.
    if (!channel->registered()) {
      continue;
    }
    channel->HandleEvent(events_[i].events);
  }

  // A full batch suggests more events are ready than we can receive at
  // once; grow so the next epoll_wait() can return them all.
  if (count == events_.size()) {
    events_.resize(events_.size() * 2);
  }

  // Timers run after events so that a timeout never preempts data that
  // arrived in the same iteration; tasks run last so that anything a timer
  // queued for DeleteLater is destroyed in this same iteration.
  RunExpiredTimers();
  RunPendingTasks();
  return n;
}

int EventLoop::EffectiveTimeout(int timeout_ms) const {
  // Never block while tasks are waiting to run.
  if (!pending_tasks_.empty()) {
    return 0;
  }
  if (timers_.empty()) {
    return timeout_ms;
  }
  const Clock::duration until_next =
      timers_.begin()->first.first - Clock::now();
  // Round up: waking a millisecond early would just spin once more.
  const auto ms = std::chrono::ceil<std::chrono::milliseconds>(until_next);
  const int timer_ms =
      static_cast<int>(std::clamp<int64_t>(ms.count(), 0, kMaxWaitMs));
  return timeout_ms < 0 ? timer_ms : std::min(timeout_ms, timer_ms);
}

EventLoop::TimerId EventLoop::RunAfter(Clock::duration delay, Task task) {
  const TimerId id = next_timer_id_++;
  const Clock::time_point deadline = Clock::now() + delay;
  timers_.emplace(std::pair(deadline, id), std::move(task));
  timer_deadlines_.emplace(id, deadline);
  return id;
}

void EventLoop::CancelTimer(TimerId id) {
  const auto it = timer_deadlines_.find(id);
  if (it == timer_deadlines_.end()) {
    return;
  }
  timers_.erase(std::pair(it->second, id));
  timer_deadlines_.erase(it);
}

void EventLoop::RunExpiredTimers() {
  const Clock::time_point now = Clock::now();
  // One timer at a time: a timer's task may cancel another timer that has
  // also expired, which must then not run.
  while (!timers_.empty() && timers_.begin()->first.first <= now) {
    auto node = timers_.extract(timers_.begin());
    timer_deadlines_.erase(node.key().second);
    node.mapped()();
  }
}

void EventLoop::QueueInLoop(Task task) {
  pending_tasks_.push_back(std::move(task));
}

Result<void> EventLoop::UpdateChannel(Channel* channel) {
  epoll_event ev{};
  ev.events = channel->events();
  ev.data.ptr = channel;

  if (!channel->registered()) {
    if (channel->events() == 0) {
      return {};
    }
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, channel->fd(), &ev) != 0) {
      const int err = errno;
      if (err == ENOMEM || err == ENOSPC) {
        return ErrnoError(err);
      }
      FatalErrno("epoll_ctl(ADD)", err);
    }
    channel->registered_ = true;
    ++registered_count_;
    return {};
  }

  // Unregister instead of keeping an empty interest set: epoll always
  // reports EPOLLHUP/EPOLLERR, which would otherwise keep waking the loop
  // for a channel that asked for nothing.
  if (channel->events() == 0) {
    RemoveChannel(channel);
    return {};
  }
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, channel->fd(), &ev) != 0) {
    FatalErrno("epoll_ctl(MOD)", errno);
  }
  return {};
}

void EventLoop::RemoveChannel(Channel* channel) {
  if (!channel->registered()) {
    return;
  }
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, channel->fd(), nullptr) !=
      0) {
    FatalErrno("epoll_ctl(DEL)", errno);
  }
  channel->registered_ = false;
  --registered_count_;
}

void EventLoop::RunPendingTasks() {
  // Swap first: tasks may queue more tasks, which then run on the next
  // iteration instead of extending this one indefinitely.
  std::vector<Task> tasks;
  tasks.swap(pending_tasks_);
  for (Task& task : tasks) {
    task();
  }
}

}  // namespace sluice::net
