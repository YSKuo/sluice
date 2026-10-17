#!/usr/bin/env bash
# Builds the libFuzzer target for the HTTP request parser (fuzz preset) and
# runs it for a bounded time. Usage: ./scripts/fuzz.sh [seconds] (default 60).
# Any crash aborts with the failing input saved under build/fuzz/.
# Linux only; on macOS run it inside the devcontainer.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

seconds="${1:-60}"

cmake --preset fuzz
cmake --build --preset fuzz

# Copy the seed corpus so libFuzzer's discoveries do not pollute the repo.
work_corpus="build/fuzz/corpus"
mkdir -p "${work_corpus}"
cp fuzz/corpus/* "${work_corpus}/"

./build/fuzz/request_parser_fuzzer \
  -max_total_time="${seconds}" \
  -dict=fuzz/http.dict \
  -artifact_prefix=build/fuzz/ \
  "${work_corpus}"
