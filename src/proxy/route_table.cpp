#include "proxy/route_table.h"

#include <cerrno>
#include <charconv>
#include <system_error>
#include <utility>

namespace sluice::proxy {

namespace {

// Parses "HOST:PORT" (the last colon separates the port).
std::optional<RouteSpec::Target> ParseTarget(std::string_view text) {
  const size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon == 0) {
    return std::nullopt;
  }
  const std::string_view port_text = text.substr(colon + 1);
  uint16_t port = 0;
  const char* const first = port_text.data();
  const char* const last = first + port_text.size();
  const auto [ptr, ec] = std::from_chars(first, last, port);
  if (ec != std::errc() || ptr != last || port == 0) {
    return std::nullopt;
  }
  return RouteSpec::Target{.host = std::string(text.substr(0, colon)),
                           .port = port};
}

bool PrefixMatches(std::string_view prefix, std::string_view path) {
  if (!path.starts_with(prefix)) {
    return false;
  }
  // "/api/" covers everything below it; "/api" only whole segments.
  return prefix.ends_with('/') || path.size() == prefix.size() ||
         path[prefix.size()] == '/';
}

}  // namespace

std::optional<RouteSpec> ParseRouteSpec(std::string_view text) {
  const size_t equals = text.find('=');
  if (equals == std::string_view::npos) {
    return std::nullopt;
  }
  RouteSpec spec;
  spec.prefix = std::string(text.substr(0, equals));
  if (spec.prefix.empty() || spec.prefix.front() != '/') {
    return std::nullopt;
  }
  std::string_view rest = text.substr(equals + 1);
  while (true) {
    const size_t comma = rest.find(',');
    const std::optional<RouteSpec::Target> target =
        ParseTarget(rest.substr(0, comma));
    if (!target) {
      return std::nullopt;
    }
    spec.targets.push_back(*target);
    if (comma == std::string_view::npos) {
      break;
    }
    rest.remove_prefix(comma + 1);
  }
  return spec;
}

Result<void> RouteTable::AddRoute(
    std::string prefix,
    const std::vector<std::pair<std::string, net::InetAddress>>& targets) {
  for (const Route& route : routes_) {
    if (route.prefix == prefix) {
      return ErrnoError(EEXIST);
    }
  }
  if (targets.empty()) {
    return ErrnoError(EINVAL);
  }
  std::vector<Upstream*> upstreams;
  upstreams.reserve(targets.size());
  for (const auto& [name, address] : targets) {
    upstreams.push_back(FindOrAddUpstream(name, address));
  }
  routes_.push_back(
      Route{.prefix = std::move(prefix), .balancer = RoundRobin(upstreams)});
  return {};
}

RouteTable::Route* RouteTable::Match(std::string_view target) {
  const std::string_view path = target.substr(0, target.find('?'));
  Route* best = nullptr;
  for (Route& route : routes_) {
    if (PrefixMatches(route.prefix, path) &&
        (best == nullptr || route.prefix.size() > best->prefix.size())) {
      best = &route;
    }
  }
  return best;
}

Upstream* RouteTable::FindOrAddUpstream(const std::string& name,
                                        const net::InetAddress& address) {
  for (const std::unique_ptr<Upstream>& upstream : upstreams_) {
    if (upstream->address().ToString() == address.ToString()) {
      return upstream.get();
    }
  }
  upstreams_.push_back(std::make_unique<Upstream>(name, address));
  return upstreams_.back().get();
}

}  // namespace sluice::proxy
