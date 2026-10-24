# ADR 0004: Reverse proxy, routing, load balancing, and timeouts

## Status

Accepted

## Context

Week 3 turns the HTTP server into a gateway: each request is routed by
path prefix, forwarded to one of several upstreams, and the response is
streamed back. The gateway must not buffer unbounded data for a slow peer,
must notice failed upstreams, and must not let a slow or silent peer hold a
connection forever. Out of scope (README "Future work"): connection
pooling, retries, active health checks, outlier detection, hot reload.

## Options

- **Proxy structure:** make `HttpServer`'s handler asynchronous and plug the
  proxy in as a handler vs a dedicated `ProxyServer` with its own
  per-connection state machine (reusing the parser and serializer).
- **Upstream connections:** one per request with `Connection: close` vs a
  pool of keep-alive connections.
- **Response handling:** buffer the whole upstream response vs stream it.
- **Timers:** `timerfd` per timer vs timers kept by the event loop that
  bound the `epoll_wait()` timeout.
- **Route matching:** raw string prefix (nginx `location`) vs
  segment-aware prefix.

## Decision

- **Dedicated proxy module** (`src/proxy/`): `ProxyServer` owns one
  `ClientSession` per client connection (client-side state machine), which
  owns at most one `UpstreamCall` (connect, send, parse head, stream body).
  Streaming, two-sided backpressure, and cancellation when the client leaves
  did not fit a generic handler interface without making it harder to read.
  `HttpServer` stays as a simple synchronous server (tests use it as a fake
  upstream).
- **One upstream connection per request**, sent with `Connection: close`.
  Pooling is out of scope; this also delimits close-framed responses.
- **Streamed responses with backpressure.** Upstream bytes are forwarded as
  they arrive. When more than 256 KiB are queued for the client, the
  upstream is paused until the client drains its output. The request
  direction is bounded by the parser's 1 MiB body limit.
- **Event-loop timers.** `EventLoop::RunAfter()` keeps timers in a map
  ordered by `steady_clock` deadline; `epoll_wait()` sleeps at most until the
  next one. `net::Timer` is the RAII handle: destroying it cancels it.
  Timers run after I/O events and before deferred deletions.
- **Timeouts:** client idle 60 s (between requests; silent close), client
  request 10 s (from the first byte, not extended by further bytes: 408;
  stops slowloris), upstream connect 2 s (504), upstream idle 15 s (no
  bytes from the upstream: 504, or a closed client connection if the
  response had started). The upstream idle timer also bounds how long a
  client that stops reading can hold an upstream, because a paused upstream
  sends no bytes.
- **Segment-aware longest-prefix routing.** `/api` matches `/api` and
  `/api/x` but not `/apikey`; `/api/` matches everything below it. Targets
  with `.` or `..` segments are rejected with 400, because the gateway
  routes on the raw path and an upstream that normalizes it could serve a
  route the gateway never checked.
- **Round robin with passive down-marking.** A connect failure or connect
  timeout takes an upstream out of rotation for 10 s; no retry for the
  failed request (502/504). If every upstream of a route is down: 503.
  Upstreams are shared across routes by address, so health state is too.
- **Header rewriting** (RFC 9110 7.6): hop-by-hop fields (fixed list plus
  those named in `Connection`) are dropped both ways; `Expect` is dropped
  because the body is already buffered; `X-Forwarded-For` gets the client
  address appended; HTTP/1.0 clients without `Host` get the upstream's name.
- **Upstream responses are parsed as strictly as requests** (shared rules in
  `http/syntax.h`). Chunked or malformed responses are 502. A response
  without `Content-Length` is passed on until the upstream closes, and the
  client connection is closed after it.

## Trade-offs

- A new TCP handshake per request costs latency and upstream sockets in
  TIME_WAIT; connection pooling is the first thing to add later.
- No retries: a request that hits an upstream just as it fails gets an
  error, even if another upstream is healthy.
- Passive down-marking reacts only to connect failures, not to slow or
  erroring upstreams (that is outlier detection, out of scope).
- While a request is in flight the client connection's reading is paused,
  so a client that disconnects is only noticed when the response is written
  or the upstream idle timeout fires.
- Percent-encoded dot segments (`%2e%2e`) are not decoded or rejected.
- Upstream names are resolved once at startup; an upstream whose address
  changes (e.g. a restarted container) needs a gateway restart.

## References

- RFC 9110 sections 7.6 (message forwarding), 15.6.3-15.6.5 (502-504)
- RFC 9112 section 6.3 (message body length)
- `src/proxy/`, `src/net/connector.{h,cpp}`, `src/net/timer.h`,
  `tests/proxy_server_test.cpp`
