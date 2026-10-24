#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "gtest/gtest.h"

#include "http/request.h"
#include "http/response_parser.h"

namespace sluice::http {
namespace {

using Status = ResponseParser::Status;

struct Outcome {
  Status status = Status::kNeedMore;
  ResponseHead head;
  std::string rest;  // Unconsumed bytes after the head.
};

// Feeds `input` `chunk_size` bytes at a time, like UpstreamCall does.
Outcome Parse(std::string_view input, size_t chunk_size,
              ParserLimits limits = {}) {
  ResponseParser parser(limits);
  std::string buffer;
  size_t fed = 0;
  while (true) {
    const ResponseParser::FeedResult result = parser.Feed(buffer);
    buffer.erase(0, result.consumed);
    if (result.status == Status::kComplete) {
      buffer += input.substr(fed);
      return {.status = Status::kComplete,
              .head = parser.TakeHead(),
              .rest = buffer};
    }
    if (result.status == Status::kError || fed == input.size()) {
      return {.status = result.status, .head = {}, .rest = buffer};
    }
    const size_t n = std::min(chunk_size, input.size() - fed);
    buffer.append(input.substr(fed, n));
    fed += n;
  }
}

Outcome ParseAll(std::string_view input, ParserLimits limits = {}) {
  return Parse(input, input.size(), limits);
}

constexpr std::string_view kResponse =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 5\r\n"
    "\r\n"
    "nope!";

TEST(ResponseParserTest, ParsesHeadAndLeavesBody) {
  const Outcome outcome = ParseAll(kResponse);
  ASSERT_EQ(outcome.status, Status::kComplete);
  EXPECT_EQ(outcome.head.version, Version::kHttp11);
  EXPECT_EQ(outcome.head.status, 404);
  EXPECT_EQ(outcome.head.reason, "Not Found");
  EXPECT_EQ(outcome.head.headers.Get("content-type").value_or(""),
            "text/plain");
  EXPECT_EQ(outcome.rest, "nope!");
}

TEST(ResponseParserTest, EveryChunkSizeGivesTheSameHead) {
  for (size_t chunk = 1; chunk <= kResponse.size(); ++chunk) {
    const Outcome outcome = Parse(kResponse, chunk);
    ASSERT_EQ(outcome.status, Status::kComplete) << "chunk " << chunk;
    EXPECT_EQ(outcome.head.status, 404) << "chunk " << chunk;
    EXPECT_EQ(outcome.rest, "nope!") << "chunk " << chunk;
  }
}

TEST(ResponseParserTest, AcceptsHttp10AndEmptyReason) {
  const Outcome with_space = ParseAll("HTTP/1.0 200 \r\n\r\n");
  ASSERT_EQ(with_space.status, Status::kComplete);
  EXPECT_EQ(with_space.head.version, Version::kHttp10);
  EXPECT_EQ(with_space.head.reason, "");
  EXPECT_EQ(ParseAll("HTTP/1.1 204\r\n\r\n").status, Status::kComplete);
}

TEST(ResponseParserTest, RejectsMalformedResponses) {
  for (const std::string_view bad : {
           std::string_view("HTTP/2.0 200 OK\r\n\r\n"),
           std::string_view("HTTP/1.1 2000 OK\r\n\r\n"),
           std::string_view("HTTP/1.1 600 Odd\r\n\r\n"),
           std::string_view("HTTP/1.1 20x OK\r\n\r\n"),
           std::string_view("garbage\r\n\r\n"),
           std::string_view("HTTP/1.1 200 OK\nX: y\r\n\r\n"),
           std::string_view("HTTP/1.1 200 OK\r\nBad Header: x\r\n\r\n"),
           std::string_view("HTTP/1.1 200 OK\r\nTransfer-Encoding: "
                            "chunked\r\n\r\n"),
       }) {
    EXPECT_EQ(ParseAll(bad).status, Status::kError) << bad;
  }
}

TEST(ResponseParserTest, EnforcesHeadLimits) {
  ParserLimits limits;
  limits.max_header_bytes = 32;
  EXPECT_EQ(
      ParseAll("HTTP/1.1 200 OK\r\nX: " + std::string(64, 'v') + "\r\n\r\n",
               limits)
          .status,
      Status::kError);
  // Rejected before the line ends.
  EXPECT_EQ(
      ParseAll("HTTP/1.1 200 OK\r\nX: " + std::string(64, 'v'), limits).status,
      Status::kError);
}

ResponseHead MakeHead(int status, std::optional<std::string> length) {
  ResponseHead head;
  head.status = status;
  if (length) {
    head.headers.Add("Content-Length", *length);
  }
  return head;
}

TEST(BodyLengthTest, FollowsRfc9112Rules) {
  const auto get = [](int status, std::optional<std::string> length,
                      std::string_view method = "GET") {
    return GetBodyLength(MakeHead(status, std::move(length)), method);
  };
  ASSERT_TRUE(get(200, "12").has_value());
  EXPECT_EQ(get(200, "12")->kind, BodyLength::Kind::kLength);
  EXPECT_EQ(get(200, "12")->length, 12U);
  EXPECT_EQ(get(200, std::nullopt)->kind, BodyLength::Kind::kUntilClose);
  // No body, whatever Content-Length says.
  EXPECT_EQ(get(204, "12")->length, 0U);
  EXPECT_EQ(get(304, "12")->length, 0U);
  EXPECT_EQ(get(200, "12", "HEAD")->length, 0U);
  EXPECT_EQ(get(200, std::nullopt, "HEAD")->kind, BodyLength::Kind::kLength);
  // Invalid framing.
  EXPECT_FALSE(get(200, "-1").has_value());
  EXPECT_FALSE(get(200, "99999999999999999999999").has_value());
}

}  // namespace
}  // namespace sluice::http
