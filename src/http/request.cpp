#include "http/request.h"

#include <optional>
#include <string_view>

namespace sluice::http {

bool IsKeepAlive(const Request& request) {
  const std::optional<std::string_view> connection =
      request.headers.Get("Connection");
  if (connection && HasToken(*connection, "close")) {
    return false;
  }
  if (request.version == Version::kHttp10) {
    return connection && HasToken(*connection, "keep-alive");
  }
  return true;
}

}  // namespace sluice::http
