#!/usr/bin/env bash
# Formats (or checks the formatting of) all project source files with
# clang-format. Pass --check to verify formatting without modifying files
# (used by CI); with no arguments, files are reformatted in place.
# clang-format is pinned to the toolchain version (LLVM 18) because
# formatting output differs between releases.
# Written for bash 3.2 (macOS /bin/bash): no mapfile, no empty-array
# expansion under `set -u`.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/.."

files=()
while IFS= read -r file; do
  files+=("${file}")
done < <(find src tests fuzz -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.hpp' \) | sort)

if [[ ${#files[@]} -eq 0 ]]; then
  echo "No source files to format."
  exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
  clang-format-18 --dry-run --Werror "${files[@]}"
else
  clang-format-18 -i "${files[@]}"
fi
