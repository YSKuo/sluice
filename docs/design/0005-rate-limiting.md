# ADR 0005: Per-client-IP rate limiting

## Status

Accepted

## Context

Week 4 adds rate limiting so that one greedy client cannot monopolize the
upstreams. The gateway is single-threaded and keeps no shared state, so the
limiter can be a plain in-process data structure. It must stay cheap on the
request path and bounded in memory no matter how many distinct clients
connect.

## Options

- **Algorithm:** token bucket vs fixed window counter vs sliding window log
  vs GCRA (generic cell rate algorithm).
- **Key:** client IP vs IP + port (connection) vs a request header.
- **Time source:** read the clock inside the limiter vs pass `now` in.
- **Memory bound:** fixed-size LRU vs evicting buckets that have refilled,
  plus a hard cap.
- **When the table is full:** fail open (allow untracked) vs fail closed
  (reject new clients).

## Decision

- **Token bucket** (`proxy::TokenBucket`): capacity `burst`, refilled at
  `requests_per_second`, one token per request, refilled lazily from the
  elapsed time when the bucket is used. It allows short bursts while
  enforcing the average rate, and is the easiest of the options to explain.
  A fixed window allows twice the limit across a window boundary; a
  sliding log stores a timestamp per request; GCRA is equivalent to a token
  bucket but harder to read.
- **Keyed by client IP** (`InetAddress::ip()`), not by connection: opening
  more connections must not buy more requests. The IP is the TCP peer, so
  it cannot be spoofed by a header (`X-Forwarded-For` is ignored).
- **`now` is a parameter.** `TokenBucket` and `RateLimiter` never read a
  clock; `ClientSession` passes `steady_clock::now()`. Tests use exact
  time points, so they are deterministic and run instantly.
- **Checked first** in `ClientSession::HandleRequest`, before routing or
  touching an upstream. A rejected request gets 429 with `Retry-After`
  (seconds until the next token, rounded up, at least 1); the connection
  stays open, because the client did nothing wrong at the protocol level.
- **Memory bounded by eviction plus a cap.** A bucket that has refilled to
  capacity is indistinguishable from a new one, so dropping it changes no
  decision. `ProxyServer` sweeps such buckets every 30 s with a
  `net::Timer`, and `RateLimiter` also sweeps when it reaches its cap
  (100,000 clients by default, a few MiB).
- **Fail open when full.** If the table is full of clients that are all
  actively limited, a new client is allowed without being tracked (and
  counted in `untracked_allows`). Rate limiting protects upstreams from a
  few greedy clients; it is not a defense against a distributed flood, and
  failing closed would lock out every legitimate newcomer during one.
- **Off by default**, enabled with `--rate-limit RPS:BURST`.

## Trade-offs

- Clients behind one NAT share one bucket.
- The limit is per gateway process; several gateways would each allow the
  full rate (multi-worker is out of scope anyway).
- Floating-point token counts can drift by tiny amounts; at request rates
  this is irrelevant, but the exact boundary tests use rates that are exact
  in binary (0.25, 2, 4 tokens per second).
- Rejected requests still cost a parse and a small response; dropping
  connections instead would be cheaper but gives clients no `Retry-After`.

## References

- `src/proxy/rate_limiter.{h,cpp}`, `tests/proxy_rate_limiter_test.cpp`
- RFC 6585 section 4 (429 Too Many Requests), RFC 9110 section 10.2.3
  (Retry-After)
