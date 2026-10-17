#include "gtest/gtest.h"

#include "http/headers.h"
#include "http/request.h"

namespace sluice::http {
namespace {

TEST(HeadersTest, EqualsIgnoreCase) {
  EXPECT_TRUE(EqualsIgnoreCase("Content-Length", "content-length"));
  EXPECT_TRUE(EqualsIgnoreCase("", ""));
  EXPECT_FALSE(EqualsIgnoreCase("Host", "Hosts"));
  EXPECT_FALSE(EqualsIgnoreCase("Host", "Hast"));
}

TEST(HeadersTest, HasToken) {
  EXPECT_TRUE(HasToken("close", "close"));
  EXPECT_TRUE(HasToken("Keep-Alive, Upgrade", "keep-alive"));
  EXPECT_TRUE(HasToken("upgrade ,  CLOSE ", "close"));
  EXPECT_FALSE(HasToken("closed", "close"));
  EXPECT_FALSE(HasToken("", "close"));
}

TEST(HeadersTest, GetReturnsFirstMatchIgnoringCase) {
  Headers headers;
  headers.Add("Accept", "text/plain");
  headers.Add("accept", "text/html");
  ASSERT_TRUE(headers.Get("ACCEPT").has_value());
  EXPECT_EQ(headers.Get("ACCEPT").value_or(""), "text/plain");
  EXPECT_FALSE(headers.Get("Host").has_value());
  EXPECT_EQ(headers.Count("Accept"), 2U);
  EXPECT_EQ(headers.size(), 2U);
}

Request MakeRequest(Version version, const char* connection) {
  Request request;
  request.version = version;
  if (connection != nullptr) {
    request.headers.Add("Connection", connection);
  }
  return request;
}

TEST(KeepAliveTest, Http11StaysOpenUnlessClose) {
  EXPECT_TRUE(IsKeepAlive(MakeRequest(Version::kHttp11, nullptr)));
  EXPECT_TRUE(IsKeepAlive(MakeRequest(Version::kHttp11, "keep-alive")));
  EXPECT_FALSE(IsKeepAlive(MakeRequest(Version::kHttp11, "close")));
  EXPECT_FALSE(IsKeepAlive(MakeRequest(Version::kHttp11, "Upgrade, Close")));
}

TEST(KeepAliveTest, Http10ClosesUnlessKeepAlive) {
  EXPECT_FALSE(IsKeepAlive(MakeRequest(Version::kHttp10, nullptr)));
  EXPECT_TRUE(IsKeepAlive(MakeRequest(Version::kHttp10, "Keep-Alive")));
  EXPECT_FALSE(IsKeepAlive(MakeRequest(Version::kHttp10, "keep-alive, close")));
}

}  // namespace
}  // namespace sluice::http
