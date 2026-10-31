# sluice

A small, single-threaded HTTP/1.1 API gateway written in C++20, built to be
completely understood file by file rather than feature-complete.

## Non-goals

This project deliberately stays small. The following are out of scope for
now; see [Future work](#future-work) for why, and what would change first.

- Multi-worker processes / `SO_REUSEPORT`
- Upstream connection pooling
- Power-of-two-choices (P2C) load balancing
- Active health checking
- Outlier detection
- Automatic retries
- Hot config reload
- Prometheus metrics
- ThreadSanitizer (TSan)
- toxiproxy-based fault injection
- HTTP/2
- TLS

## Quick start

sluice is built directly on Linux APIs (epoll, signalfd, accept4), so it
builds and runs on Linux only. On macOS, use the devcontainer.

### Using the devcontainer (recommended)

Open this folder in VS Code and reopen in the devcontainer, or build and run
the same image directly with Docker:

```sh
docker build -t sluice-dev -f .devcontainer/Dockerfile .
docker run --rm -it -v "$PWD":/workspace -w /workspace sluice-dev bash
```

### Build, test, run

```sh
# Configure + build (debug, with ASan/UBSan)
cmake --preset debug-asan
cmake --build --preset debug-asan

# Run the unit tests
ctest --preset debug-asan --output-on-failure

# Start two test backends, then the gateway in front of them (Ctrl-C to stop)
python3 deploy/backends/server.py --name a --port 9001 &
python3 deploy/backends/server.py --name b --port 9002 &
./build/debug-asan/sluice --port 8080 \
  --route /api/=127.0.0.1:9001 \
  --route /=127.0.0.1:9001,127.0.0.1:9002

# In another shell: round robin across a and b, then the /api/ route
curl http://127.0.0.1:8080/ http://127.0.0.1:8080/ http://127.0.0.1:8080/api/x

# Full pre-commit check: format, clang-tidy, both presets, tests
./scripts/check.sh

# Fuzz the HTTP request parser for 60 seconds (libFuzzer + ASan/UBSan)
./scripts/fuzz.sh 60

# Run the ASan build under load with misbehaving peers for 60 seconds
./scripts/soak.sh 60
```

For an optimized build (used for benchmarking):

```sh
cmake --preset release
cmake --build --preset release
./build/release/sluice --version
```

## Milestones

| Week | Scope | Status |
| --- | --- | --- |
| Week 1 | Event loop + Socket RAII (echo server) | Done |
| Week 2 | HTTP/1.1 parser + keep-alive + header limits | Done |
| Week 3 | Reverse proxy, backpressure, routing, round robin, timeouts | Done |
| Week 4 | Rate limiting, benchmark vs Nginx, ASan clean, design write-up | Done |

## Design

sluice is one process with one thread. Everything happens inside a single
epoll event loop: no locks, no thread pools, and every callback must
return quickly without blocking.

### Layers

```
main.cpp              flags, signalfd shutdown, startup DNS for upstreams
proxy/                ProxyServer -> ClientSession -> UpstreamCall
                      RouteTable, RoundRobin, RateLimiter, forwarding rules
http/                 RequestParser, ResponseParser, Request/Response types
net/                  EventLoop (epoll + timers), Channel, TcpServer,
                      TcpConnection, Acceptor, Connector, Socket, Buffer
result.h              Result<T>: a value or a std::error_code
```

Each layer only uses the ones below it. `net/` knows nothing about HTTP;
`http/` parses and serializes but does no I/O; `proxy/` combines them.

### A request's path

1. `Acceptor` accepts the client; `TcpServer` wraps it in a
   `TcpConnection`; `ProxyServer` creates a `ClientSession` for it.
2. Bytes arrive in the connection's input buffer. `RequestParser` consumes
   complete lines only and enforces the size limits before a line ends.
3. With a complete request, `ClientSession` applies the rate limit, rejects
   dot segments, picks the longest-prefix route and a round-robin upstream,
   and pauses reading the client (one request in flight per connection).
4. `UpstreamCall` connects without blocking (`Connector`), sends the
   rewritten request, parses the response head with `ResponseParser`, and
   streams the body to the client. If the client falls behind, the upstream
   is paused until the client's output drains.
5. When the response ends, the upstream connection closes and the session
   goes back to step 2 for the next (possibly already buffered) request.

### Decisions worth knowing

- **Level-triggered epoll and deferred destruction** (ADR 0001). An object
  that closes from inside its own callback is destroyed only after the
  current batch of events, so no callback ever runs on freed memory.
- **Errors are values** (ADR 0002). `Result<T>` instead of exceptions or
  output parameters; failures that can only be bugs abort immediately.
- **Strict parsing as a security boundary** (ADR 0003). CRLF only, no
  `Transfer-Encoding`, one `Host`, exact `Content-Length`: anything the
  gateway and an upstream might read differently is rejected.
- **Every wait is bounded** (ADR 0004). Client idle and request timeouts,
  upstream connect and idle timeouts, all on `steady_clock`; the request
  timeout is not extended by trickled bytes, which stops slowloris.
- **Every buffer is bounded.** Parser limits bound requests; one request
  in flight bounds pipelining; the 256 KiB high-water mark bounds
  responses queued for slow clients; rate-limit buckets are evicted once
  refilled (ADR 0005).
- **Failures are passive and local.** An upstream that refuses
  connections is skipped for 10 s; there are no retries, health probes, or
  shared state, which keeps every decision explainable from one request.

### Verification

- 141 unit and integration tests, run under ASan/UBSan and in release
  mode (`./scripts/check.sh`, also run by CI).
- A libFuzzer target for the request parser (`./scripts/fuzz.sh`).
- A soak test that runs the ASan build under load with failing, slow, and
  refusing upstreams plus malformed and disconnecting clients, and fails on
  any sanitizer report (`./scripts/soak.sh`).

### Design records

Each decision above is argued, with the options that were rejected, in an
ADR under `docs/design/`:

- [ADR 0001: Event loop and connection lifecycle](docs/design/0001-event-loop.md)
- [ADR 0002: Google C++ style and error handling](docs/design/0002-google-style-and-errors.md)
- [ADR 0003: HTTP/1.1 request parser and keep-alive](docs/design/0003-http-parser.md)
- [ADR 0004: Reverse proxy, routing, load balancing, and timeouts](docs/design/0004-reverse-proxy.md)
- [ADR 0005: Per-client-IP rate limiting](docs/design/0005-rate-limiting.md)

## Benchmark

sluice and Nginx were run side by side under matched conditions (1 CPU and
256 MiB each, one worker, same routes and backends, no upstream connection
pooling) and loaded with wrk2 at fixed request rates. Median of three
rounds; method, raw data, and caveats are in
[`bench/README.md`](bench/README.md).

| Target req/s | Gateway | p50 | p90 | p99 | p99.9 | CPU (avg) | Memory |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 2,000 | nginx | 2.2 ms | 5.0 ms | 42 ms | 129 ms | 20% | 3.3 MiB |
| 2,000 | sluice | 3.4 ms | 5.9 ms | 8.9 ms | 106 ms | 23% | 2.6 MiB |
| 5,000 | nginx | 2.2 ms | 5.0 ms | 28 ms | 97 ms | 25% | 3.3 MiB |
| 5,000 | sluice | 2.3 ms | 5.2 ms | 34 ms | 128 ms | 29% | 2.8 MiB |
| 10,000 | nginx | 2.6 ms | 5.7 ms | 58 ms | 91 ms | 35% | 3.4 MiB |
| 10,000 | sluice | 2.7 ms | 6.5 ms | 141 ms | 205 ms | 41% | 2.6 MiB |
| 15,000 | nginx | 2.9 ms | 619 ms | 1.6 s | 1.7 s | 52% | 3.6 MiB |
| 15,000 | sluice | 96 ms | 1.2 s | 2.5 s | 3.0 s | 68% | 2.2 MiB |

In short: equal median latency up to 10,000 req/s, about 10-20% more CPU
per request than Nginx, earlier degradation once the machine saturates,
and a smaller memory footprint. Tail percentiles in this setup are
dominated by noise from the shared VM.

## Future work

Each item below is out of scope for this one-month project, along with why
it is deferred and what the eventual approach would look like.

- **Multi-worker + `SO_REUSEPORT`** — Later, each CPU core would run its own
  event loop with its own `SO_REUSEPORT`-bound listening socket. Getting a
  single event loop correct and clean first is the prerequisite for safely
  layering threads on top.
- **Upstream connection pooling** — Later, each upstream would keep a pool of
  idle, reusable connections to avoid a fresh three-way handshake per
  request. For now, the priority is making "open a new connection, forward,
  close" fully correct.
- **P2C load balancing** — Later, load balancing would pick two upstreams at
  random and route to whichever has fewer in-flight connections, which
  reflects real load better than round robin. It requires tracking live
  per-upstream connection counts, which round robin does not need.
- **Active health checking** — Later, a timer would periodically probe each
  upstream with a TCP connect, instead of waiting for real traffic to fail.
  The current passive down-marking is simple but inherently reacts late.
- **Outlier detection** — Later, the gateway would passively track real
  traffic failure rates and temporarily evict an upstream once its failure
  rate crosses a threshold — a finer-grained signal than today's "one
  connection failure marks it down," at the cost of maintaining a sliding
  window of statistics per upstream.
- **Automatic retries with a retry budget** — Today, a failed request is
  returned to the client as-is, with no retry. If retries are added later,
  the retry ceiling will be a percentage of currently in-flight requests
  rather than a fixed count, so retry volume cannot itself explode when an
  upstream is already failing broadly.
- **Hot config reload** — No configuration file format has been introduced
  yet. Once one exists, reload would swap in a new config atomically via
  `atomic<shared_ptr<const Config>>`, rather than mutating fields in place.
- **Prometheus metrics** — Later, a `/metrics` endpoint would expose
  connection counts, rate-limit rejections, and similar basics. For now,
  logs and manual observation are sufficient.
- **TSan** — The gateway is single-threaded today, so there is no data race
  to catch. TSan earns its keep once the multi-worker model lands.
- **toxiproxy fault injection** — Testing currently uses hand-rolled fake
  upstreams (unresponsive, or deliberately slow). More systematic fault
  injection is deferred until a more complete test framework is in place.
- **HTTP/2** — A fundamentally different binary framing protocol, with a
  parser design that has little in common with the HTTP/1.1 text parser
  built here. Out of scope for this project.
- **TLS** — Certificate management, the handshake state machine, and the
  integration work with the event loop are all substantial on their own and
  not the focus of this month; the priority is getting plaintext HTTP
  mechanics right first.
