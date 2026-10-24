#ifndef SLUICE_NET_INET_ADDRESS_H_
#define SLUICE_NET_INET_ADDRESS_H_

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "result.h"

namespace sluice::net {

// An IPv4 address + port, stored as the sockaddr_in the kernel expects.
// A plain value type: cheap to copy, owns no resources.
class InetAddress {
 public:
  // Parses a dotted-quad IPv4 address such as "127.0.0.1". Returns nullopt
  // if `ip` is not a valid IPv4 literal (host names are not resolved).
  static std::optional<InetAddress> FromIpPort(std::string_view ip,
                                               uint16_t port);
  // Resolves `host` (a name such as "backend-a" or an IPv4 literal) to its
  // first IPv4 address. Blocks on DNS, so call it only at startup, never on
  // the event loop. Fails with EHOSTUNREACH if the name does not resolve.
  static Result<InetAddress> Resolve(const std::string& host, uint16_t port);
  // 0.0.0.0:<port>, i.e. every local interface.
  static InetAddress Any(uint16_t port);
  // 127.0.0.1:<port>
  static InetAddress Loopback(uint16_t port);

  // 0.0.0.0:0
  InetAddress() noexcept;
  explicit InetAddress(const sockaddr_in& addr) noexcept : addr_(addr) {}

  // Port in host byte order.
  [[nodiscard]] uint16_t port() const noexcept;
  // "a.b.c.d:port"
  [[nodiscard]] std::string ToString() const;

  [[nodiscard]] const sockaddr* sockaddr_ptr() const noexcept;
  [[nodiscard]] static socklen_t length() noexcept {
    return sizeof(sockaddr_in);
  }

 private:
  sockaddr_in addr_;
};

}  // namespace sluice::net

#endif  // SLUICE_NET_INET_ADDRESS_H_
