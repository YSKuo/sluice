#include <string>

#include "gtest/gtest.h"

#include "http/request.h"
#include "http/response_parser.h"
#include "net/inet_address.h"
#include "proxy/forwarding.h"

namespace sluice::proxy {
namespace {

TEST(ForwardingTest, HopByHopFields) {
  http::Headers headers;
  headers.Add("Connection", "keep-alive, X-Secret");
  EXPECT_TRUE(IsHopByHop("connection", headers));
  EXPECT_TRUE(IsHopByHop("Keep-Alive", headers));
  EXPECT_TRUE(IsHopByHop("transfer-encoding", headers));
  EXPECT_TRUE(IsHopByHop("x-secret", headers));  // Named by Connection.
  EXPECT_FALSE(IsHopByHop("Content-Type", headers));
}

TEST(ForwardingTest, DotSegments) {
  EXPECT_TRUE(HasDotSegment("/a/../b"));
  EXPECT_TRUE(HasDotSegment("/.."));
  EXPECT_TRUE(HasDotSegment("/a/./b"));
  EXPECT_TRUE(HasDotSegment("/a/.."));
  EXPECT_FALSE(HasDotSegment("/"));
  EXPECT_FALSE(HasDotSegment("/a/b"));
  EXPECT_FALSE(HasDotSegment("/a/..b/.c"));
  EXPECT_FALSE(HasDotSegment("/a?x=../y"));  // The query is not a path.
}

TEST(ForwardingTest, BuildsUpstreamRequest) {
  http::Request request;
  request.method = "POST";
  request.target = "/api/items?x=1";
  request.headers.Add("Host", "gateway.test");
  request.headers.Add("Connection", "keep-alive, X-Secret");
  request.headers.Add("X-Secret", "s3cr3t");
  request.headers.Add("Keep-Alive", "timeout=5");
  request.headers.Add("Content-Type", "text/plain");
  request.headers.Add("Content-Length", "2");
  request.headers.Add("Expect", "100-continue");
  request.headers.Add("X-Forwarded-For", "10.1.1.1");
  request.body = "hi";

  EXPECT_EQ(
      BuildUpstreamRequest(
          request, net::InetAddress::FromIpPort("192.0.2.7", 5555).value(),
          "backend-a:9000"),
      "POST /api/items?x=1 HTTP/1.1\r\n"
      "Host: gateway.test\r\n"
      "Content-Type: text/plain\r\n"
      "X-Forwarded-For: 10.1.1.1, 192.0.2.7\r\n"
      "Content-Length: 2\r\n"
      "Connection: close\r\n"
      "\r\n"
      "hi");
}

TEST(ForwardingTest, AddsHostForHttp10Clients) {
  http::Request request;
  request.method = "GET";
  request.target = "/";
  request.version = http::Version::kHttp10;
  EXPECT_EQ(BuildUpstreamRequest(request, net::InetAddress::Loopback(1),
                                 "backend-a:9000"),
            "GET / HTTP/1.1\r\n"
            "Host: backend-a:9000\r\n"
            "X-Forwarded-For: 127.0.0.1\r\n"
            "Connection: close\r\n"
            "\r\n");
}

TEST(ForwardingTest, BuildsClientResponseHead) {
  http::ResponseHead head;
  head.version = http::Version::kHttp10;
  head.status = 200;
  head.reason = "OK";
  head.headers.Add("Content-Type", "text/plain");
  head.headers.Add("Connection", "close");
  head.headers.Add("Content-Length", "3");
  head.headers.Add("content-length", "3");
  EXPECT_EQ(BuildClientResponseHead(head, /*keep_alive=*/true),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 3\r\n"
            "Connection: keep-alive\r\n"
            "\r\n");
}

}  // namespace
}  // namespace sluice::proxy
