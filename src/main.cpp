#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "http/http_server.h"
#include "http/request.h"
#include "http/response.h"
#include "net/channel.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/unique_fd.h"
#include "result.h"
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
      "Week 2 milestone: serves HTTP/1.1 on HOST:PORT (default\n"
      "0.0.0.0:8080), answering every request with a plain-text summary of\n"
      "it. Stop it with Ctrl-C or SIGTERM.\n");
}

// Parses a whole string as a TCP port; trailing characters are rejected.
std::optional<uint16_t> ParsePort(std::string_view text) {
  const char* const first = text.data();
  const char* const last = first + text.size();
  uint16_t port = 0;
  const auto [ptr, ec] = std::from_chars(first, last, port);
  if (ec != std::errc() || ptr != last) {
    return std::nullopt;
  }
  return port;
}

// Returns nullopt (after printing why) on invalid arguments. `args` is the
// full argv, including the program name.
std::optional<Options> ParseArgs(std::span<char* const> args) {
  Options options;
  for (size_t i = 1; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    const bool has_value = i + 1 < args.size();
    if (arg == "--version") {
      options.show_version = true;
    } else if (arg == "--help" || arg == "-h") {
      options.show_help = true;
    } else if (arg == "--host" && has_value) {
      options.host = args[++i];
    } else if (arg == "--port" && has_value) {
      const std::string_view value = args[++i];
      const std::optional<uint16_t> port = ParsePort(value);
      if (!port) {
        std::fprintf(stderr, "sluice: invalid port '%s'\n",
                     std::string(value).c_str());
        return std::nullopt;
      }
      options.port = *port;
    } else {
      std::fprintf(stderr, "sluice: unknown or incomplete argument '%s'\n",
                   std::string(arg).c_str());
      return std::nullopt;
    }
  }
  return options;
}

// The Week 2 handler: describes the request back to the client. Week 3
// replaces it with routing and the reverse proxy.
sluice::http::Response DescribeRequest(const sluice::http::Request& request) {
  sluice::http::Response response;
  response.headers.Add("Content-Type", "text/plain; charset=utf-8");
  response.body =
      std::format("sluice {}\n{} {}\nbody: {} bytes\n", sluice::GetVersion(),
                  request.method, request.target, request.body.size());
  return response;
}

// Delivers SIGINT/SIGTERM through a signalfd so they arrive as ordinary
// readable events on the loop: no async-signal-safety constraints, and no
// EINTR racing with epoll_wait().
sluice::Result<sluice::net::UniqueFd> CreateShutdownSignalFd() {
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  // The signals must be blocked, or their default action (terminate) would
  // run before the signalfd ever becomes readable.
  if (sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
    return sluice::LastErrnoError();
  }
  sluice::net::UniqueFd fd(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
  if (!fd.valid()) {
    return sluice::LastErrnoError();
  }
  return fd;
}

}  // namespace

int main(int argc, char** argv) {
  const std::optional<Options> options =
      ParseArgs(std::span(argv, static_cast<size_t>(argc)));
  if (!options) {
    PrintUsage();
    return 2;
  }
  if (options->show_help) {
    PrintUsage();
    return 0;
  }
  if (options->show_version) {
    std::printf("sluice %s\n", std::string(sluice::GetVersion()).c_str());
    return 0;
  }

  const std::optional<sluice::net::InetAddress> listen_addr =
      sluice::net::InetAddress::FromIpPort(options->host, options->port);
  if (!listen_addr) {
    std::fprintf(stderr, "sluice: invalid IPv4 address '%s'\n",
                 std::string(options->host).c_str());
    return 2;
  }

  sluice::Result<std::unique_ptr<sluice::net::EventLoop>> loop =
      sluice::net::EventLoop::Create();
  if (!loop) {
    std::fprintf(stderr, "sluice: epoll_create1: %s\n",
                 loop.error().message().c_str());
    return 1;
  }
  sluice::net::EventLoop& event_loop = **loop;

  sluice::Result<sluice::net::UniqueFd> signal_fd = CreateShutdownSignalFd();
  if (!signal_fd) {
    std::fprintf(stderr, "sluice: signalfd: %s\n",
                 signal_fd.error().message().c_str());
    return 1;
  }
  const int signal_fd_num = signal_fd->get();
  sluice::net::Channel signal_channel(&event_loop, signal_fd_num);
  signal_channel.SetReadCallback([signal_fd_num, &event_loop] {
    signalfd_siginfo info{};
    // Draining the signalfd is not strictly needed since we quit anyway,
    // but it keeps the descriptor from staying readable.
    static_cast<void>(read(signal_fd_num, &info, sizeof(info)));
    std::fprintf(stderr, "sluice: received %s, shutting down\n",
                 strsignal(static_cast<int>(info.ssi_signo)));
    event_loop.Quit();
  });
  if (sluice::Result<void> r = signal_channel.EnableReading(); !r) {
    std::fprintf(stderr, "sluice: epoll_ctl: %s\n",
                 r.error().message().c_str());
    return 1;
  }

  sluice::Result<std::unique_ptr<sluice::http::HttpServer>> server =
      sluice::http::HttpServer::Create(&event_loop, *listen_addr,
                                       DescribeRequest);
  sluice::Result<void> started;
  if (server) {
    started = (*server)->Start();
  } else {
    started = sluice::Error(server.error());
  }
  if (!started) {
    std::fprintf(stderr, "sluice: cannot listen on %s: %s\n",
                 listen_addr->ToString().c_str(),
                 started.error().message().c_str());
    // A Channel must be unregistered before it is destroyed.
    signal_channel.Remove();
    return 1;
  }

  std::fprintf(stderr, "sluice %s: HTTP server listening on %s\n",
               std::string(sluice::GetVersion()).c_str(),
               listen_addr->ToString().c_str());
  event_loop.Loop();

  // Tear down in reverse order of registration; the loop is destroyed last
  // (it was declared first).
  server->reset();
  signal_channel.Remove();
  return 0;
}
