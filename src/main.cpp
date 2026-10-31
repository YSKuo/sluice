#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/channel.h"
#include "net/event_loop.h"
#include "net/inet_address.h"
#include "net/unique_fd.h"
#include "proxy/proxy_options.h"
#include "proxy/proxy_server.h"
#include "proxy/route_table.h"
#include "result.h"
#include "version.h"

namespace {

struct Options {
  std::string_view host = "0.0.0.0";
  uint16_t port = 8080;
  std::vector<sluice::proxy::RouteSpec> routes;
  std::optional<sluice::proxy::RateLimit> rate_limit;
  bool show_version = false;
  bool show_help = false;
};

void PrintUsage() {
  std::printf(
      "usage: sluice --route PREFIX=HOST:PORT[,HOST:PORT...] [--route ...]\n"
      "              [--rate-limit RPS:BURST] [--host IPV4] [--port PORT]\n"
      "              [--version] [--help]\n"
      "\n"
      "Reverse proxy listening on HOST:PORT (default 0.0.0.0:8080). Each\n"
      "request goes to the route with the longest matching path prefix, and\n"
      "round robin across that route's upstreams, e.g.\n"
      "  --route /api/=10.0.0.5:9000,10.0.0.6:9000 --route /=10.0.0.7:9000\n"
      "Upstream host names are resolved once at startup.\n"
      "--rate-limit allows each client IP RPS requests per second on\n"
      "average, in bursts of up to BURST (e.g. 100:200); excess requests get\n"
      "429. Stop it with Ctrl-C or SIGTERM.\n");
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
    } else if (arg == "--route" && has_value) {
      const std::string_view value = args[++i];
      std::optional<sluice::proxy::RouteSpec> route =
          sluice::proxy::ParseRouteSpec(value);
      if (!route) {
        std::fprintf(stderr, "sluice: invalid route '%s'\n",
                     std::string(value).c_str());
        return std::nullopt;
      }
      options.routes.push_back(*std::move(route));
    } else if (arg == "--rate-limit" && has_value) {
      const std::string_view value = args[++i];
      options.rate_limit = sluice::proxy::ParseRateLimit(value);
      if (!options.rate_limit) {
        std::fprintf(stderr, "sluice: invalid rate limit '%s'\n",
                     std::string(value).c_str());
        return std::nullopt;
      }
    } else {
      std::fprintf(stderr, "sluice: unknown or incomplete argument '%s'\n",
                   std::string(arg).c_str());
      return std::nullopt;
    }
  }
  return options;
}

// Resolves every route's upstream names (blocking DNS, so only at
// startup) and builds the routing table. Prints why on failure.
std::optional<sluice::proxy::RouteTable> BuildRouteTable(
    const std::vector<sluice::proxy::RouteSpec>& specs) {
  sluice::proxy::RouteTable table;
  for (const sluice::proxy::RouteSpec& spec : specs) {
    std::vector<std::pair<std::string, sluice::net::InetAddress>> targets;
    for (const sluice::proxy::RouteSpec::Target& target : spec.targets) {
      const sluice::Result<sluice::net::InetAddress> address =
          sluice::net::InetAddress::Resolve(target.host, target.port);
      if (!address) {
        std::fprintf(stderr, "sluice: cannot resolve '%s'\n",
                     target.host.c_str());
        return std::nullopt;
      }
      targets.emplace_back(target.host + ":" + std::to_string(target.port),
                           *address);
    }
    if (sluice::Result<void> added = table.AddRoute(spec.prefix, targets);
        !added) {
      std::fprintf(stderr, "sluice: duplicate route '%s'\n",
                   spec.prefix.c_str());
      return std::nullopt;
    }
  }
  return table;
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

  if (options->routes.empty()) {
    std::fprintf(stderr, "sluice: at least one --route is required\n");
    PrintUsage();
    return 2;
  }
  std::optional<sluice::proxy::RouteTable> routes =
      BuildRouteTable(options->routes);
  if (!routes) {
    return 2;
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

  sluice::proxy::ProxyOptions proxy_options;
  proxy_options.rate_limit = options->rate_limit;
  sluice::Result<std::unique_ptr<sluice::proxy::ProxyServer>> server =
      sluice::proxy::ProxyServer::Create(&event_loop, *listen_addr,
                                         *std::move(routes), proxy_options);
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

  std::fprintf(stderr, "sluice %s: proxy listening on %s\n",
               std::string(sluice::GetVersion()).c_str(),
               listen_addr->ToString().c_str());
  for (const sluice::proxy::RouteTable::Route& route :
       (*server)->routes().routes()) {
    std::string upstreams;
    for (const sluice::proxy::Upstream* upstream : route.balancer.upstreams()) {
      upstreams += (upstreams.empty() ? "" : ", ") + upstream->name();
    }
    std::fprintf(stderr, "  route %s -> %s\n", route.prefix.c_str(),
                 upstreams.c_str());
  }
  if (options->rate_limit) {
    std::fprintf(stderr, "  rate limit: %g req/s per client IP, burst %g\n",
                 options->rate_limit->requests_per_second,
                 options->rate_limit->burst);
  }
  event_loop.Loop();

  // Tear down in reverse order of registration; the loop is destroyed last
  // (it was declared first).
  server->reset();
  signal_channel.Remove();
  return 0;
}
