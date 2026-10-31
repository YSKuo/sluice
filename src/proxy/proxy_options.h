#ifndef SLUICE_PROXY_PROXY_OPTIONS_H_
#define SLUICE_PROXY_PROXY_OPTIONS_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "http/request_parser.h"
#include "proxy/rate_limiter.h"

namespace sluice::proxy {

// Tunables of the reverse proxy. Every duration is measured with
// steady_clock. The defaults suit the benchmark setup; tests shrink them.
struct ProxyOptions {
  // Limits for parsing client requests and upstream response heads.
  http::ParserLimits limits;

  // A keep-alive connection with no request in progress is closed after
  // this long.
  std::chrono::milliseconds client_idle_timeout{60'000};
  // Once the first byte of a request arrives, the whole request must arrive
  // within this time, or the client gets 408. The deadline is not extended
  // by further bytes, which is what stops slowloris-style clients that
  // trickle one byte at a time.
  std::chrono::milliseconds client_request_timeout{10'000};

  std::chrono::milliseconds upstream_connect_timeout{2'000};
  // Maximum silence from an upstream once connected (see
  // UpstreamCall::Options::idle_timeout).
  std::chrono::milliseconds upstream_idle_timeout{15'000};
  // How long an upstream that failed to connect stays out of rotation.
  std::chrono::milliseconds upstream_down_cooldown{10'000};

  // Backpressure: when this many response bytes are queued for a client,
  // stop reading from the upstream until the client has drained them.
  size_t response_high_water_mark = size_t{256} * 1024;

  // Per-client-IP rate limit; nullopt disables rate limiting.
  std::optional<RateLimit> rate_limit;
  // Bound on tracked client IPs (see RateLimiter).
  size_t rate_limit_max_clients = 100'000;
  // How often buckets that have refilled completely are dropped.
  std::chrono::milliseconds rate_limit_sweep_interval{30'000};
};

// Counters for tests and future observability.
struct ProxyStats {
  uint64_t requests = 0;
  uint64_t upstream_failures = 0;
  // Times an upstream was paused because its client fell behind.
  uint64_t upstream_read_pauses = 0;
  // Requests answered with 429 by the rate limiter.
  uint64_t rate_limited = 0;
};

}  // namespace sluice::proxy

#endif  // SLUICE_PROXY_PROXY_OPTIONS_H_
