#include "gtest/gtest.h"

#include "http/response.h"
#include "http/status.h"

namespace sluice::http {
namespace {

TEST(ResponseTest, SerializesStatusHeadersAndBody) {
  Response response;
  response.headers.Add("Content-Type", "text/plain");
  response.body = "hello";
  EXPECT_EQ(Serialize(response, /*keep_alive=*/true),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 5\r\n"
            "Connection: keep-alive\r\n"
            "\r\n"
            "hello");
}

TEST(ResponseTest, SerializesEmptyBodyWithClose) {
  Response response;
  response.status = StatusCode::kNotFound;
  EXPECT_EQ(Serialize(response, /*keep_alive=*/false),
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n");
}

TEST(ResponseTest, ErrorResponseDescribesStatus) {
  const Response response =
      MakeErrorResponse(StatusCode::kRequestHeaderFieldsTooLarge);
  EXPECT_EQ(response.status, StatusCode::kRequestHeaderFieldsTooLarge);
  EXPECT_EQ(response.body, "431 Request Header Fields Too Large\n");
}

}  // namespace
}  // namespace sluice::http
