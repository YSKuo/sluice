#include "net/inet_address.h"

#include <arpa/inet.h>

#include <array>

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

InetAddress::InetAddress() noexcept : addr_(MakeSockaddr(INADDR_ANY, 0)) {}

std::optional<InetAddress> InetAddress::FromIpPort(std::string_view ip, uint16_t port) {
  // inet_pton needs a NUL-terminated string.
  const std::string ip_str(ip);
  sockaddr_in addr = MakeSockaddr(INADDR_ANY, port);
  if (::inet_pton(AF_INET, ip_str.c_str(), &addr.sin_addr) != 1) {
    return std::nullopt;
  }
  return InetAddress(addr);
}

InetAddress InetAddress::Any(uint16_t port) { return InetAddress(MakeSockaddr(INADDR_ANY, port)); }

InetAddress InetAddress::Loopback(uint16_t port) {
  return InetAddress(MakeSockaddr(INADDR_LOOPBACK, port));
}

uint16_t InetAddress::port() const noexcept { return ntohs(addr_.sin_port); }

std::string InetAddress::ToString() const {
  std::array<char, INET_ADDRSTRLEN> ip{};
  // Cannot fail: the family is always AF_INET and the buffer is large enough.
  ::inet_ntop(AF_INET, &addr_.sin_addr, ip.data(), ip.size());
  return std::string(ip.data()) + ":" + std::to_string(port());
}

const sockaddr* InetAddress::sockaddr_ptr() const noexcept {
  // sockaddr_in is layout-compatible with sockaddr by POSIX design; this
  // cast is how every socket API expects to receive it.
  return reinterpret_cast<const sockaddr*>(&addr_);
}

}  // namespace sluice::net
