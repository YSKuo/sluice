# ADR 0001: Event loop and connection lifecycle

## Status

Accepted

## Context

Week 1 needs a single-threaded reactor that every later feature (HTTP
parsing, proxying, timeouts, rate limiting) builds on. Its decisions are hard
to change later, and its bugs (use-after-free on close, busy loops, lost
events) are the subtle kind. Constraints from the project rules: Linux only,
RAII for every resource, `unique_ptr` for ownership, no exceptions on the
hot path, and every file must be explainable.

## Options

Considered for each sub-decision:

- **Trigger mode:** level-triggered (LT) vs edge-triggered (ET) epoll.
- **Dispatch structure:** a `Channel` object per descriptor holding its
  interest set and callbacks (the muduo design) vs a single `switch` over
  descriptor types inside the loop.
- **Connection ownership:** `shared_ptr` connections kept alive by callbacks
  vs a single `unique_ptr` owner (the server) plus deferred destruction.
- **Signal handling:** `signal()`/`sigaction()` handlers setting a flag vs
  `signalfd` registered on the loop.

## Decision

- **Level-triggered epoll.** A callback may do part of the work (one
  `read()` per event, at most 64 `accept()`s per event) and the loop reports
  the descriptor again next iteration. ET requires draining every descriptor
  to `EAGAIN` on every event or losing it forever, which is a correctness
  trap with no measurable benefit at this scale.
- **Channel per descriptor.** `Channel` stores the interest set and
  callbacks; owners (`Acceptor`, `TcpConnection`, the signalfd in `main`)
  own both the descriptor and its `Channel`. The kernel's epoll entry stores
  the `Channel*` directly (`epoll_event.data.ptr`), so dispatch needs no
  lookup table.
- **An empty interest set unregisters.** When a channel disables every
  event it is removed from epoll (`EPOLL_CTL_DEL`), because epoll always
  reports `EPOLLHUP`/`EPOLLERR` and would otherwise keep waking the loop.
- **`unique_ptr` ownership with deferred destruction.** `TcpServer` owns
  each `TcpConnection` through a `unique_ptr`. A connection almost always
  closes from inside one of its own callbacks, so the server moves it out of
  the table and hands it to `EventLoop::DeleteLater`, which destroys it after
  the current batch of events has been dispatched. Combined with the loop
  skipping channels that were unregistered earlier in the same batch, no
  pointer in the batch can dangle.
- **`signalfd` for SIGINT/SIGTERM.** Signals become ordinary readable
  events: no async-signal-safety rules, no `EINTR` race with `epoll_wait`.
- **Error reporting by return value.** Syscall wrappers return `0` or the
  `errno` value; factories return `nullptr` plus an `int* error`. Failures
  that can only mean a bug (`EBADF` from `epoll_ctl`, destroying a still-
  registered `Channel`) abort immediately with a message.
- **Backpressure in the echo server.** When a connection's queued output
  exceeds a high-water mark (1 MiB by default), reading from it pauses until
  the output is fully flushed, bounding per-connection memory.

## Trade-offs

- LT costs one extra `epoll_wait` wake-up when a callback leaves work
  behind; accepted for simpler, harder-to-break code.
- Deferred destruction means a closed connection's memory and descriptor are
  released one iteration later, and `DeleteLater` uses a `shared_ptr`
  internally purely to type-erase into `std::function`.
- `epoll_event.data.ptr` ties `Channel` addresses to the kernel
  registration: `Channel` is non-copyable and non-movable, and must outlive
  its registration (enforced by the abort in its destructor).
- The loop is strictly single-threaded; `Quit()` and `QueueInLoop()` are not
  safe to call from another thread. Multi-worker support (out of scope)
  would need an `eventfd` wake-up and a mutex-protected task queue.
- IPv4 only; IPv6 would need `InetAddress` to wrap `sockaddr_storage`.

## References

- `src/net/event_loop.{h,cpp}`, `src/net/channel.{h,cpp}`,
  `src/net/tcp_connection.{h,cpp}`, `src/net/tcp_server.{h,cpp}`
- `epoll(7)`, `accept(2)` (network errors passed through accept), `signalfd(2)`
- Chen Shuo, *Linux Multithreaded Server Programming* (muduo), for the
  Channel/EventLoop split
