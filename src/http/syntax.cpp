#include "http/syntax.h"

#include <algorithm>
#include <charconv>
#include <system_error>

namespace sluice::http::internal {

bool IsTokenChar(char c) {
  if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c)) {
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

bool IsVisibleAscii(char c) { return c > 0x20 && c < 0x7F; }

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

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

std::optional<FieldLine> ParseFieldLine(std::string_view line) {
  if (line.empty() || line.front() == ' ' || line.front() == '\t') {
    return std::nullopt;
  }
  const size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view name = line.substr(0, colon);
  if (!IsToken(name)) {
    return std::nullopt;
  }
  const std::string_view value = TrimOptionalWhitespace(line.substr(colon + 1));
  if (!std::ranges::all_of(value, IsFieldValueChar)) {
    return std::nullopt;
  }
  return FieldLine{.name = name, .value = value};
}

ContentLength ParseContentLength(const Headers& headers) {
  ContentLength result{.kind = ContentLength::Kind::kAbsent, .value = 0};
  for (const Headers::Field& field : headers.fields()) {
    if (!EqualsIgnoreCase(field.name, "Content-Length")) {
      continue;
    }
    const std::string_view text = field.value;
    if (text.empty() || !std::ranges::all_of(text, IsDigit)) {
      return {.kind = ContentLength::Kind::kInvalid, .value = 0};
    }
    uint64_t value = 0;
    const char* const last = text.data() + text.size();
    const auto [ptr, ec] = std::from_chars(text.data(), last, value);
    if (ec == std::errc::result_out_of_range) {
      return {.kind = ContentLength::Kind::kOverflow, .value = 0};
    }
    if (ec != std::errc() || ptr != last) {
      return {.kind = ContentLength::Kind::kInvalid, .value = 0};
    }
    if (result.kind == ContentLength::Kind::kValue && result.value != value) {
      return {.kind = ContentLength::Kind::kInvalid, .value = 0};
    }
    result = {.kind = ContentLength::Kind::kValue, .value = value};
  }
  return result;
}

}  // namespace sluice::http::internal
