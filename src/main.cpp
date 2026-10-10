#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>

#include "echo/echo_server.h"
#include "net/channel.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/unique_fd.h"
#include "version.h"

namespace {

struct Options {
  std::string_view host = "0.0.0.0";
  uint16_t port = 8080;
  bool show_version = false;
  bool show_help = false;
};

void PrintUsage() {
  std::printf(
      "usage: sluice [--host IPV4] [--port PORT] [--version] [--help]\n"
      "\n"
      "Week 1 milestone: runs a TCP echo server on HOST:PORT (default\n"
      "0.0.0.0:8080). Stop it with Ctrl-C or SIGTERM.\n");
}

bool ParsePort(std::string_view text, uint16_t* port) {
  const char* end = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(text.data(), end, *port);
  return ec == std::errc() && ptr == end;
}

// Returns false (after printing why) on invalid arguments.
bool ParseArgs(int argc, char** argv, Options* options) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (arg == "--version") {
      options->show_version = true;
    } else if (arg == "--help" || arg == "-h") {
      options->show_help = true;
    } else if (arg == "--host" && has_value) {
      options->host = argv[++i];
    } else if (arg == "--port" && has_value) {
      if (!ParsePort(argv[++i], &options->port)) {
        std::fprintf(stderr, "sluice: invalid port '%s'\n", argv[i]);
        return false;
      }
    } else {
      std::fprintf(stderr, "sluice: unknown or incomplete argument '%s'\n", argv[i]);
      return false;
    }
  }
  return true;
}

// Delivers SIGINT/SIGTERM through a signalfd so they arrive as ordinary
// readable events on the loop: no async-signal-safety constraints, and no
// EINTR racing with epoll_wait(). Returns an invalid fd on failure.
sluice::net::UniqueFd CreateShutdownSignalFd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  // The signals must be blocked, or their default action (terminate) would
  // run before the signalfd ever becomes readable.
  if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
    return sluice::net::UniqueFd();
  }
  return sluice::net::UniqueFd(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseArgs(argc, argv, &options)) {
    PrintUsage();
    return 2;
  }
  if (options.show_help) {
    PrintUsage();
    return 0;
  }
  if (options.show_version) {
    std::printf("sluice %s\n", std::string(sluice::GetVersion()).c_str());
    return 0;
  }

  const auto listen_addr = sluice::net::InetAddress::FromIpPort(options.host, options.port);
  if (!listen_addr) {
    std::fprintf(stderr, "sluice: invalid IPv4 address '%s'\n", std::string(options.host).c_str());
    return 2;
  }

  int err = 0;
  auto loop = sluice::net::EventLoop::Create(&err);
  if (!loop) {
    std::fprintf(stderr, "sluice: epoll_create1: %s\n", std::strerror(err));
    return 1;
  }

  sluice::net::UniqueFd signal_fd = CreateShutdownSignalFd();
  if (!signal_fd.valid()) {
    std::fprintf(stderr, "sluice: signalfd: %s\n", std::strerror(errno));
    return 1;
  }
  sluice::net::Channel signal_channel(loop.get(), signal_fd.get());
  signal_channel.SetReadCallback([&] {
    signalfd_siginfo info{};
    // Draining the signalfd is not strictly needed since we quit anyway,
    // but it keeps the descriptor from staying readable.
    static_cast<void>(read(signal_fd.get(), &info, sizeof(info)));
    std::fprintf(stderr, "sluice: received %s, shutting down\n",
                 strsignal(static_cast<int>(info.ssi_signo)));
    loop->Quit();
  });
  err = signal_channel.EnableReading();
  if (err != 0) {
    std::fprintf(stderr, "sluice: epoll_ctl: %s\n", std::strerror(err));
    return 1;
  }

  auto server = sluice::echo::EchoServer::Create(loop.get(), *listen_addr, &err);
  if (server) {
    err = server->Start();
  }
  if (err != 0) {
    std::fprintf(stderr, "sluice: cannot listen on %s: %s\n", listen_addr->ToString().c_str(),
                 std::strerror(err));
    // A Channel must be unregistered before it is destroyed.
    signal_channel.Remove();
    return 1;
  }

  std::fprintf(stderr, "sluice %s: echo server listening on %s\n",
               std::string(sluice::GetVersion()).c_str(), listen_addr->ToString().c_str());
  loop->Loop();

  // Tear down in reverse order of registration; the loop is destroyed last
  // (it was declared first).
  server.reset();
  signal_channel.Remove();
  return 0;
}
