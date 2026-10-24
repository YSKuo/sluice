#ifndef SLUICE_PROXY_ROUTE_TABLE_H_
#define SLUICE_PROXY_ROUTE_TABLE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "net/inet_address.h"
#include "proxy/upstream.h"
#include "result.h"

namespace sluice::proxy {

// A route as written on the command line, before name resolution:
// "/api/=backend-a:9000,backend-b:9000".
struct RouteSpec {
  struct Target {
    std::string host;
    uint16_t port;
  };
  std::string prefix;
  std::vector<Target> targets;
};

// Parses "PREFIX=HOST:PORT[,HOST:PORT...]". The prefix must start with '/'.
// Returns nullopt if the text is malformed.
std::optional<RouteSpec> ParseRouteSpec(std::string_view text);

// The gateway's routing table: maps a request path to the round-robin
// balancer of the route with the longest matching prefix.
//
// Owns every Upstream. An upstream that appears in several routes (same
// address) is one Upstream object, so its down-marking applies everywhere.
class RouteTable {
 public:
  struct Route {
    std::string prefix;
    RoundRobin balancer;
  };

  RouteTable() = default;
  // Routes hold pointers into upstreams_; moving keeps them valid because
  // the Upstreams live behind unique_ptr, but copying would not.
  RouteTable(const RouteTable&) = delete;
  RouteTable& operator=(const RouteTable&) = delete;
  RouteTable(RouteTable&&) = default;
  RouteTable& operator=(RouteTable&&) = default;
  ~RouteTable() = default;

  // Adds a route. `targets` pairs each display name with its resolved
  // address. A prefix that is already present is rejected (EEXIST).
  [[nodiscard]] Result<void> AddRoute(
      std::string prefix,
      const std::vector<std::pair<std::string, net::InetAddress>>& targets);

  // The route whose prefix is the longest match for `target`'s path (the
  // part before '?'), or nullptr. Matching respects path segments: the
  // prefix "/api" matches "/api" and "/api/users" but not "/apikey"; a
  // prefix ending in '/' ("/api/") matches anything below it.
  Route* Match(std::string_view target);

  [[nodiscard]] const std::vector<Route>& routes() const noexcept {
    return routes_;
  }

 private:
  Upstream* FindOrAddUpstream(const std::string& name,
                              const net::InetAddress& address);

  std::vector<std::unique_ptr<Upstream>> upstreams_;
  std::vector<Route> routes_;
};

}  // namespace sluice::proxy

#endif  // SLUICE_PROXY_ROUTE_TABLE_H_
