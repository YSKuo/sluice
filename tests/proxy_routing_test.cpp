#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "net/inet_address.h"
#include "proxy/route_table.h"
#include "proxy/upstream.h"

namespace sluice::proxy {
namespace {

using std::chrono::seconds;

std::pair<std::string, net::InetAddress> Target(const std::string& name,
                                                uint16_t port) {
  return {name, net::InetAddress::Loopback(port)};
}

// Names of the upstreams picked by `count` consecutive requests.
std::vector<std::string> Picks(RoundRobin& balancer, int count,
                               Clock::time_point now) {
  std::vector<std::string> names;
  for (int i = 0; i < count; ++i) {
    const Upstream* upstream = balancer.Pick(now);
    names.push_back(upstream == nullptr ? "none" : upstream->name());
  }
  return names;
}

TEST(ParseRouteSpecTest, ParsesPrefixAndTargets) {
  const std::optional<RouteSpec> spec =
      ParseRouteSpec("/api/=backend-a:9000,10.0.0.2:81");
  ASSERT_TRUE(spec.has_value());
  EXPECT_EQ(spec->prefix, "/api/");
  ASSERT_EQ(spec->targets.size(), 2U);
  EXPECT_EQ(spec->targets[0].host, "backend-a");
  EXPECT_EQ(spec->targets[0].port, 9000);
  EXPECT_EQ(spec->targets[1].host, "10.0.0.2");
  EXPECT_EQ(spec->targets[1].port, 81);
}

TEST(ParseRouteSpecTest, RejectsMalformedSpecs) {
  for (const char* bad : {"", "/api", "api=a:1", "/=a", "/=a:", "/=a:0",
                          "/=a:70000", "/=a:1,", "/=:80", "/=a:1x"}) {
    EXPECT_FALSE(ParseRouteSpec(bad).has_value()) << bad;
  }
}

TEST(RoundRobinTest, RotatesAndSkipsDownUpstreams) {
  Upstream a("a", net::InetAddress::Loopback(1));
  Upstream b("b", net::InetAddress::Loopback(2));
  Upstream c("c", net::InetAddress::Loopback(3));
  RoundRobin balancer({&a, &b, &c});
  const Clock::time_point now = Clock::now();

  EXPECT_EQ(Picks(balancer, 4, now),
            (std::vector<std::string>{"a", "b", "c", "a"}));

  b.MarkDown(now, seconds(10));
  EXPECT_EQ(Picks(balancer, 4, now),
            (std::vector<std::string>{"c", "a", "c", "a"}));

  // After the cooldown, b is tried again.
  EXPECT_TRUE(b.IsUp(now + seconds(10)));
  EXPECT_EQ(Picks(balancer, 3, now + seconds(10)),
            (std::vector<std::string>{"b", "c", "a"}));
}

TEST(RoundRobinTest, ReturnsNullWhenAllAreDown) {
  Upstream a("a", net::InetAddress::Loopback(1));
  RoundRobin balancer({&a});
  const Clock::time_point now = Clock::now();
  a.MarkDown(now, seconds(5));
  EXPECT_EQ(balancer.Pick(now), nullptr);
  EXPECT_EQ(balancer.Pick(now + seconds(5)), &a);
}

TEST(RouteTableTest, LongestPrefixWins) {
  RouteTable table;
  ASSERT_TRUE(table.AddRoute("/", {Target("root", 1)}).has_value());
  ASSERT_TRUE(table.AddRoute("/api", {Target("api", 2)}).has_value());
  ASSERT_TRUE(table.AddRoute("/api/admin/", {Target("admin", 3)}).has_value());

  const auto route_of = [&table](std::string_view target) {
    const RouteTable::Route* route = table.Match(target);
    return route == nullptr ? std::string("none") : route->prefix;
  };
  EXPECT_EQ(route_of("/"), "/");
  EXPECT_EQ(route_of("/index.html"), "/");
  EXPECT_EQ(route_of("/api"), "/api");
  EXPECT_EQ(route_of("/api/users?id=1"), "/api");
  EXPECT_EQ(route_of("/api?x=1"), "/api");
  // Segment boundaries: "/api" does not cover "/apikey".
  EXPECT_EQ(route_of("/apikey"), "/");
  EXPECT_EQ(route_of("/api/admin/users"), "/api/admin/");
  // "/api/admin/" (trailing slash) does not cover "/api/admin" itself.
  EXPECT_EQ(route_of("/api/admin"), "/api");
}

TEST(RouteTableTest, NoMatchWithoutCatchAll) {
  RouteTable table;
  ASSERT_TRUE(table.AddRoute("/api", {Target("api", 2)}).has_value());
  EXPECT_EQ(table.Match("/other"), nullptr);
}

TEST(RouteTableTest, RejectsDuplicateAndEmptyRoutes) {
  RouteTable table;
  ASSERT_TRUE(table.AddRoute("/", {Target("a", 1)}).has_value());
  EXPECT_FALSE(table.AddRoute("/", {Target("b", 2)}).has_value());
  EXPECT_FALSE(table.AddRoute("/x", {}).has_value());
}

TEST(RouteTableTest, SharesUpstreamsAcrossRoutes) {
  RouteTable table;
  ASSERT_TRUE(
      table.AddRoute("/", {Target("a", 1), Target("b", 2)}).has_value());
  ASSERT_TRUE(table.AddRoute("/api", {Target("a", 1)}).has_value());
  // One Upstream object for a: marking it down affects both routes.
  RouteTable::Route* root = table.Match("/");
  RouteTable::Route* api = table.Match("/api");
  ASSERT_NE(root, nullptr);
  ASSERT_NE(api, nullptr);
  EXPECT_EQ(root->balancer.upstreams()[0], api->balancer.upstreams()[0]);
}

}  // namespace
}  // namespace sluice::proxy
