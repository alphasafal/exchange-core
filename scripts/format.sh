#!/usr/bin/env bash
# Format (or check) every source file with a pinned clang-format.
#
# The version is pinned because clang-format's output changes between releases:
# a repository formatted with 17 and checked with 18 produces a red CI run that
# has nothing to do with the change under review. Installing the formatter from
# PyPI gives developers and CI byte-identical binaries on every platform.
#
#   ./scripts/format.sh          rewrite files in place
#   ./scripts/format.sh --check  fail if anything is unformatted (used by CI)
set -euo pipefail

CLANG_FORMAT_VERSION="17.0.6"
cd "$(dirname "$0")/.."

if [[ -n "${CLANG_FORMAT:-}" ]]; then
    formatter="${CLANG_FORMAT}"
elif command -v clang-format >/dev/null 2>&1 &&
     clang-format --version | grep -q "${CLANG_FORMAT_VERSION}"; then
    formatter="clang-format"
else
    echo "clang-format ${CLANG_FORMAT_VERSION} not found. Install it with:" >&2
    echo "    pip install clang-format==${CLANG_FORMAT_VERSION}" >&2
    echo "or point CLANG_FORMAT at an existing binary of that version." >&2
    exit 1
fi

# Read into an array without mapfile: macOS still ships bash 3.2 as /bin/bash,
# and every contributor script here has to run on the machine the project is
# developed on as well as on the Linux CI runners.
files=()
while IFS= read -r file; do
    files+=("${file}")
done < <(find include src apps tests bench fuzz \
    \( -name '*.hpp' -o -name '*.cpp' -o -name '*.h' \) 2>/dev/null | sort)

if [[ ${#files[@]} -eq 0 ]]; then
    echo "no source files found"
    exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
    "${formatter}" --style=file --dry-run --Werror "${files[@]}"
    echo "format: ${#files[@]} files clean"
else
    "${formatter}" --style=file -i "${files[@]}"
    echo "format: ${#files[@]} files rewritten"
fi
