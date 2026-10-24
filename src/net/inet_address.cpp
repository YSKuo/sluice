#include "net/inet_address.h"

#include <arpa/inet.h>
#include <netdb.h>

#include <array>
#include <cerrno>
#include <format>
#include <memory>

namespace sluice::net {

namespace {

sockaddr_in MakeSockaddr(uint32_t ip_host_order, uint16_t port) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(ip_host_order);
  return addr;
}

}  // namespace

std::optional<InetAddress> InetAddress::FromIpPort(std::string_view ip,
                                                   uint16_t port) {
  // inet_pton needs a NUL-terminated string.
  const std::string ip_str(ip);
  sockaddr_in addr = MakeSockaddr(INADDR_ANY, port);
  if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) != 1) {
    return std::nullopt;
  }
  return InetAddress(addr);
}

Result<InetAddress> InetAddress::Resolve(const std::string& host,
                                         uint16_t port) {
  if (std::optional<InetAddress> literal = FromIpPort(host, port)) {
    return *literal;
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* raw = nullptr;
  if (::getaddrinfo(host.c_str(), nullptr, &hints, &raw) != 0 ||
      raw == nullptr) {
    return ErrnoError(EHOSTUNREACH);
  }
  // getaddrinfo allocates the list; freeaddrinfo is its only release.
  const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> list(
      raw, &::freeaddrinfo);
  sockaddr_in addr{};
  // ai_addr points at a sockaddr_in because ai_family is AF_INET.
  addr = *reinterpret_cast<const sockaddr_in*>(list->ai_addr);
  addr.sin_port = htons(port);
  return InetAddress(addr);
}

InetAddress InetAddress::Any(uint16_t port) {
  return InetAddress(MakeSockaddr(INADDR_ANY, port));
}

InetAddress InetAddress::Loopback(uint16_t port) {
  return InetAddress(MakeSockaddr(INADDR_LOOPBACK, port));
}

InetAddress::InetAddress() noexcept : addr_(MakeSockaddr(INADDR_ANY, 0)) {}

uint16_t InetAddress::port() const noexcept { return ntohs(addr_.sin_port); }

std::string InetAddress::ToString() const {
  std::array<char, INET_ADDRSTRLEN> ip{};
  // Cannot fail: the family is always AF_INET and the buffer is large enough.
  ::inet_ntop(AF_INET, &addr_.sin_addr, ip.data(), ip.size());
  return std::format("{}:{}", ip.data(), port());
}

const sockaddr* InetAddress::sockaddr_ptr() const noexcept {
  // sockaddr_in is layout-compatible with sockaddr by POSIX design; this
  // cast is how every socket API expects to receive it.
  return reinterpret_cast<const sockaddr*>(&addr_);
}

}  // namespace sluice::net
