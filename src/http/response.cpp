#include "http/response.h"

#include <format>

namespace sluice::http {

std::string Serialize(const Response& response, bool keep_alive) {
  std::string out =
      std::format("HTTP/1.1 {} {}\r\n", static_cast<int>(response.status),
                  ReasonPhrase(response.status));
  for (const Headers::Field& field : response.headers.fields()) {
    out += std::format("{}: {}\r\n", field.name, field.value);
  }
  out += std::format("Content-Length: {}\r\n", response.body.size());
  out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
  out += "\r\n";
  out += response.body;
  return out;
}

Response MakeErrorResponse(StatusCode status) {
  Response response;
  response.status = status;
  response.headers.Add("Content-Type", "text/plain; charset=utf-8");
  response.body =
      std::format("{} {}\n", static_cast<int>(status), ReasonPhrase(status));
  return response;
}

}  // namespace sluice::http
