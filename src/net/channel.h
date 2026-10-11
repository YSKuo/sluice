#ifndef SLUICE_NET_CHANNEL_H_
#define SLUICE_NET_CHANNEL_H_

#include <cstdint>
#include <functional>
#include <utility>

#include "result.h"

namespace sluice::net {

// Forward-declared rather than included: event_loop.h includes this header,
// so including it back would be circular.
class EventLoop;

// Binds one file descriptor to the EventLoop: which events we are
// interested in (EPOLLIN/EPOLLOUT), and which callback runs when the loop
// reports them.
//
// A Channel does NOT own its descriptor; the object that owns the
// descriptor (Acceptor, TcpConnection, ...) also owns the Channel and must
// call Remove() before the descriptor is closed. The EventLoop stores the
// Channel's address inside the kernel's epoll registration, so a Channel
// can be neither copied nor moved, and must stay alive until the end of
// the loop iteration in which it was removed (see EventLoop::DeleteLater).
class Channel {
 public:
  using EventCallback = std::function<void()>;

  Channel(EventLoop* loop, int fd) noexcept : loop_(loop), fd_(fd) {}

  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  Channel(Channel&&) = delete;
  Channel& operator=(Channel&&) = delete;

  // Aborts if the channel is still registered: destroying a registered
  // channel would leave a dangling pointer inside epoll.
  ~Channel();

  void SetReadCallback(EventCallback cb) { read_cb_ = std::move(cb); }
  void SetWriteCallback(EventCallback cb) { write_cb_ = std::move(cb); }
  // Peer hung up (EPOLLHUP with no readable data left).
  void SetCloseCallback(EventCallback cb) { close_cb_ = std::move(cb); }
  // EPOLLERR: a socket error is pending (read it with Socket::TakeError).
  void SetErrorCallback(EventCallback cb) { error_cb_ = std::move(cb); }

  // Enabling an event may register the descriptor with epoll for the first
  // time, which can fail at runtime (ENOMEM, or ENOSPC when the per-user
  // epoll watch limit is hit). On failure the caller decides what to do
  // with the descriptor (usually: close it).
  [[nodiscard]] Result<void> EnableReading();
  [[nodiscard]] Result<void> EnableWriting();
  // Disabling never needs a new registration, so the only possible
  // failures are programming errors, which abort.
  void DisableReading();
  void DisableWriting();
  void DisableAll();
  // Unregisters from the loop. Idempotent.
  void Remove();

  [[nodiscard]] bool IsReading() const noexcept;
  [[nodiscard]] bool IsWriting() const noexcept;
  [[nodiscard]] int fd() const noexcept { return fd_; }
  [[nodiscard]] uint32_t events() const noexcept { return events_; }
  // True while the descriptor is registered with epoll, i.e. while the
  // loop may deliver events to this channel.
  [[nodiscard]] bool registered() const noexcept { return registered_; }

  // Called by EventLoop with the events epoll reported for this fd.
  void HandleEvent(uint32_t revents);

 private:
  friend class EventLoop;

  Result<void> Update();
  // Update() for changes that never add a registration; aborts on failure.
  void UpdateExistingRegistration();

  EventLoop* loop_;
  int fd_;
  uint32_t events_ = 0;
  bool registered_ = false;

  EventCallback read_cb_;
  EventCallback write_cb_;
  EventCallback close_cb_;
  EventCallback error_cb_;
};

}  // namespace sluice::net

#endif  // SLUICE_NET_CHANNEL_H_
