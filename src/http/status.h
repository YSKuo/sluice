#ifndef SLUICE_HTTP_STATUS_H_
#define SLUICE_HTTP_STATUS_H_

#include <cstdint>
#include <string_view>

namespace sluice::http {

// The HTTP status codes sluice produces. Values are the wire codes.
enum class StatusCode : uint16_t {
  kOk = 200,
  kBadRequest = 400,
  kNotFound = 404,
  kContentTooLarge = 413,
  kUriTooLong = 414,
  kRequestHeaderFieldsTooLarge = 431,
  kInternalServerError = 500,
  kNotImplemented = 501,
  kHttpVersionNotSupported = 505,
};

// The standard reason phrase, e.g. "Bad Request" (RFC 9110 section 15).
std::string_view ReasonPhrase(StatusCode status);

}  // namespace sluice::http

#endif  // SLUICE_HTTP_STATUS_H_
