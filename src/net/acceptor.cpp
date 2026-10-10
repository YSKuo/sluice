#include "net/acceptor.h"

#include <fcntl.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <utility>

#include "net/event_loop.h"

namespace sluice::net {

namespace {

int OpenIdleFd() { return ::open("/dev/null", O_RDONLY | O_CLOEXEC); }

// Errors after which the listening socket is still healthy and accepting
// should simply continue. ECONNABORTED: the client reset the connection
// while it sat in the backlog. The rest are network errors that Linux
// passes through accept() for the pending connection (see accept(2)).
bool IsTransientAcceptError(int err) {
  switch (err) {
    case ECONNABORTED:
    case EPROTO:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case ENETDOWN:
    case ENETUNREACH:
      return true;
    default:
      return false;
  }
}

}  // namespace

std::unique_ptr<Acceptor> Acceptor::Create(EventLoop* loop, const InetAddress& listen_addr,
                                           int* error) {
  Socket socket = Socket::CreateNonBlockingTcp(error);
  if (!socket.valid()) {
    return nullptr;
  }
  // Allows an immediate restart while old connections sit in TIME_WAIT.
  *error = socket.SetReuseAddr(true);
  if (*error != 0) {
    return nullptr;
  }
  *error = socket.Bind(listen_addr);
  if (*error != 0) {
    return nullptr;
  }
  UniqueFd idle_fd(OpenIdleFd());
  if (!idle_fd.valid()) {
    *error = errno;
    return nullptr;
  }
  *error = 0;
  return std::make_unique<Acceptor>(PassKey(), loop, std::move(socket), std::move(idle_fd));
}

Acceptor::Acceptor(PassKey /*key*/, EventLoop* loop, Socket socket, UniqueFd idle_fd)
    : socket_(std::move(socket)), idle_fd_(std::move(idle_fd)), channel_(loop, socket_.fd()) {
  channel_.SetReadCallback([this] { HandleRead(); });
}

Acceptor::~Acceptor() { channel_.Remove(); }

int Acceptor::Listen() {
  if (const int err = socket_.Listen(kBacklog); err != 0) {
    return err;
  }
  return channel_.EnableReading();
}

void Acceptor::HandleRead() {
  for (int i = 0; i < kMaxAcceptsPerEvent; ++i) {
    AcceptResult result = socket_.Accept();
    if (result.error == 0) {
      if (new_connection_cb_) {
        new_connection_cb_(Socket(std::move(result.fd)), result.peer);
      }
      // Without a callback, result.fd closes here and the client sees the
      // connection closed immediately.
      continue;
    }
    if (result.error == EAGAIN || result.error == EWOULDBLOCK) {
      return;  // Backlog drained.
    }
    if (result.error == EMFILE || result.error == ENFILE) {
      HandleDescriptorExhaustion();
      return;
    }
    if (IsTransientAcceptError(result.error)) {
      continue;
    }
    // ENOBUFS, ENOMEM, ...: leave the connection in the backlog and retry
    // on the next iteration.
    std::fprintf(stderr, "accept: %s\n", std::strerror(result.error));
    return;
  }
}

void Acceptor::HandleDescriptorExhaustion() {
  // Out of descriptors, so accept() cannot take the pending connection off
  // the backlog. With level-triggered epoll the listening socket would then
  // stay readable forever and spin the loop at 100% CPU. Instead, free the
  // reserved descriptor, accept the connection, close it at once (the
  // client sees the connection closed rather than hanging), and reserve a
  // descriptor again.
  std::fprintf(stderr, "accept: out of file descriptors, rejecting a connection\n");
  idle_fd_.Reset();
  // The returned AcceptResult is a temporary, so its descriptor is closed
  // at the end of this statement.
  static_cast<void>(socket_.Accept());
  idle_fd_.Reset(OpenIdleFd());
}

}  // namespace sluice::net
