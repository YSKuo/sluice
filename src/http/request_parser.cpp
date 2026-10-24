#include "http/request_parser.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <variant>

#include "http/syntax.h"

namespace sluice::http {

namespace {

using internal::ContentLength;
using internal::IsDigit;
using internal::IsToken;
using internal::IsVisibleAscii;
using internal::Line;
using internal::NextLine;
using internal::ParseContentLength;
using internal::ParseFieldLine;

// Parses "HTTP/1.1" or "HTTP/1.0", returning the version or the status to
// fail with: any other well-formed "HTTP/x.y" is a version we do not
// support (505); anything else is malformed (400).
std::variant<Version, StatusCode> ParseVersion(std::string_view text) {
  if (text == "HTTP/1.1") {
    return Version::kHttp11;
  }
  if (text == "HTTP/1.0") {
    return Version::kHttp10;
  }
  const bool well_formed = text.size() == 8 && text.starts_with("HTTP/") &&
                           IsDigit(text[5]) && text[6] == '.' &&
                           IsDigit(text[7]);
  return well_formed ? StatusCode::kHttpVersionNotSupported
                     : StatusCode::kBadRequest;
}

}  // namespace

RequestParser::FeedResult RequestParser::Feed(std::string_view data) {
  size_t pos = 0;
  while (true) {
    const std::string_view rest = data.substr(pos);
    switch (state_) {
      case State::kRequestLine:
      case State::kHeaders: {
        const Line line = NextLine(rest);
        if (line.kind == Line::Kind::kMalformed) {
          return Fail(StatusCode::kBadRequest, pos);
        }
        if (line.kind == Line::Kind::kIncomplete) {
          if (const auto error = CheckIncompleteLine(rest)) {
            return Fail(*error, pos);
          }
          return {.status = Status::kNeedMore, .consumed = pos};
        }
        pos += line.size;
        if (state_ == State::kHeaders) {
          header_bytes_ += line.size;
        }
        const std::optional<StatusCode> error =
            state_ == State::kRequestLine ? HandleRequestLine(line.text)
                                          : HandleHeaderLine(line.text);
        if (error) {
          return Fail(*error, pos);
        }
        break;
      }
      case State::kBody: {
        const size_t n = std::min(rest.size(), body_remaining_);
        request_.body.append(rest.substr(0, n));
        pos += n;
        body_remaining_ -= n;
        if (body_remaining_ > 0) {
          return {.status = Status::kNeedMore, .consumed = pos};
        }
        state_ = State::kComplete;
        break;
      }
      case State::kComplete:
        return {.status = Status::kComplete, .consumed = pos};
      case State::kError:
        return {.status = Status::kError, .consumed = pos};
    }
  }
}

Request RequestParser::TakeRequest() {
  Request request = std::move(request_);
  request_ = Request();
  state_ = State::kRequestLine;
  header_bytes_ = 0;
  body_remaining_ = 0;
  return request;
}

std::optional<StatusCode> RequestParser::HandleRequestLine(
    std::string_view line) {
  // RFC 9112 section 2.2: ignore empty lines before the request line (some
  // clients send an extra CRLF after a request body).
  if (line.empty()) {
    return std::nullopt;
  }
  if (line.size() > limits_.max_request_line_bytes) {
    return StatusCode::kUriTooLong;
  }

  // method SP request-target SP HTTP-version, with exactly two spaces.
  const size_t sp1 = line.find(' ');
  if (sp1 == std::string_view::npos) {
    return StatusCode::kBadRequest;
  }
  const size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos ||
      line.find(' ', sp2 + 1) != std::string_view::npos) {
    return StatusCode::kBadRequest;
  }
  const std::string_view method = line.substr(0, sp1);
  const std::string_view target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const std::string_view version = line.substr(sp2 + 1);

  if (!IsToken(method)) {
    return StatusCode::kBadRequest;
  }
  // Only origin-form ("/path?query") is accepted: sluice is a gateway, not
  // a forward proxy, so absolute-form and authority-form make no sense.
  if (target.empty() || target.front() != '/' ||
      !std::ranges::all_of(target, IsVisibleAscii)) {
    return StatusCode::kBadRequest;
  }
  const std::variant<Version, StatusCode> parsed = ParseVersion(version);
  if (const auto* error = std::get_if<StatusCode>(&parsed)) {
    return *error;
  }

  request_.version = std::get<Version>(parsed);
  request_.method = std::string(method);
  request_.target = std::string(target);
  state_ = State::kHeaders;
  return std::nullopt;
}

std::optional<StatusCode> RequestParser::HandleHeaderLine(
    std::string_view line) {
  if (header_bytes_ > limits_.max_header_bytes) {
    return StatusCode::kRequestHeaderFieldsTooLarge;
  }
  if (line.empty()) {
    return FinishHeaders();
  }
  // Rejects obs-fold, whitespace before the colon, and control characters
  // in the value (see internal::ParseFieldLine).
  const std::optional<internal::FieldLine> field = ParseFieldLine(line);
  if (!field) {
    return StatusCode::kBadRequest;
  }
  if (request_.headers.size() >= limits_.max_header_count) {
    return StatusCode::kRequestHeaderFieldsTooLarge;
  }
  request_.headers.Add(field->name, field->value);
  return std::nullopt;
}

std::optional<StatusCode> RequestParser::FinishHeaders() {
  const Headers& headers = request_.headers;

  // Chunked (or any other transfer coding) is out of scope. Rejecting it
  // outright also rules out the Transfer-Encoding + Content-Length
  // combination behind classic request smuggling.
  if (headers.Count("Transfer-Encoding") > 0) {
    return StatusCode::kNotImplemented;
  }

  const size_t host_count = headers.Count("Host");
  if (host_count > 1 ||
      (request_.version == Version::kHttp11 && host_count == 0)) {
    return StatusCode::kBadRequest;
  }

  // Every Content-Length field must be plain digits, and all must agree.
  const ContentLength length = ParseContentLength(headers);
  if (length.kind == ContentLength::Kind::kInvalid) {
    return StatusCode::kBadRequest;
  }
  if (length.kind == ContentLength::Kind::kOverflow) {
    return StatusCode::kContentTooLarge;
  }
  const uint64_t body_length =
      length.kind == ContentLength::Kind::kValue ? length.value : 0;
  if (body_length > limits_.max_body_bytes) {
    return StatusCode::kContentTooLarge;
  }
  body_remaining_ = static_cast<size_t>(body_length);
  request_.body.reserve(body_remaining_);
  state_ = body_remaining_ > 0 ? State::kBody : State::kComplete;
  return std::nullopt;
}

std::optional<StatusCode> RequestParser::CheckIncompleteLine(
    std::string_view pending) const {
  // A trailing CR may be the first half of the CRLF, so it does not count
  // toward the line's content.
  const size_t content =
      pending.ends_with('\r') ? pending.size() - 1 : pending.size();
  if (state_ == State::kRequestLine) {
    return content > limits_.max_request_line_bytes
               ? std::optional(StatusCode::kUriTooLong)
               : std::nullopt;
  }
  return header_bytes_ + content > limits_.max_header_bytes
             ? std::optional(StatusCode::kRequestHeaderFieldsTooLarge)
             : std::nullopt;
}

RequestParser::FeedResult RequestParser::Fail(StatusCode status,
                                              size_t consumed) {
  state_ = State::kError;
  error_ = status;
  return {.status = Status::kError, .consumed = consumed};
}

}  // namespace sluice::http
