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
- The Python backends raise their listen backlog from socketserver's
  default of 5 to 1024. Both gateways open a new upstream connection per
  request, so a backlog of 5 overflows under bursts; the dropped SYNs are
  retried after 1 s and produced ~1 s p99 latency for nginx and sluice
  alike in early smoke tests, an artifact of the backend, not the gateway.
- Each run records: request rate, duration, p50/p90/p99/p99.9 latency, error
  count, and CPU/memory usage of the gateway process.
- Hardware and OS details (CPU model, core count, RAM, kernel version, and
  whether the run happened inside the devcontainer or on bare metal) are
  recorded alongside every result set, since absolute numbers are not
  comparable across machines.

## How to run

```sh
docker build -t sluice-dev -f .devcontainer/Dockerfile .   # once, for wrk2
./bench/run.sh 60 2000 5000 10000 15000
```

`run.sh` starts `deploy/docker-compose.yml`, runs wrk2 (2 threads, 64
connections) against each gateway in turn for every rate, skips wrk2's
calibration window, samples `docker stats` for CPU and memory, and prints
one Markdown row per run.

## Results (2026-10-31)

Median of three rounds of 60 s each; the raw rounds are in
[`results/2026-10-31.md`](results/2026-10-31.md). Environment: Apple M2
(8 cores, 16 GiB), OrbStack Linux VM (8 vCPUs, 8 GiB). Each gateway was
limited to 1 CPU and 256 MiB; wrk2 and the backends shared the same VM. No
run reported an error (non-2xx response or socket error).

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

- **Up to 10,000 req/s the two are indistinguishable at the median.** p50
  and p90 stay within about 1 ms of each other, with no errors and every
  run reaching its target rate.
- **Tail latency is dominated by the environment.** The same gateway at
  the same rate varied by up to 100x at p99 between rounds (nginx at
  10,000 req/s: 46 ms to 846 ms; sluice at 2,000 req/s: 7.6 ms to 592 ms).
  Neither gateway was CPU-throttled (cgroup `nr_throttled` stayed at 0), so
  these spikes come from the shared VM (wrk2 and three Python backends run
  alongside), not from either gateway. Only large, consistent differences
  should be read into the p99 columns.
- **sluice uses about 10-20% more CPU per request.** This held in every
  round. A 30 s run at 20,000 req/s measured 19.5 s of CPU for sluice and
  17.0 s for nginx, mostly system time, so the extra cost is in syscalls.
  A likely cause (not yet profiled) is the upstream connect path: the
  descriptor is registered with epoll by `Connector`, removed, and
  registered again by the `TcpConnection`, and `TCP_NODELAY` and `SO_ERROR`
  each cost a syscall per request.
- **sluice degrades first under overload.** At 15,000 req/s the shared VM
  saturates: both gateways' tails grow to seconds, and in round 3 both
  fell short of the target rate (nginx 10,069 req/s, sluice 4,802 req/s).
  With less CPU headroom, sluice falls behind sooner.
- **sluice uses less memory**: 2.2-3.1 MiB peak against nginx's
  2.8-3.9 MiB, since it has no configuration machinery or modules.

These numbers compare two gateways under the same conditions on one
laptop. They are not absolute capacity figures: on dedicated hardware, with
the load generator and backends on separate machines, both would go
further and the tails would be far less noisy.
