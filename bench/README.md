# Benchmarking sluice vs Nginx

This directory will hold the benchmark scripts and results comparing `sluice`
against Nginx under equivalent reverse-proxy configurations.

## Methodology

- Load generator: [wrk2](https://github.com/giltene/wrk2), run with a fixed
  request rate (`-R <rate>`). Plain `wrk` is not used because, under
  saturation, it only issues a new request after the previous one completes —
  so slow responses silently shrink the sample size instead of showing up as
  high latency. This is the coordinated omission problem, and it makes plain
  `wrk` understate tail latency exactly when it matters most.
- The target rate for each run is chosen below that setup's actual
  saturation point, so reported latencies reflect queueing and processing
  delay rather than the load generator's own backpressure.
- Both `sluice` and Nginx are benchmarked against the same three backend
  instances (see `deploy/backends/`), with equivalent routing and load
  balancing behavior (longest-prefix routing, round robin across backends).
- Both gateways run from `deploy/docker-compose.yml` with identical CPU
  (1 core) and memory limits, one worker each, Nginx's access log off, and
  no upstream connection pooling on either side (`deploy/nginx/nginx.conf`
  documents each matching choice).
- Each run records: request rate, duration, p50/p90/p99/p99.9 latency, error
  count, and CPU/memory usage of the gateway process.
- Hardware and OS details (CPU model, core count, RAM, kernel version, and
  whether the run happened inside the devcontainer or on bare metal) are
  recorded alongside every result set, since absolute numbers are not
  comparable across machines.

## Status

No benchmarks have been run yet. This file will be updated with the actual
`wrk2` invocation, the Nginx reference configuration diff (if any), and
result tables once the reverse proxy path is implemented (Week 3-4).
