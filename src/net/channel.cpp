#include "net/channel.h"

#include <sys/epoll.h>

#include <cstdio>
#include <cstdlib>

#include "net/event_loop.h"

namespace sluice::net {

namespace {

constexpr uint32_t kReadEvents = EPOLLIN | EPOLLPRI;
constexpr uint32_t kWriteEvents = EPOLLOUT;

}  // namespace

Channel::~Channel() {
  if (registered_) {
    std::fprintf(stderr,
                 "fatal: Channel for fd %d destroyed while still registered\n",
                 fd_);
    std::abort();
  }
}

Result<void> Channel::EnableReading() {
  events_ |= kReadEvents;
  return Update();
}

Result<void> Channel::EnableWriting() {
  events_ |= kWriteEvents;
  return Update();
}

void Channel::DisableReading() {
  events_ &= ~kReadEvents;
  UpdateExistingRegistration();
}

void Channel::DisableWriting() {
  events_ &= ~kWriteEvents;
  UpdateExistingRegistration();
}

void Channel::DisableAll() {
  events_ = 0;
  UpdateExistingRegistration();
}

void Channel::Remove() {
  events_ = 0;
  // Checked here rather than in the loop so that Remove() never touches an
  // EventLoop that is already being destroyed.
  if (registered_) {
    loop_->RemoveChannel(this);
  }
}

bool Channel::IsReading() const noexcept {
  return (events_ & kReadEvents) != 0;
}

bool Channel::IsWriting() const noexcept {
  return (events_ & kWriteEvents) != 0;
}

Result<void> Channel::Update() { return loop_->UpdateChannel(this); }

void Channel::UpdateExistingRegistration() {
  // Shrinking the interest set only modifies or removes a registration,
  // which cannot fail at runtime; a failure here means a bug.
  if (const Result<void> result = Update(); !result) {
    std::fprintf(stderr, "fatal: updating Channel for fd %d: %s\n", fd_,
                 result.error().message().c_str());
    std::abort();
  }
}

void Channel::HandleEvent(uint32_t revents) {
  // Hang-up with nothing left to read: the connection is gone. If EPOLLIN
  // is also set, fall through so the read path drains the remaining data
  // first; it will then see EOF (read returns 0) and close.
  if ((revents & EPOLLHUP) != 0 && (revents & EPOLLIN) == 0) {
    if (close_cb_) {
      close_cb_();
    }
    return;
  }
  if ((revents & EPOLLERR) != 0) {
    if (error_cb_) {
      error_cb_();
    }
    return;
  }
  // Mask with the *current* interest set: an earlier callback in the same
  // loop iteration may have disabled an event that was already reported.
  if ((revents & events_ & kReadEvents) != 0 && read_cb_) {
    read_cb_();
    // The read callback may have closed the connection; deliver nothing
    // more to a channel that is no longer registered.
    if (!registered_) {
      return;
    }
  }
  if ((revents & events_ & kWriteEvents) != 0 && write_cb_) {
    write_cb_();
  }
}

}  // namespace sluice::net
