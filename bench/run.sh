#!/usr/bin/env bash
# Benchmarks sluice against Nginx under matched conditions (see README.md
# in this directory): both gateways from deploy/docker-compose.yml, same
# limits, same routes, same three backends; wrk2 at fixed request rates.
#
# Usage: ./bench/run.sh [seconds per run] [rates...]
#   e.g. ./bench/run.sh 60 500 1000 2000
# Prints one Markdown table row per gateway and rate. Needs Docker and the
# sluice-dev image (built from .devcontainer/Dockerfile), which has wrk2.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../deploy"

seconds="${1:-60}"
shift || true
rates=("$@")
if [[ ${#rates[@]} -eq 0 ]]; then
  rates=(500 1000 2000)
fi

docker compose up -d --build >/dev/null 2>&1
trap 'docker compose down >/dev/null 2>&1' EXIT
sleep 3

# Average CPU % and peak memory of one container while a run is going.
sample_stats() {
  local container="$1" samples="$2" cpu_sum=0 mem_max=0 n=0
  for ((i = 0; i < samples; i++)); do
    sleep 2
    read -r cpu mem < <(docker stats --no-stream --format \
      '{{.CPUPerc}} {{.MemUsage}}' "${container}" |
      awk '{gsub("%","",$1); print $1, $2}')
    mem_mib=$(awk -v m="${mem}" 'BEGIN {
      v = m + 0
      if (m ~ /GiB/) v *= 1024
      else if (m ~ /KiB/) v /= 1024
      printf "%.1f", v }')
    cpu_sum=$(awk -v a="${cpu_sum}" -v b="${cpu}" 'BEGIN { print a + b }')
    mem_max=$(awk -v a="${mem_max}" -v b="${mem_mib}" \
      'BEGIN { print (b > a ? b : a) }')
    n=$((n + 1))
  done
  awk -v s="${cpu_sum}" -v n="${n}" -v m="${mem_max}" \
    'BEGIN { printf "%.0f%% | %.1f MiB", s / n, m }'
}

echo "| Gateway | Target req/s | Achieved req/s | p50 | p90 | p99 | p99.9 | Errors | CPU (avg) | Memory (peak) |"
echo "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"
for rate in "${rates[@]}"; do
  # Alternate the order per rate so neither gateway always runs second.
  for target in "nginx:8081:deploy-nginx-1" "sluice:8080:deploy-sluice-1"; do
    IFS=: read -r name port container <<<"${target}"
    out=$(mktemp)
    docker run --rm --network deploy_default sluice-dev \
      wrk2 -t2 -c64 -d"${seconds}s" -R"${rate}" --latency \
      "http://${name}:${port}/" >"${out}" 2>&1 &
    wrk_pid=$!
    # Skip wrk2's ~10 s calibration window, then sample.
    sleep 12
    stats=$(sample_stats "${container}" $(((seconds - 16) / 3)))
    wait "${wrk_pid}"
    pct() { awk -v p="$1" '$1 == p { print $2 }' "${out}"; }
    achieved=$(awk '/Requests\/sec/ { print $2 }' "${out}")
    errors=$(awk '/Non-2xx/ { n += $5 } /Socket errors/ {
      gsub(",", ""); n += $4 + $6 + $8 + $10 } END { print n + 0 }' "${out}")
    echo "| ${name} | ${rate} | ${achieved} | $(pct 50.000%) | $(pct 90.000%) | $(pct 99.000%) | $(pct 99.900%) | ${errors} | ${stats} |"
    rm -f "${out}"
    sleep 5
  done
done
