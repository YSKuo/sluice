#include "net/acceptor.h"

#include <fcntl.h>

#include <cerrno>
#include <cstdio>
#include <system_error>
#include <utility>

namespace sluice::net {

namespace {

UniqueFd OpenIdleFd() {
  return UniqueFd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
}

// Errors after which the listening socket is still healthy and accepting
// should simply continue. ECONNABORTED: the client reset the connection
// while it sat in the backlog. The rest are network errors that Linux
// passes through accept() for the pending connection (see accept(2)).
// Compared as raw errno values because several (EHOSTDOWN, ENONET) have no
// std::errc equivalent.
bool IsTransientAcceptError(const std::error_code& error) {
  switch (error.value()) {
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

bool IsDescriptorExhaustion(const std::error_code& error) {
  return error == std::errc::too_many_files_open ||
         error == std::errc::too_many_files_open_in_system;
}

}  // namespace

Result<std::unique_ptr<Acceptor>> Acceptor::Create(
    EventLoop* loop, const InetAddress& listen_addr) {
  Result<Socket> socket = Socket::CreateNonBlockingTcp();
  if (!socket) {
    return Error(socket.error());
  }
  // Allows an immediate restart while old connections sit in TIME_WAIT.
  if (Result<void> r = socket->SetReuseAddr(true); !r) {
    return Error(r.error());
  }
  if (Result<void> r = socket->Bind(listen_addr); !r) {
    return Error(r.error());
  }
  UniqueFd idle_fd = OpenIdleFd();
  if (!idle_fd.valid()) {
    return LastErrnoError();
  }
  return std::make_unique<Acceptor>(PassKey(), loop, *std::move(socket),
                                    std::move(idle_fd));
}

Acceptor::Acceptor(PassKey /*key*/, EventLoop* loop, Socket socket,
                   UniqueFd idle_fd)
    : socket_(std::move(socket)),
      idle_fd_(std::move(idle_fd)),
      channel_(loop, socket_.fd()) {
  channel_.SetReadCallback([this] { HandleRead(); });
}

Acceptor::~Acceptor() { channel_.Remove(); }

Result<void> Acceptor::Listen() {
  if (Result<void> listening = socket_.Listen(kBacklog); !listening) {
    return listening;
  }
  return channel_.EnableReading();
}

void Acceptor::HandleRead() {
  for (int i = 0; i < kMaxAcceptsPerEvent; ++i) {
    Result<AcceptedSocket> accepted = socket_.Accept();
    if (accepted) {
      if (new_connection_cb_) {
        new_connection_cb_(std::move(accepted->socket), accepted->peer);
      }
      // Without a callback, the socket closes here and the client sees the
      // connection closed immediately.
      continue;
    }
    const std::error_code& error = accepted.error();
    if (IsWouldBlock(error)) {
      return;  // Backlog drained.
    }
    if (IsDescriptorExhaustion(error)) {
      HandleDescriptorExhaustion();
      return;
    }
    if (IsTransientAcceptError(error)) {
      continue;
    }
    // ENOBUFS, ENOMEM, ...: leave the connection in the backlog and retry
    // on the next iteration.
    std::fprintf(stderr, "accept: %s\n", error.message().c_str());
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
  std::fprintf(stderr,
               "accept: out of file descriptors, rejecting a connection\n");
  idle_fd_.Reset();
  if (Result<AcceptedSocket> rejected = socket_.Accept(); !rejected) {
    std::fprintf(stderr, "accept: %s\n", rejected.error().message().c_str());
  }
  // The rejected socket was closed when `rejected` went out of scope above.
  idle_fd_ = OpenIdleFd();
}

}  // namespace sluice::net
