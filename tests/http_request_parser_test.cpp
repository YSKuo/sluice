#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"

#include "http/request.h"
#include "http/request_parser.h"
#include "http/status.h"

namespace sluice::http {
namespace {

using Status = RequestParser::Status;

// What a connection would see after feeding `input` to one parser.
struct Outcome {
  Status status = Status::kNeedMore;
  std::vector<Request> requests;
  StatusCode error = StatusCode::kOk;
  // Bytes still buffered (unconsumed) when the input ran out.
  size_t leftover = 0;
};

// Drives a parser exactly the way HttpServer does: the bytes that arrived
// so far sit in a buffer, Feed() sees the whole buffer, consumed bytes are
// dropped, and new bytes arrive `chunk_size` at a time.
Outcome Parse(std::string_view input, size_t chunk_size,
              ParserLimits limits = {}) {
  RequestParser parser(limits);
  Outcome outcome;
  std::string buffer;
  size_t fed = 0;
  while (true) {
    const RequestParser::FeedResult result = parser.Feed(buffer);
    buffer.erase(0, result.consumed);
    if (result.status == Status::kComplete) {
      outcome.requests.push_back(parser.TakeRequest());
      continue;
    }
    if (result.status == Status::kError) {
      outcome.status = Status::kError;
      outcome.error = parser.error_status();
      return outcome;
    }
    if (fed == input.size()) {
      outcome.status = Status::kNeedMore;
      outcome.leftover = buffer.size();
      return outcome;
    }
    const size_t n = std::min(chunk_size, input.size() - fed);
    buffer.append(input.substr(fed, n));
    fed += n;
  }
}

Outcome ParseAll(std::string_view input, ParserLimits limits = {}) {
  return Parse(input, input.empty() ? 1 : input.size(), limits);
}

// Expects `input` to be rejected with `status`.
void ExpectError(std::string_view input, StatusCode status,
                 ParserLimits limits = {}) {
  const Outcome outcome = ParseAll(input, limits);
  EXPECT_EQ(outcome.status, Status::kError) << input;
  EXPECT_EQ(outcome.error, status) << input;
}

constexpr std::string_view kPost =
    "POST /api/items?id=7 HTTP/1.1\r\n"
    "Host: example.test\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 11\r\n"
    "\r\n"
    "hello world";

TEST(RequestParserTest, ParsesSimpleGet) {
  const Outcome outcome =
      ParseAll("GET / HTTP/1.1\r\nHost: example.test\r\n\r\n");
  ASSERT_EQ(outcome.requests.size(), 1U);
  const Request& request = outcome.requests[0];
  EXPECT_EQ(request.method, "GET");
  EXPECT_EQ(request.target, "/");
  EXPECT_EQ(request.version, Version::kHttp11);
  EXPECT_EQ(request.headers.Get("host").value_or(""), "example.test");
  EXPECT_TRUE(request.body.empty());
  EXPECT_EQ(outcome.status, Status::kNeedMore);
  EXPECT_EQ(outcome.leftover, 0U);
}

TEST(RequestParserTest, ParsesBody) {
  const Outcome outcome = ParseAll(kPost);
  ASSERT_EQ(outcome.requests.size(), 1U);
  EXPECT_EQ(outcome.requests[0].method, "POST");
  EXPECT_EQ(outcome.requests[0].target, "/api/items?id=7");
  EXPECT_EQ(outcome.requests[0].body, "hello world");
}

TEST(RequestParserTest, EveryChunkSizeGivesTheSameRequest) {
  // TCP may split the bytes anywhere; the parser must not care where.
  for (size_t chunk = 1; chunk <= kPost.size(); ++chunk) {
    const Outcome outcome = Parse(kPost, chunk);
    ASSERT_EQ(outcome.requests.size(), 1U) << "chunk size " << chunk;
    EXPECT_EQ(outcome.requests[0].body, "hello world") << "chunk " << chunk;
    EXPECT_EQ(outcome.requests[0].headers.size(), 3U) << "chunk " << chunk;
  }
}

TEST(RequestParserTest, PipelinedRequestsAreParsedInOrder) {
  const std::string input = std::string(kPost) +
                            "GET /second HTTP/1.1\r\nHost: a\r\n\r\n" +
                            "GET /third HTTP/1.1\r\nHo";
  for (size_t chunk : {size_t{1}, size_t{7}, input.size()}) {
    const Outcome outcome = Parse(input, chunk);
    ASSERT_EQ(outcome.requests.size(), 2U) << "chunk " << chunk;
    EXPECT_EQ(outcome.requests[0].target, "/api/items?id=7");
    EXPECT_EQ(outcome.requests[1].target, "/second");
    EXPECT_EQ(outcome.status, Status::kNeedMore);
    // The incomplete "Ho" header line stays buffered.
    EXPECT_EQ(outcome.leftover, 2U) << "chunk " << chunk;
  }
}

TEST(RequestParserTest, TrimsFieldValueWhitespace) {
  const Outcome outcome =
      ParseAll("GET / HTTP/1.1\r\nHost:\t example.test  \r\nX-Empty:\r\n\r\n");
  ASSERT_EQ(outcome.requests.size(), 1U);
  EXPECT_EQ(outcome.requests[0].headers.Get("Host").value_or("?"),
            "example.test");
  EXPECT_EQ(outcome.requests[0].headers.Get("X-Empty").value_or("?"), "");
}

TEST(RequestParserTest, IgnoresEmptyLinesBeforeRequestLine) {
  const Outcome outcome = ParseAll("\r\n\r\nGET / HTTP/1.1\r\nHost: a\r\n\r\n");
  EXPECT_EQ(outcome.requests.size(), 1U);
}

TEST(RequestParserTest, Http10DoesNotRequireHost) {
  const Outcome outcome = ParseAll("GET / HTTP/1.0\r\n\r\n");
  ASSERT_EQ(outcome.requests.size(), 1U);
  EXPECT_EQ(outcome.requests[0].version, Version::kHttp10);
}

TEST(RequestParserTest, IdenticalContentLengthsAreAccepted) {
  const Outcome outcome = ParseAll(
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n"
      "content-length: 2\r\n\r\nok");
  ASSERT_EQ(outcome.requests.size(), 1U);
  EXPECT_EQ(outcome.requests[0].body, "ok");
}

TEST(RequestParserTest, RejectsMalformedLineEndings) {
  ExpectError("GET / HTTP/1.1\nHost: a\r\n\r\n", StatusCode::kBadRequest);
  ExpectError("GET / HTTP/1.1\r\nHost: a\n\r\n", StatusCode::kBadRequest);
  ExpectError("GET / HT\rTP/1.1\r\nHost: a\r\n\r\n", StatusCode::kBadRequest);
  // A bare CR is caught even before any LF arrives.
  ExpectError("GET /\rx", StatusCode::kBadRequest);
}

TEST(RequestParserTest, RejectsMalformedRequestLines) {
  ExpectError("GET\r\n\r\n", StatusCode::kBadRequest);
  ExpectError("GET  / HTTP/1.1\r\nHost: a\r\n\r\n", StatusCode::kBadRequest);
  ExpectError("GET / HTTP/1.1 \r\nHost: a\r\n\r\n", StatusCode::kBadRequest);
  ExpectError("G(T / HTTP/1.1\r\nHost: a\r\n\r\n", StatusCode::kBadRequest);
  ExpectError("GET index.html HTTP/1.1\r\nHost: a\r\n\r\n",
              StatusCode::kBadRequest);
  ExpectError("GET http://a/ HTTP/1.1\r\nHost: a\r\n\r\n",
              StatusCode::kBadRequest);
  ExpectError("GET / http/1.1\r\nHost: a\r\n\r\n", StatusCode::kBadRequest);
  ExpectError("GET / HTTP/1.10\r\nHost: a\r\n\r\n", StatusCode::kBadRequest);
}

TEST(RequestParserTest, RejectsUnsupportedVersions) {
  ExpectError("GET / HTTP/2.0\r\nHost: a\r\n\r\n",
              StatusCode::kHttpVersionNotSupported);
  ExpectError("GET / HTTP/0.9\r\n\r\n", StatusCode::kHttpVersionNotSupported);
}

TEST(RequestParserTest, RejectsMalformedHeaders) {
  // Whitespace between name and colon (RFC 9112 section 5.1).
  ExpectError("GET / HTTP/1.1\r\nHost : a\r\n\r\n", StatusCode::kBadRequest);
  // obs-fold continuation line.
  ExpectError("GET / HTTP/1.1\r\nHost: a\r\nX-A: 1\r\n 2\r\n\r\n",
              StatusCode::kBadRequest);
  ExpectError("GET / HTTP/1.1\r\nHost a\r\n\r\n", StatusCode::kBadRequest);
  ExpectError("GET / HTTP/1.1\r\n: a\r\n\r\n", StatusCode::kBadRequest);
  // A NUL byte inside a field value.
  constexpr char kWithNul[] = "GET / HTTP/1.1\r\nHost: a\0b\r\n\r\n";
  ExpectError(std::string_view(kWithNul, sizeof(kWithNul) - 1),
              StatusCode::kBadRequest);
}

TEST(RequestParserTest, RequiresExactlyOneHostForHttp11) {
  ExpectError("GET / HTTP/1.1\r\n\r\n", StatusCode::kBadRequest);
  ExpectError("GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
              StatusCode::kBadRequest);
}

TEST(RequestParserTest, RejectsTransferEncoding) {
  ExpectError(
      "POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n",
      StatusCode::kNotImplemented);
  // The smuggling combination: both framings present.
  ExpectError(
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\n"
      "Transfer-Encoding: chunked\r\n\r\n",
      StatusCode::kNotImplemented);
}

TEST(RequestParserTest, RejectsInvalidContentLength) {
  const auto with_length = [](std::string_view value) {
    return "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: " +
           std::string(value) + "\r\n\r\n";
  };
  ExpectError(with_length("+5"), StatusCode::kBadRequest);
  ExpectError(with_length("-1"), StatusCode::kBadRequest);
  ExpectError(with_length("5, 5"), StatusCode::kBadRequest);
  ExpectError(with_length("0x10"), StatusCode::kBadRequest);
  ExpectError(with_length(""), StatusCode::kBadRequest);
  ExpectError(with_length("99999999999999999999999"),
              StatusCode::kContentTooLarge);
  ExpectError(
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n"
      "Content-Length: 2\r\n\r\n",
      StatusCode::kBadRequest);
}

TEST(RequestParserTest, ErrorIsSticky) {
  RequestParser parser;
  EXPECT_EQ(parser.Feed("BAD\r\n").status, Status::kError);
  EXPECT_EQ(parser.Feed("GET / HTTP/1.1\r\nHost: a\r\n\r\n").status,
            Status::kError);
}

// Small limits keep the boundary tests readable.
constexpr ParserLimits kSmallLimits{
    .max_request_line_bytes = 32,
    .max_header_bytes = 64,
    .max_header_count = 3,
    .max_body_bytes = 10,
};

TEST(RequestParserLimitsTest, RequestLineAtLimitIsAccepted) {
  // "GET " (4) + target (19) + " HTTP/1.1" (9) = 32 bytes.
  const std::string line = "GET /" + std::string(18, 'a') + " HTTP/1.1";
  ASSERT_EQ(line.size(), 32U);
  const Outcome outcome = ParseAll(line + "\r\nHost: a\r\n\r\n", kSmallLimits);
  EXPECT_EQ(outcome.requests.size(), 1U);
}

TEST(RequestParserLimitsTest, RequestLineOverLimitIsRejected) {
  const std::string line = "GET /" + std::string(19, 'a') + " HTTP/1.1";
  ExpectError(line + "\r\nHost: a\r\n\r\n", StatusCode::kUriTooLong,
              kSmallLimits);
}

TEST(RequestParserLimitsTest, UnterminatedRequestLineIsRejectedEarly) {
  // No CRLF ever arrives; the parser must not wait for one.
  ExpectError("GET /" + std::string(40, 'a'), StatusCode::kUriTooLong,
              kSmallLimits);
}

// "Host: a\r\n" (9) + "X: <value>\r\n" (5 + n) + "\r\n" (2) = 16 + n.
std::string RequestWithHeaderValue(size_t n) {
  return "GET / HTTP/1.1\r\nHost: a\r\nX: " + std::string(n, 'v') + "\r\n\r\n";
}

TEST(RequestParserLimitsTest, HeaderBytesAtLimitAreAccepted) {
  const Outcome outcome = ParseAll(RequestWithHeaderValue(48), kSmallLimits);
  EXPECT_EQ(outcome.requests.size(), 1U);
}

TEST(RequestParserLimitsTest, HeaderBytesOverLimitAreRejected) {
  ExpectError(RequestWithHeaderValue(49),
              StatusCode::kRequestHeaderFieldsTooLarge, kSmallLimits);
}

TEST(RequestParserLimitsTest, UnterminatedHeaderIsRejectedEarly) {
  ExpectError("GET / HTTP/1.1\r\nX: " + std::string(100, 'v'),
              StatusCode::kRequestHeaderFieldsTooLarge, kSmallLimits);
}

TEST(RequestParserLimitsTest, HeaderCountLimit) {
  const Outcome at_limit = ParseAll(
      "GET / HTTP/1.1\r\nHost: a\r\nA: 1\r\nB: 2\r\n\r\n", kSmallLimits);
  EXPECT_EQ(at_limit.requests.size(), 1U);
  ExpectError("GET / HTTP/1.1\r\nHost: a\r\nA: 1\r\nB: 2\r\nC: 3\r\n\r\n",
              StatusCode::kRequestHeaderFieldsTooLarge, kSmallLimits);
}

TEST(RequestParserLimitsTest, BodyLimit) {
  const Outcome at_limit = ParseAll(
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\n0123456789",
      kSmallLimits);
  EXPECT_EQ(at_limit.requests.size(), 1U);
  ExpectError("POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 11\r\n\r\n",
              StatusCode::kContentTooLarge, kSmallLimits);
}

}  // namespace
}  // namespace sluice::http
