#ifndef SLUICE_HTTP_REQUEST_PARSER_H_
#define SLUICE_HTTP_REQUEST_PARSER_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "http/request.h"
#include "http/status.h"

namespace sluice::http {

// Size limits enforced while parsing. They are a security boundary: each
// one is checked as soon as it can be exceeded, including while a line is
// still incomplete, so a client can never make the parser buffer more than
// these amounts.
struct ParserLimits {
  // Request line, excluding CRLF. Exceeding it yields 414.
  size_t max_request_line_bytes = size_t{8} * 1024;
  // All header lines together, including CRLFs and the final empty line.
  // Exceeding it yields 431.
  size_t max_header_bytes = size_t{16} * 1024;
  // Number of header fields. Exceeding it yields 431.
  size_t max_header_count = 100;
  // Content-Length. Exceeding it yields 413.
  size_t max_body_bytes = size_t{1024} * 1024;
};

// Incremental HTTP/1.1 request parser (RFC 9112), driven by whatever bytes
// have arrived so far:
//
//   kRequestLine --CRLF--> kHeaders --empty line--> kBody --> kComplete
//          \                   \                      \
//           +-------------------+----- violation -----+--> kError
//
// Deliberately strict, because Week 3 forwards requests upstream and any
// leniency the upstream does not share is a request-smuggling vector:
// - lines must end in CRLF; a bare CR or bare LF is rejected;
// - no whitespace between a field name and its colon, no obs-fold;
// - bodies are framed only by Content-Length; any Transfer-Encoding is
//   rejected (501), as are conflicting Content-Length values (400);
// - HTTP/1.1 requests must carry exactly one Host field.
class RequestParser {
 public:
  enum class Status : uint8_t {
    kNeedMore,  // Consumed what it could; call again with more data.
    kComplete,  // A full request is ready; call TakeRequest().
    kError,     // The request is invalid; see error_status().
  };

  struct FeedResult {
    Status status;
    // Bytes of the input that were consumed. The caller must drop them
    // from its buffer and pass the remaining bytes again on the next call.
    size_t consumed;
  };

  explicit RequestParser(ParserLimits limits = {}) : limits_(limits) {}

  // Parses as much of `data` as possible. `data` must start at the first
  // byte the previous call did not consume. An incomplete line is left
  // unconsumed until its CRLF arrives. On kComplete, any bytes after the
  // request (the start of a pipelined request) are left unconsumed.
  FeedResult Feed(std::string_view data);

  // Valid after kComplete: moves the request out and resets the parser for
  // the next request on the same connection.
  Request TakeRequest();

  // Valid after kError: the status to respond with.
  [[nodiscard]] StatusCode error_status() const noexcept { return error_; }

 private:
  enum class State : uint8_t {
    kRequestLine,
    kHeaders,
    kBody,
    kComplete,
    kError,
  };

  // Each Handle* function returns the error status, or nullopt if the line
  // was valid.
  std::optional<StatusCode> HandleRequestLine(std::string_view line);
  std::optional<StatusCode> HandleHeaderLine(std::string_view line);
  // Validates message framing once all headers are in, and decides whether
  // a body follows.
  std::optional<StatusCode> FinishHeaders();
  // Checks the limits that an incomplete line (no LF yet) may already
  // exceed.
  std::optional<StatusCode> CheckIncompleteLine(std::string_view pending) const;
  FeedResult Fail(StatusCode status, size_t consumed);

  ParserLimits limits_;
  State state_ = State::kRequestLine;
  Request request_;
  size_t header_bytes_ = 0;
  size_t body_remaining_ = 0;
  StatusCode error_ = StatusCode::kBadRequest;
};

}  // namespace sluice::http

#endif  // SLUICE_HTTP_REQUEST_PARSER_H_
