#ifndef SLUICE_HTTP_RESPONSE_PARSER_H_
#define SLUICE_HTTP_RESPONSE_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "http/headers.h"
#include "http/request.h"
#include "http/request_parser.h"

namespace sluice::http {

// The status line and header fields of a response from an upstream.
struct ResponseHead {
  Version version = Version::kHttp11;
  int status = 0;
  std::string reason;
  Headers headers;
};

// Incremental parser for the head of an HTTP/1.x response (RFC 9112
// section 4): status line, header fields, empty line. It stops there; the
// body is streamed by the proxy, which uses BodyLength() to know where it
// ends.
//
// Applies the same strictness as RequestParser (shared rules in
// http/syntax.h). Any violation, including a Transfer-Encoding header
// (chunked is out of scope) or an oversized head, is an error; the proxy
// answers the client with 502 Bad Gateway.
class ResponseParser {
 public:
  enum class Status : uint8_t {
    kNeedMore,
    kComplete,  // The head is complete; call TakeHead().
    kError,
  };

  struct FeedResult {
    Status status;
    // Bytes consumed. On kComplete, the remaining bytes are the body.
    size_t consumed;
  };

  // Uses max_request_line_bytes as the status line limit, and the header
  // limits as they are. The body limit does not apply to responses.
  explicit ResponseParser(ParserLimits limits = {}) : limits_(limits) {}

  // Same contract as RequestParser::Feed().
  FeedResult Feed(std::string_view data);

  // Valid after kComplete.
  ResponseHead TakeHead();

 private:
  enum class State : uint8_t {
    kStatusLine,
    kHeaders,
    kComplete,
    kError,
  };

  // Each returns false if the line is invalid.
  bool HandleStatusLine(std::string_view line);
  bool HandleHeaderLine(std::string_view line);
  FeedResult Fail(size_t consumed);

  ParserLimits limits_;
  State state_ = State::kStatusLine;
  ResponseHead head_;
  size_t header_bytes_ = 0;
};

// How the body of a response is delimited (RFC 9112 section 6.3).
struct BodyLength {
  enum class Kind : uint8_t {
    kLength,     // Exactly `length` bytes follow (possibly zero).
    kUntilClose  // No Content-Length: the body ends when the upstream
                 // closes the connection.
  };
  Kind kind;
  uint64_t length;  // Valid only for kLength.
};

// Determines the body framing of `head`, received in reply to a request
// with `request_method`. HEAD requests and 1xx/204/304 responses never have
// a body, whatever their headers say. Returns nullopt if the framing is
// invalid (conflicting or malformed Content-Length).
std::optional<BodyLength> GetBodyLength(const ResponseHead& head,
                                        std::string_view request_method);

}  // namespace sluice::http

#endif  // SLUICE_HTTP_RESPONSE_PARSER_H_
