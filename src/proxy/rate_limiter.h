#ifndef SLUICE_PROXY_RATE_LIMITER_H_
#define SLUICE_PROXY_RATE_LIMITER_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <unordered_map>

#include "proxy/upstream.h"

namespace sluice::proxy {

// How fast one client may send requests: on average `requests_per_second`,
// with bursts of up to `burst` requests at once.
struct RateLimit {
  double requests_per_second;
  double burst;
};

// Parses "RATE:BURST", e.g. "100:200" (both positive numbers). Returns
// nullopt if the text is malformed.
std::optional<RateLimit> ParseRateLimit(std::string_view text);

// The classic token bucket. It holds up to `capacity` tokens and gains
// `rate` tokens per second; each request takes one token, and a request
// that finds the bucket empty is rejected.
//
// Time is passed in rather than read from a clock, so the arithmetic is
// testable with exact time points. Tokens are refilled lazily: elapsed
// time is converted to tokens only when the bucket is used.
class TokenBucket {
 public:
  // A new bucket starts full, so a new client can burst immediately.
  TokenBucket(double rate, double capacity, Clock::time_point now) noexcept
      : rate_(rate), capacity_(capacity), tokens_(capacity), last_(now) {}

  // Takes one token if available.
  bool TryTake(Clock::time_point now) noexcept;

  // How long until one token is available (zero if one is available now).
  [[nodiscard]] Clock::duration TimeUntilToken(
      Clock::time_point now) const noexcept;

  // Whether the bucket would be full at `now`, i.e. indistinguishable from
  // a new bucket, so forgetting it changes nothing.
  [[nodiscard]] bool IsFull(Clock::time_point now) const noexcept;

 private:
  // Tokens available at `now`, without modifying the bucket.
  [[nodiscard]] double TokensAt(Clock::time_point now) const noexcept;

  double rate_;
  double capacity_;
  double tokens_;
  Clock::time_point last_;
};

// Per-client-IP rate limiting with one TokenBucket per address.
//
// Memory is bounded two ways: EvictIdle() drops buckets that have refilled
// completely (forgetting them changes no decision), and at most
// `max_clients` buckets are kept. When the table is full of active
// clients, new clients are allowed without being tracked (fail open):
// rate limiting protects upstreams from a few greedy clients, not from a
// distributed flood, and failing closed would lock out every new client.
class RateLimiter {
 public:
  struct Decision {
    bool allowed;
    // When rejected: how long the client should wait, rounded up to whole
    // seconds for the Retry-After header (at least 1).
    std::chrono::seconds retry_after;
  };

  RateLimiter(RateLimit limit, size_t max_clients) noexcept
      : limit_(limit), max_clients_(max_clients) {}

  Decision Allow(uint32_t client_ip, Clock::time_point now);

  // Drops every bucket that is full at `now`.
  void EvictIdle(Clock::time_point now);

  [[nodiscard]] size_t tracked_clients() const noexcept {
    return buckets_.size();
  }
  // Requests allowed without tracking because the table was full.
  [[nodiscard]] uint64_t untracked_allows() const noexcept {
    return untracked_allows_;
  }

 private:
  RateLimit limit_;
  size_t max_clients_;
  std::unordered_map<uint32_t, TokenBucket> buckets_;
  uint64_t untracked_allows_ = 0;
};

}  // namespace sluice::proxy

#endif  // SLUICE_PROXY_RATE_LIMITER_H_
