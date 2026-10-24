#ifndef SLUICE_PROXY_UPSTREAM_H_
#define SLUICE_PROXY_UPSTREAM_H_

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "net/inet_address.h"

namespace sluice::proxy {

using Clock = std::chrono::steady_clock;

// One backend server, with the passive health state the gateway keeps for
// it. "Passive" means the gateway never probes it (active health checking
// is out of scope): it only reacts when a real request fails to connect.
class Upstream {
 public:
  // `name` is what the client's request is labelled with upstream (the Host
  // header for HTTP/1.0 clients) and what logs show, e.g. "backend-a:9000".
  Upstream(std::string name, const net::InetAddress& address)
      : name_(std::move(name)), address_(address) {}

  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] const net::InetAddress& address() const noexcept {
    return address_;
  }

  [[nodiscard]] bool IsUp(Clock::time_point now) const noexcept {
    return now >= down_until_;
  }
  // Takes the upstream out of rotation until `now + cooldown`, after which
  // it is tried again by the next request that reaches it.
  void MarkDown(Clock::time_point now, Clock::duration cooldown) noexcept {
    down_until_ = now + cooldown;
  }

 private:
  std::string name_;
  net::InetAddress address_;
  Clock::time_point down_until_{};
};

// Round robin over a fixed list of upstreams, skipping any that are marked
// down. The upstreams are owned elsewhere (RouteTable) and outlive this.
class RoundRobin {
 public:
  explicit RoundRobin(std::vector<Upstream*> upstreams)
      : upstreams_(std::move(upstreams)) {}

  // The next upstream that is up, or nullptr if every one is down. Each
  // call advances the rotation, so consecutive requests spread out.
  Upstream* Pick(Clock::time_point now);

  [[nodiscard]] const std::vector<Upstream*>& upstreams() const noexcept {
    return upstreams_;
  }

 private:
  std::vector<Upstream*> upstreams_;
  size_t next_ = 0;
};

}  // namespace sluice::proxy

#endif  // SLUICE_PROXY_UPSTREAM_H_
