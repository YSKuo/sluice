#include "net/event_loop.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#include "net/channel.h"

namespace sluice::net {

namespace {

constexpr size_t kInitialEventCapacity = 64;

[[noreturn]] void FatalErrno(const char* what, int err) {
  std::fprintf(stderr, "fatal: %s: %s\n", what, std::strerror(err));
  std::abort();
}

}  // namespace

std::unique_ptr<EventLoop> EventLoop::Create(int* error) {
  UniqueFd epoll_fd(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_fd.valid()) {
    *error = errno;
    return nullptr;
  }
  *error = 0;
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
  // Never block while tasks are waiting to run.
  const int timeout = pending_tasks_.empty() ? timeout_ms : 0;
  int n = ::epoll_wait(epoll_fd_.get(), events_.data(), static_cast<int>(events_.size()), timeout);
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

  RunPendingTasks();
  return n;
}

void EventLoop::QueueInLoop(Task task) { pending_tasks_.push_back(std::move(task)); }

int EventLoop::UpdateChannel(Channel* channel) {
  epoll_event ev{};
  ev.events = channel->events();
  ev.data.ptr = channel;

  if (!channel->registered()) {
    if (channel->events() == 0) {
      return 0;
    }
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, channel->fd(), &ev) != 0) {
      const int err = errno;
      if (err == ENOMEM || err == ENOSPC) {
        return err;
      }
      FatalErrno("epoll_ctl(ADD)", err);
    }
    channel->registered_ = true;
    ++registered_count_;
    return 0;
  }

  // Unregister instead of keeping an empty interest set: epoll always
  // reports EPOLLHUP/EPOLLERR, which would otherwise keep waking the loop
  // for a channel that asked for nothing.
  if (channel->events() == 0) {
    RemoveChannel(channel);
    return 0;
  }
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, channel->fd(), &ev) != 0) {
    FatalErrno("epoll_ctl(MOD)", errno);
  }
  return 0;
}

void EventLoop::RemoveChannel(Channel* channel) {
  if (!channel->registered()) {
    return;
  }
  if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, channel->fd(), nullptr) != 0) {
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
