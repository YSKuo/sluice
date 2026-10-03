#!/usr/bin/env bash
# Formats (or checks the formatting of) all project source files with
# clang-format. Pass --check to verify formatting without modifying files
# (used by CI); with no arguments, files are reformatted in place.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

mapfile -t files < <(find src tests -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \))

if [[ ${#files[@]} -eq 0 ]]; then
  echo "No source files to format."
  exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
  clang-format --dry-run --Werror "${files[@]}"
else
  clang-format -i "${files[@]}"
fi
