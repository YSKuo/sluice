#ifndef SLUICE_HTTP_REQUEST_H_
#define SLUICE_HTTP_REQUEST_H_

#include <cstdint>
#include <string>

#include "http/headers.h"

namespace sluice::http {

enum class Version : uint8_t {
  kHttp10,
  kHttp11,
};

// A parsed HTTP request. Every string is owned (copied out of the
// connection's input buffer), so a Request stays valid after the buffer
// is compacted or reused for the next request.
struct Request {
  std::string method;
  // The request-target in origin-form, e.g. "/api/users?id=7".
  std::string target;
  Version version = Version::kHttp11;
  Headers headers;
  std::string body;
};

// Whether the connection should stay open after responding to `request`
// (RFC 9112 section 9.3): HTTP/1.1 stays open unless the client sent
// "Connection: close"; HTTP/1.0 closes unless it sent
// "Connection: keep-alive".
bool IsKeepAlive(const Request& request);

}  // namespace sluice::http

#endif  // SLUICE_HTTP_REQUEST_H_
