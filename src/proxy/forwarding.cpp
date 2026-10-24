#include "proxy/forwarding.h"

#include <array>
#include <format>
#include <optional>

namespace sluice::proxy {

namespace {

constexpr std::array<std::string_view, 7> kHopByHopFields = {
    "Connection", "Keep-Alive",        "Proxy-Connection", "TE",
    "Trailer",    "Transfer-Encoding", "Upgrade",
};

// The IPv4 address of `addr` without the port.
std::string HostOnly(const net::InetAddress& addr) {
  std::string text = addr.ToString();
  return text.substr(0, text.rfind(':'));
}

}  // namespace

bool IsHopByHop(std::string_view name, const http::Headers& headers) {
  for (std::string_view field : kHopByHopFields) {
    if (http::EqualsIgnoreCase(name, field)) {
      return true;
    }
  }
  for (const http::Headers::Field& field : headers.fields()) {
    if (http::EqualsIgnoreCase(field.name, "Connection") &&
        http::HasToken(field.value, name)) {
      return true;
    }
  }
  return false;
}

bool HasDotSegment(std::string_view target) {
  std::string_view path = target.substr(0, target.find('?'));
  while (!path.empty()) {
    path.remove_prefix(1);  // The leading '/' of this segment.
    const size_t slash = path.find('/');
    const std::string_view segment = path.substr(0, slash);
    if (segment == "." || segment == "..") {
      return true;
    }
    if (slash == std::string_view::npos) {
      break;
    }
    path.remove_prefix(slash);
  }
  return false;
}

std::string BuildUpstreamRequest(const http::Request& request,
                                 const net::InetAddress& client,
                                 std::string_view upstream_host) {
  const http::Headers& in = request.headers;
  std::string out =
      std::format("{} {} HTTP/1.1\r\n", request.method, request.target);

  std::optional<std::string_view> forwarded_for;
  for (const http::Headers::Field& field : in.fields()) {
    if (IsHopByHop(field.name, in) ||
        http::EqualsIgnoreCase(field.name, "Content-Length") ||
        http::EqualsIgnoreCase(field.name, "Expect")) {
      continue;
    }
    if (http::EqualsIgnoreCase(field.name, "X-Forwarded-For")) {
      forwarded_for = field.value;  // Re-added below with the client.
      continue;
    }
    out += std::format("{}: {}\r\n", field.name, field.value);
  }
  if (!in.Get("Host")) {
    out += std::format("Host: {}\r\n", upstream_host);
  }
  const std::string client_ip = HostOnly(client);
  out += forwarded_for ? std::format("X-Forwarded-For: {}, {}\r\n",
                                     *forwarded_for, client_ip)
                       : std::format("X-Forwarded-For: {}\r\n", client_ip);
  if (!request.body.empty() || in.Get("Content-Length")) {
    out += std::format("Content-Length: {}\r\n", request.body.size());
  }
  out += "Connection: close\r\n\r\n";
  out += request.body;
  return out;
}

std::string BuildClientResponseHead(const http::ResponseHead& head,
                                    bool keep_alive) {
  std::string out = std::format("HTTP/1.1 {} {}\r\n", head.status, head.reason);
  for (const http::Headers::Field& field : head.headers.fields()) {
    if (IsHopByHop(field.name, head.headers) ||
        http::EqualsIgnoreCase(field.name, "Content-Length")) {
      continue;
    }
    out += std::format("{}: {}\r\n", field.name, field.value);
  }
  // Already validated by GetBodyLength(): all copies agree, so send one.
  if (const std::optional<std::string_view> length =
          head.headers.Get("Content-Length")) {
    out += std::format("Content-Length: {}\r\n", *length);
  }
  out += keep_alive ? "Connection: keep-alive\r\n\r\n"
                    : "Connection: close\r\n\r\n";
  return out;
}

}  // namespace sluice::proxy
