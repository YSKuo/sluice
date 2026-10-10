#include "net/inet_address.h"

#include <gtest/gtest.h>

namespace sluice::net {
namespace {

TEST(InetAddressTest, DefaultIsAnyPortZero) { EXPECT_EQ(InetAddress().ToString(), "0.0.0.0:0"); }

TEST(InetAddressTest, ParsesDottedQuad) {
  const auto addr = InetAddress::FromIpPort("192.168.1.20", 8080);
  ASSERT_TRUE(addr.has_value());
  const InetAddress parsed = addr.value_or(InetAddress());
  EXPECT_EQ(parsed.ToString(), "192.168.1.20:8080");
  EXPECT_EQ(parsed.port(), 8080);
}

TEST(InetAddressTest, RejectsInvalidAddresses) {
  EXPECT_FALSE(InetAddress::FromIpPort("", 80).has_value());
  EXPECT_FALSE(InetAddress::FromIpPort("256.0.0.1", 80).has_value());
  EXPECT_FALSE(InetAddress::FromIpPort("localhost", 80).has_value());
  EXPECT_FALSE(InetAddress::FromIpPort("::1", 80).has_value());
}

TEST(InetAddressTest, NamedConstructors) {
  EXPECT_EQ(InetAddress::Any(80).ToString(), "0.0.0.0:80");
  EXPECT_EQ(InetAddress::Loopback(65535).ToString(), "127.0.0.1:65535");
}

}  // namespace
}  // namespace sluice::net
