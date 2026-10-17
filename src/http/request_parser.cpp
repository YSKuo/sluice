#include "http/request_parser.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <system_error>
#include <utility>
#include <variant>

namespace sluice::http {

namespace {

// tchar from RFC 9110 section 5.6.2: the characters allowed in a token,
// which is what methods and field names are made of.
bool IsTokenChar(char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
      (c >= '0' && c <= '9')) {
    return true;
  }
  switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
      return true;
    default:
      return false;
  }
}

bool IsToken(std::string_view s) {
  return !s.empty() && std::ranges::all_of(s, IsTokenChar);
}

// Visible ASCII, the only bytes allowed in an origin-form request-target.
bool IsVisibleAscii(char c) { return c > 0x20 && c < 0x7F; }

// Field values may contain visible characters, spaces, tabs, and obs-text
// (bytes >= 0x80), but no other control characters (RFC 9110 5.5).
bool IsFieldValueChar(char c) {
  const auto byte = static_cast<unsigned char>(c);
  return byte == '\t' || byte == ' ' || (byte > 0x20 && byte != 0x7F);
}

std::string_view TrimOptionalWhitespace(std::string_view s) {
  const size_t first = s.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  const size_t last = s.find_last_not_of(" \t");
  return s.substr(first, last - first + 1);
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// Outcome of looking for the next CRLF-terminated line.
struct Line {
  enum class Kind : uint8_t {
    kComplete,
    kIncomplete,  // No LF yet; wait for more data.
    kMalformed,   // A bare CR or bare LF.
  };
  Kind kind;
  std::string_view text;  // Without the CRLF; valid only for kComplete.
  size_t size;            // Including the CRLF; valid only for kComplete.
};

Line NextLine(std::string_view data) {
  const size_t lf = data.find('\n');
  if (lf == std::string_view::npos) {
    // A CR is allowed only as the very last byte, where its LF may still
    // be on the way.
    const size_t cr = data.find('\r');
    if (cr != std::string_view::npos && cr + 1 < data.size()) {
      return {.kind = Line::Kind::kMalformed, .text = {}, .size = 0};
    }
    return {.kind = Line::Kind::kIncomplete, .text = {}, .size = 0};
  }
  if (lf == 0 || data[lf - 1] != '\r') {
    return {.kind = Line::Kind::kMalformed, .text = {}, .size = 0};
  }
  const std::string_view text = data.substr(0, lf - 1);
  if (text.find('\r') != std::string_view::npos) {
    return {.kind = Line::Kind::kMalformed, .text = {}, .size = 0};
  }
  return {.kind = Line::Kind::kComplete, .text = text, .size = lf + 1};
}

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
  // obs-fold: a continuation line starting with whitespace (RFC 9112 5.2).
  if (line.front() == ' ' || line.front() == '\t') {
    return StatusCode::kBadRequest;
  }
  const size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    return StatusCode::kBadRequest;
  }
  // The name must be a bare token: whitespace before the colon is
  // rejected, as RFC 9112 section 5.1 requires.
  const std::string_view name = line.substr(0, colon);
  if (!IsToken(name)) {
    return StatusCode::kBadRequest;
  }
  const std::string_view value = TrimOptionalWhitespace(line.substr(colon + 1));
  if (!std::ranges::all_of(value, IsFieldValueChar)) {
    return StatusCode::kBadRequest;
  }
  if (request_.headers.size() >= limits_.max_header_count) {
    return StatusCode::kRequestHeaderFieldsTooLarge;
  }
  request_.headers.Add(name, value);
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

  // Every Content-Length field must be a plain decimal number, and all of
  // them must agree. Signs ("+5"), lists ("5, 5"), and values that overflow
  // 64 bits are rejected.
  std::optional<uint64_t> length;
  for (const Headers::Field& field : headers.fields()) {
    if (!EqualsIgnoreCase(field.name, "Content-Length")) {
      continue;
    }
    const std::string_view text = field.value;
    if (text.empty() || !std::ranges::all_of(text, IsDigit)) {
      return StatusCode::kBadRequest;
    }
    uint64_t value = 0;
    const auto [ptr, ec] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec == std::errc::result_out_of_range) {
      return StatusCode::kContentTooLarge;
    }
    if (ec != std::errc() || ptr != text.data() + text.size()) {
      return StatusCode::kBadRequest;
    }
    if (length && *length != value) {
      return StatusCode::kBadRequest;
    }
    length = value;
  }

  const uint64_t body_length = length.value_or(0);
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
