#ifndef SLUICE_HTTP_RESPONSE_H_
#define SLUICE_HTTP_RESPONSE_H_

#include <string>

#include "http/headers.h"
#include "http/status.h"

namespace sluice::http {

// An HTTP response to be serialized. Content-Length and Connection are
// added by Serialize(), so `headers` must not contain them.
struct Response {
  StatusCode status = StatusCode::kOk;
  Headers headers;
  std::string body;
};

// Serializes `response` as HTTP/1.1 wire bytes. Always sends
// Content-Length (sluice never uses chunked encoding), and a Connection
// header telling the client whether the connection stays open.
std::string Serialize(const Response& response, bool keep_alive);

// A plain-text response for a request that could not be served, e.g.
// "400 Bad Request". Error responses always close the connection.
Response MakeErrorResponse(StatusCode status);

}  // namespace sluice::http

#endif  // SLUICE_HTTP_RESPONSE_H_
