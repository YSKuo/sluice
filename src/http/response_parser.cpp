#include "http/response_parser.h"

#include <algorithm>
#include <utility>

#include "http/syntax.h"

namespace sluice::http {

namespace {

using internal::ContentLength;
using internal::IsDigit;
using internal::Line;
using internal::NextLine;

}  // namespace

ResponseParser::FeedResult ResponseParser::Feed(std::string_view data) {
  size_t pos = 0;
  while (state_ == State::kStatusLine || state_ == State::kHeaders) {
    const std::string_view rest = data.substr(pos);
    const Line line = NextLine(rest);
    if (line.kind == Line::Kind::kMalformed) {
      return Fail(pos);
    }
    if (line.kind == Line::Kind::kIncomplete) {
      const size_t limit = state_ == State::kStatusLine
                               ? limits_.max_request_line_bytes
                               : limits_.max_header_bytes - header_bytes_;
      if (rest.size() > limit + 1) {
        return Fail(pos);
      }
      return {.status = Status::kNeedMore, .consumed = pos};
    }
    pos += line.size;
    const bool ok = state_ == State::kStatusLine ? HandleStatusLine(line.text)
                                                 : HandleHeaderLine(line.text);
    if (!ok) {
      return Fail(pos);
    }
    // HandleHeaderLine() moved to kComplete on the empty line.
  }
  return {
      .status = state_ == State::kComplete ? Status::kComplete : Status::kError,
      .consumed = pos};
}

ResponseHead ResponseParser::TakeHead() {
  ResponseHead head = std::move(head_);
  head_ = ResponseHead();
  state_ = State::kStatusLine;
  header_bytes_ = 0;
  return head;
}

bool ResponseParser::HandleStatusLine(std::string_view line) {
  // HTTP-version SP 3DIGIT SP reason-phrase, e.g. "HTTP/1.1 404 Not Found".
  // The reason may be empty; a missing space after the code is tolerated.
  if (line.size() > limits_.max_request_line_bytes || line.size() < 12 ||
      line[8] != ' ' || (line.size() > 12 && line[12] != ' ')) {
    return false;
  }
  const std::string_view version = line.substr(0, 8);
  if (version == "HTTP/1.1") {
    head_.version = Version::kHttp11;
  } else if (version == "HTTP/1.0") {
    head_.version = Version::kHttp10;
  } else {
    return false;
  }
  const std::string_view code = line.substr(9, 3);
  if (!std::ranges::all_of(code, IsDigit) || code[0] < '1' || code[0] > '5') {
    return false;
  }
  head_.status = (code[0] - '0') * 100 + (code[1] - '0') * 10 + (code[2] - '0');
  // The reason is forwarded to the client, so it must not smuggle control
  // characters.
  const std::string_view reason =
      line.size() > 13 ? line.substr(13) : std::string_view();
  if (!std::ranges::all_of(reason, internal::IsFieldValueChar)) {
    return false;
  }
  head_.reason = std::string(reason);
  state_ = State::kHeaders;
  return true;
}

bool ResponseParser::HandleHeaderLine(std::string_view line) {
  header_bytes_ += line.size() + 2;
  if (header_bytes_ > limits_.max_header_bytes) {
    return false;
  }
  if (line.empty()) {
    if (head_.headers.Count("Transfer-Encoding") > 0) {
      return false;  // Chunked responses are out of scope.
    }
    state_ = State::kComplete;
    return true;
  }
  const std::optional<internal::FieldLine> field =
      internal::ParseFieldLine(line);
  if (!field || head_.headers.size() >= limits_.max_header_count) {
    return false;
  }
  head_.headers.Add(field->name, field->value);
  return true;
}

ResponseParser::FeedResult ResponseParser::Fail(size_t consumed) {
  state_ = State::kError;
  return {.status = Status::kError, .consumed = consumed};
}

std::optional<BodyLength> GetBodyLength(const ResponseHead& head,
                                        std::string_view request_method) {
  if (request_method == "HEAD" || head.status < 200 || head.status == 204 ||
      head.status == 304) {
    return BodyLength{.kind = BodyLength::Kind::kLength, .length = 0};
  }
  const ContentLength length = internal::ParseContentLength(head.headers);
  switch (length.kind) {
    case ContentLength::Kind::kValue:
      return BodyLength{.kind = BodyLength::Kind::kLength,
                        .length = length.value};
    case ContentLength::Kind::kAbsent:
      return BodyLength{.kind = BodyLength::Kind::kUntilClose, .length = 0};
    case ContentLength::Kind::kInvalid:
    case ContentLength::Kind::kOverflow:
      return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace sluice::http
