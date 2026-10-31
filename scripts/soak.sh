#!/usr/bin/env bash
# Runs the ASan/UBSan build of sluice under sustained load with misbehaving
# backends and clients, then fails if a sanitizer reported anything. Unit
# tests only reach the paths they were written for; this exercises the
# error paths (refused connects, slow and failing upstreams, rate limiting,
# malformed requests, abrupt disconnects) concurrently, for a while.
#
# Usage: ./scripts/soak.sh [seconds] (default 30). Linux only; on macOS run
# it inside the devcontainer, which provides wrk2, curl, and nc.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

seconds="${1:-30}"
work="build/soak"
mkdir -p "${work}"
pids=()
cleanup() {
  for pid in "${pids[@]}"; do
    kill "${pid}" 2>/dev/null || true
  done
}
trap cleanup EXIT

cmake --preset debug-asan >/dev/null
cmake --build --preset debug-asan >/dev/null

echo "==> Starting backends"
python3 deploy/backends/server.py --name fast --port 9101 &
pids+=($!)
python3 deploy/backends/server.py --name slow --port 9102 --delay-ms 50 &
pids+=($!)
python3 deploy/backends/server.py --name flaky --port 9103 --fail-rate 0.3 &
pids+=($!)
sleep 1

# Port 9109 has no listener: every request routed there is refused, which
# exercises connect failures and down-marking.
echo "==> Starting sluice (debug-asan)"
ASAN_OPTIONS="detect_leaks=1:abort_on_error=0" \
UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
  ./build/debug-asan/sluice --port 9100 \
    --route /=127.0.0.1:9101,127.0.0.1:9102,127.0.0.1:9103 \
    --route /dead/=127.0.0.1:9109,127.0.0.1:9101 \
    --rate-limit 600:600 \
    2>"${work}/sluice.err" &
sluice_pid=$!
sleep 1

echo "==> Load for ${seconds}s"
wrk2 -t2 -c32 -d"${seconds}s" -R400 http://127.0.0.1:9100/ \
  >"${work}/wrk-root.txt" 2>&1 &
wrk_root=$!
wrk2 -t1 -c8 -d"${seconds}s" -R200 http://127.0.0.1:9100/dead/x \
  >"${work}/wrk-dead.txt" 2>&1 &
wrk_dead=$!

# Misbehaving clients, repeated while the load runs.
end=$((SECONDS + seconds))
while ((SECONDS < end)); do
  printf 'GET / HTTP/1.1\r\nHost : x\r\n\r\n' | nc -q1 127.0.0.1 9100 >/dev/null || true
  printf 'POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n' |
    nc -q1 127.0.0.1 9100 >/dev/null || true
  # Disconnect in the middle of a request, and right after sending one.
  printf 'GET / HTTP/1.1\r\nHo' | nc -q0 127.0.0.1 9100 >/dev/null || true
  printf 'GET / HTTP/1.1\r\nHost: x\r\n\r\n' | nc -q0 127.0.0.1 9100 >/dev/null || true
  curl -s -o /dev/null --max-time 2 -X POST --data 'payload' \
    http://127.0.0.1:9100/echo || true
  sleep 0.2
done
wait "${wrk_root}" "${wrk_dead}" || true

echo "==> Stopping sluice"
kill -TERM "${sluice_pid}"
status=0
wait "${sluice_pid}" || status=$?

grep -E "Requests/sec|Non-2xx" "${work}/wrk-root.txt" "${work}/wrk-dead.txt" || true
if grep -qE "AddressSanitizer|LeakSanitizer|runtime error" "${work}/sluice.err"; then
  echo "error: sanitizer report in ${work}/sluice.err" >&2
  grep -E -A20 "AddressSanitizer|LeakSanitizer|runtime error" \
    "${work}/sluice.err" | head -60 >&2
  exit 1
fi
if [[ ${status} -ne 0 ]]; then
  echo "error: sluice exited with status ${status}" >&2
  exit 1
fi
echo "==> Soak passed: no sanitizer reports, clean shutdown"
