# CLAUDE.md

Context and rules for any Claude session working in this repository.

## Language rule

- All repository content — code comments, docs, README, ADRs, config
  comments, script comments and output messages, CI workflow names and
  steps, commit messages — must be written in English.
- All conversation with the user must be in Traditional Chinese (Taiwan).
  Technical terms may stay in English.

## Project

`sluice` is a one-month side project: a small, single-threaded HTTP/1.1 API
gateway in C++20, built so that every file in it can be fully explained, not
to be feature-complete. The author is an experienced JS/Python web engineer
moving into C++ and cloud infrastructure.

### In scope (this month)

- Single-threaded epoll event loop
- RAII socket wrappers
- HTTP/1.1 parser: `Content-Length` and keep-alive only (no chunked
  encoding), with header size/count limits as a security boundary
- Reverse proxy with backpressure
- Longest-prefix routing
- Round robin load balancing with passive down-marking
- Per-IP token bucket rate limiting
- Idle and upstream timeouts
- Unit tests, ASan/UBSan clean, benchmark vs Nginx under matched conditions

### Optional (only with time to spare)

- A libFuzzer target for the HTTP parser

### Out of scope (see README "Future work" for the reasoning)

Multi-worker + `SO_REUSEPORT`, upstream connection pooling, P2C load
balancing, active health checking, outlier detection, automatic retries, hot
config reload, Prometheus metrics, TSan, toxiproxy fault injection, HTTP/2,
TLS.

**Do not add any of these unprompted.** If you think one is genuinely
needed, raise it in conversation first and wait for a decision.

## Division of labor

The user writes the first version of core logic by hand: the event loop,
`Channel`, Socket RAII classes, `Acceptor`, the parser state machine,
backpressure logic, route matching, the round robin selector, timeout
checking, and the token bucket. In these areas, Claude's role is to review,
point out problems, and explain why — not to rewrite them into a full
implementation unless explicitly asked.

Build configuration, test scaffolding, scripts, Docker-related files, and
benchmark result write-ups may be produced directly by Claude.

## Explain-every-file rule

Whenever a file is added or substantially changed, explain it to the user in
three sentences, in Chinese: what this file is responsible for, what
resources it owns, and what happens when it fails. Remind the user to
confirm, before committing, that they can state these three points without
looking at the code.

## Conventions

- Namespace: `sluice::<module>` (e.g. `sluice::net`, `sluice::http`,
  `sluice::proxy`).
- RAII for all resources; no raw `new`/`delete`. Ownership is expressed with
  `unique_ptr`.
- No exceptions on the hot path; errors are reported via return values.
- Always use `steady_clock` for timing/timeouts, never `system_clock`.
- File names use snake_case.

## Common commands

```sh
# Build (debug, ASan/UBSan)
cmake --preset debug-asan && cmake --build --preset debug-asan

# Test
ctest --preset debug-asan --output-on-failure

# Format
./scripts/format.sh

# Full check (format + clang-tidy + build + test, both presets)
./scripts/check.sh
```

## Working rules

- Every change must pass `scripts/check.sh` before being considered done.
- Record significant design decisions as a new ADR under `docs/design/`,
  based on `docs/design/0000-template.md`.
