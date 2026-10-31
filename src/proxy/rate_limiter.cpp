#include "proxy/rate_limiter.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace sluice::proxy {

namespace {

using Seconds = std::chrono::duration<double>;

// Parses a whole string as a positive, finite number.
std::optional<double> ParsePositive(std::string_view text) {
  if (text.empty()) {
    return std::nullopt;
  }
  // std::from_chars for double needs a newer libstdc++ than the toolchain
  // guarantees, so use strtod on a NUL-terminated copy.
  const std::string copy(text);
  char* end = nullptr;
  const double value = std::strtod(copy.c_str(), &end);
  if (end != copy.c_str() + copy.size() || !std::isfinite(value) ||
      value <= 0) {
    return std::nullopt;
  }
  return value;
}

}  // namespace

std::optional<RateLimit> ParseRateLimit(std::string_view text) {
  const size_t colon = text.find(':');
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  const std::optional<double> rate = ParsePositive(text.substr(0, colon));
  const std::optional<double> burst = ParsePositive(text.substr(colon + 1));
  // A burst below one would reject every request.
  if (!rate || !burst || *burst < 1) {
    return std::nullopt;
  }
  return RateLimit{.requests_per_second = *rate, .burst = *burst};
}

double TokenBucket::TokensAt(Clock::time_point now) const noexcept {
  // A clock that went backwards (impossible for steady_clock, but cheap to
  // guard) adds nothing.
  const double elapsed = std::max(0.0, Seconds(now - last_).count());
  return std::min(capacity_, tokens_ + elapsed * rate_);
}

bool TokenBucket::TryTake(Clock::time_point now) noexcept {
  tokens_ = TokensAt(now);
  last_ = std::max(last_, now);
  if (tokens_ < 1.0) {
    return false;
  }
  tokens_ -= 1.0;
  return true;
}

Clock::duration TokenBucket::TimeUntilToken(
    Clock::time_point now) const noexcept {
  const double missing = 1.0 - TokensAt(now);
  if (missing <= 0) {
    return Clock::duration::zero();
  }
  return std::chrono::duration_cast<Clock::duration>(Seconds(missing / rate_));
}

bool TokenBucket::IsFull(Clock::time_point now) const noexcept {
  return TokensAt(now) >= capacity_;
}

RateLimiter::Decision RateLimiter::Allow(uint32_t client_ip,
                                         Clock::time_point now) {
  auto it = buckets_.find(client_ip);
  if (it == buckets_.end()) {
    if (buckets_.size() >= max_clients_) {
      EvictIdle(now);
    }
    if (buckets_.size() >= max_clients_) {
      ++untracked_allows_;
      return {.allowed = true, .retry_after = std::chrono::seconds(0)};
    }
    it = buckets_
             .emplace(client_ip, TokenBucket(limit_.requests_per_second,
                                             limit_.burst, now))
             .first;
  }
  if (it->second.TryTake(now)) {
    return {.allowed = true, .retry_after = std::chrono::seconds(0)};
  }
  const auto wait =
      std::chrono::ceil<std::chrono::seconds>(it->second.TimeUntilToken(now));
  return {.allowed = false,
          .retry_after = std::max(wait, std::chrono::seconds(1))};
}

void RateLimiter::EvictIdle(Clock::time_point now) {
  std::erase_if(buckets_,
                [now](const auto& entry) { return entry.second.IsFull(now); });
}

}  // namespace sluice::proxy
