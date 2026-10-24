#include "http/status.h"

namespace sluice::http {

std::string_view ReasonPhrase(StatusCode status) {
  switch (status) {
    case StatusCode::kOk:
      return "OK";
    case StatusCode::kBadRequest:
      return "Bad Request";
    case StatusCode::kNotFound:
      return "Not Found";
    case StatusCode::kRequestTimeout:
      return "Request Timeout";
    case StatusCode::kContentTooLarge:
      return "Content Too Large";
    case StatusCode::kUriTooLong:
      return "URI Too Long";
    case StatusCode::kRequestHeaderFieldsTooLarge:
      return "Request Header Fields Too Large";
    case StatusCode::kInternalServerError:
      return "Internal Server Error";
    case StatusCode::kNotImplemented:
      return "Not Implemented";
    case StatusCode::kBadGateway:
      return "Bad Gateway";
    case StatusCode::kServiceUnavailable:
      return "Service Unavailable";
    case StatusCode::kGatewayTimeout:
      return "Gateway Timeout";
    case StatusCode::kHttpVersionNotSupported:
      return "HTTP Version Not Supported";
  }
  return "Unknown";
}

}  // namespace sluice::http
