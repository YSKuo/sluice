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

# Run the gateway binary
./build/debug-asan/sluice --version
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
| Week 1 | Event loop + Socket RAII (echo server) | Not started |
| Week 2 | HTTP/1.1 parser + keep-alive + header limits | Not started |
| Week 3 | Reverse proxy, backpressure, routing, round robin, timeouts | Not started |
| Week 4 | Rate limiting, benchmark vs Nginx, ASan clean, design write-up | Not started |

## Design

TBD — design notes and rationale will be added here as each milestone lands.
See `docs/design/` for individual ADRs.

## Benchmark

TBD — see `bench/README.md` for the planned methodology. Results will be
added once the reverse proxy path is implemented.

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
