#ifndef SLUICE_HTTP_SYNTAX_H_
#define SLUICE_HTTP_SYNTAX_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include "http/headers.h"

// Grammar rules shared by RequestParser and ResponseParser (RFC 9110 and
// RFC 9112). Keeping them in one place guarantees that requests from
// clients and responses from upstreams are held to the same strictness.
namespace sluice::http::internal {

// tchar (RFC 9110 5.6.2): the characters of a token, which is what
// methods and field names are made of.
bool IsTokenChar(char c);
bool IsToken(std::string_view s);
// Visible ASCII (0x21-0x7E), e.g. the bytes of a request-target.
bool IsVisibleAscii(char c);
bool IsDigit(char c);
// Allowed in a field value or reason phrase: tab, space, visible ASCII,
// and obs-text (bytes >= 0x80), but no other control characters.
bool IsFieldValueChar(char c);
// Removes leading and trailing spaces and tabs (OWS).
std::string_view TrimOptionalWhitespace(std::string_view s);

// The next CRLF-terminated line at the start of some input.
struct Line {
  enum class Kind : uint8_t {
    kComplete,
    kIncomplete,  // No LF yet; wait for more data.
    kMalformed,   // A bare CR or a bare LF.
  };
  Kind kind;
  std::string_view text;  // Without the CRLF; valid only for kComplete.
  size_t size;            // Including the CRLF; valid only for kComplete.
};
Line NextLine(std::string_view data);

// One "name: value" field line, split and validated: the name must be a
// bare token (no whitespace before the colon, RFC 9112 5.1), the line must
// not be an obs-fold continuation (RFC 9112 5.2), and the value may not
// contain control characters other than tab (RFC 9110 5.5). Returns
// nullopt for any violation. The views point into `line`.
struct FieldLine {
  std::string_view name;
  std::string_view value;
};
std::optional<FieldLine> ParseFieldLine(std::string_view line);

// The message framing given by the Content-Length fields (RFC 9110 8.6).
// Every value must be plain decimal digits and all of them must agree;
// signs ("+5"), lists ("5, 5"), and conflicting values are kInvalid.
struct ContentLength {
  enum class Kind : uint8_t {
    kAbsent,
    kValue,
    kInvalid,
    kOverflow,  // Does not fit in 64 bits.
  };
  Kind kind;
  uint64_t value;  // Valid only for kValue.
};
ContentLength ParseContentLength(const Headers& headers);

}  // namespace sluice::http::internal

#endif  // SLUICE_HTTP_SYNTAX_H_
