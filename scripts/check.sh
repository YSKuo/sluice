#!/usr/bin/env bash
# Runs every check expected to pass before a commit: formatting, clang-tidy,
# and a full configure/build/test cycle for both CMake presets.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

echo "==> Checking formatting"
./scripts/format.sh --check

echo "==> Configuring debug-asan (for compile_commands.json)"
cmake --preset debug-asan

echo "==> Running clang-tidy"
mapfile -t files < <(find src tests -type f -name '*.cpp')
if [[ ${#files[@]} -gt 0 ]]; then
  clang-tidy -p build/debug-asan "${files[@]}"
fi

for preset in debug-asan release; do
  echo "==> Building preset: ${preset}"
  cmake --preset "${preset}"
  cmake --build --preset "${preset}"

  echo "==> Testing preset: ${preset}"
  ctest --preset "${preset}" --output-on-failure
done

echo "==> All checks passed"
