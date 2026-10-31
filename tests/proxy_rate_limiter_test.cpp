#include <chrono>
#include <optional>

#include "gtest/gtest.h"

#include "proxy/rate_limiter.h"

namespace sluice::proxy {
namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

// A fixed origin, so every test works with exact time points.
const Clock::time_point kT0 = Clock::time_point() + seconds(1000);

TEST(TokenBucketTest, StartsFullAndAllowsABurst) {
  TokenBucket bucket(/*rate=*/1.0, /*capacity=*/3.0, kT0);
  EXPECT_TRUE(bucket.TryTake(kT0));
  EXPECT_TRUE(bucket.TryTake(kT0));
  EXPECT_TRUE(bucket.TryTake(kT0));
  EXPECT_FALSE(bucket.TryTake(kT0));
}

TEST(TokenBucketTest, RefillsAtTheConfiguredRate) {
  TokenBucket bucket(/*rate=*/2.0, /*capacity=*/1.0, kT0);
  ASSERT_TRUE(bucket.TryTake(kT0));
  // Two tokens per second: one token after 500 ms, not before.
  EXPECT_FALSE(bucket.TryTake(kT0 + milliseconds(499)));
  EXPECT_TRUE(bucket.TryTake(kT0 + milliseconds(500)));
  EXPECT_FALSE(bucket.TryTake(kT0 + milliseconds(500)));
}

TEST(TokenBucketTest, NeverExceedsCapacity) {
  TokenBucket bucket(/*rate=*/100.0, /*capacity=*/2.0, kT0);
  // A long idle period refills to capacity, not beyond.
  const Clock::time_point later = kT0 + seconds(60);
  EXPECT_TRUE(bucket.TryTake(later));
  EXPECT_TRUE(bucket.TryTake(later));
  EXPECT_FALSE(bucket.TryTake(later));
}

TEST(TokenBucketTest, ReportsTimeUntilNextToken) {
  TokenBucket bucket(/*rate=*/4.0, /*capacity=*/1.0, kT0);
  EXPECT_EQ(bucket.TimeUntilToken(kT0), Clock::duration::zero());
  ASSERT_TRUE(bucket.TryTake(kT0));
  EXPECT_EQ(std::chrono::round<milliseconds>(bucket.TimeUntilToken(kT0)),
            milliseconds(250));
  EXPECT_EQ(std::chrono::round<milliseconds>(
                bucket.TimeUntilToken(kT0 + milliseconds(100))),
            milliseconds(150));
}

TEST(TokenBucketTest, KnowsWhenItIsFull) {
  TokenBucket bucket(/*rate=*/1.0, /*capacity=*/2.0, kT0);
  EXPECT_TRUE(bucket.IsFull(kT0));
  ASSERT_TRUE(bucket.TryTake(kT0));
  EXPECT_FALSE(bucket.IsFull(kT0 + milliseconds(999)));
  EXPECT_TRUE(bucket.IsFull(kT0 + seconds(1)));
}

TEST(RateLimiterTest, LimitsEachClientIndependently) {
  RateLimiter limiter({.requests_per_second = 1, .burst = 2},
                      /*max_clients=*/10);
  EXPECT_TRUE(limiter.Allow(1, kT0).allowed);
  EXPECT_TRUE(limiter.Allow(1, kT0).allowed);
  const RateLimiter::Decision third = limiter.Allow(1, kT0);
  EXPECT_FALSE(third.allowed);
  EXPECT_EQ(third.retry_after, seconds(1));
  // Another client has its own bucket.
  EXPECT_TRUE(limiter.Allow(2, kT0).allowed);
  EXPECT_EQ(limiter.tracked_clients(), 2U);
}

TEST(RateLimiterTest, RetryAfterRoundsUpToWholeSeconds) {
  RateLimiter limiter({.requests_per_second = 0.25, .burst = 1},
                      /*max_clients=*/10);
  ASSERT_TRUE(limiter.Allow(1, kT0).allowed);
  // One token every 4 s; 1.5 s in, 2.5 s remain, rounded up to 3.
  EXPECT_EQ(limiter.Allow(1, kT0 + milliseconds(1500)).retry_after, seconds(3));
}

TEST(RateLimiterTest, EvictsOnlyRefilledBuckets) {
  RateLimiter limiter({.requests_per_second = 1, .burst = 1},
                      /*max_clients=*/10);
  ASSERT_TRUE(limiter.Allow(1, kT0).allowed);
  ASSERT_TRUE(limiter.Allow(2, kT0 + milliseconds(500)).allowed);
  // At t0 + 1 s, client 1 has refilled; client 2 has not.
  limiter.EvictIdle(kT0 + seconds(1));
  EXPECT_EQ(limiter.tracked_clients(), 1U);
  // Client 2's bucket survived, so it is still limited.
  EXPECT_FALSE(limiter.Allow(2, kT0 + seconds(1)).allowed);
}

TEST(RateLimiterTest, FailsOpenWhenTableIsFullOfActiveClients) {
  RateLimiter limiter({.requests_per_second = 1, .burst = 1},
                      /*max_clients=*/2);
  ASSERT_TRUE(limiter.Allow(1, kT0).allowed);
  ASSERT_TRUE(limiter.Allow(2, kT0).allowed);
  // Both tracked buckets are empty, so nothing can be evicted.
  EXPECT_TRUE(limiter.Allow(3, kT0).allowed);
  EXPECT_TRUE(limiter.Allow(3, kT0).allowed);
  EXPECT_EQ(limiter.tracked_clients(), 2U);
  EXPECT_EQ(limiter.untracked_allows(), 2U);
}

TEST(RateLimiterTest, FullTableMakesRoomByEvictingIdleBuckets) {
  RateLimiter limiter({.requests_per_second = 1, .burst = 1},
                      /*max_clients=*/1);
  ASSERT_TRUE(limiter.Allow(1, kT0).allowed);
  // Client 1 has refilled by now, so client 2 replaces it and is tracked.
  ASSERT_TRUE(limiter.Allow(2, kT0 + seconds(1)).allowed);
  EXPECT_FALSE(limiter.Allow(2, kT0 + seconds(1)).allowed);
  EXPECT_EQ(limiter.untracked_allows(), 0U);
}

TEST(ParseRateLimitTest, ParsesRateAndBurst) {
  const std::optional<RateLimit> limit = ParseRateLimit("100:200");
  ASSERT_TRUE(limit.has_value());
  EXPECT_EQ(limit->requests_per_second, 100);
  EXPECT_EQ(limit->burst, 200);
  ASSERT_TRUE(ParseRateLimit("0.5:1").has_value());
  EXPECT_EQ(ParseRateLimit("0.5:1")->requests_per_second, 0.5);
}

TEST(ParseRateLimitTest, RejectsMalformedLimits) {
  for (const char* bad : {"", "100", "100:", ":5", "0:5", "-1:5", "5:0.5",
                          "5:x", "x:5", "inf:5", "nan:5", "5:5 "}) {
    EXPECT_FALSE(ParseRateLimit(bad).has_value()) << bad;
  }
}

}  // namespace
}  // namespace sluice::proxy
