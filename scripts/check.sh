#!/usr/bin/env bash
# Runs every check expected to pass before a commit: formatting, clang-tidy,
# and a full configure/build/test cycle for both CMake presets. CI runs this
# same script, so a local pass means a CI pass.
#
# sluice targets Linux only (epoll), so on macOS run this inside the
# devcontainer. Written for bash 3.2 so the platform check below can still
# print a useful message there.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

if [[ "$(uname -s)" != "Linux" ]]; then
  echo "error: sluice builds on Linux only (epoll); run this inside the devcontainer." >&2
  exit 1
fi

echo "==> Checking formatting"
./scripts/format.sh --check

echo "==> Configuring debug-asan (for compile_commands.json)"
cmake --preset debug-asan

echo "==> Running clang-tidy"
files=()
while IFS= read -r file; do
  files+=("${file}")
done < <(find src tests -type f -name '*.cpp' | sort)
if [[ ${#files[@]} -gt 0 ]]; then
  clang-tidy-18 --quiet -p build/debug-asan "${files[@]}"
fi

for preset in debug-asan release; do
  echo "==> Building preset: ${preset}"
  cmake --preset "${preset}"
  cmake --build --preset "${preset}"

  echo "==> Testing preset: ${preset}"
  ctest --preset "${preset}" --output-on-failure
done

echo "==> All checks passed"
