#!/usr/bin/env bash
#
# Builds the browser demo: xc_core compiled to WebAssembly.
#
# Deliberately a script rather than a CMake target. The toolchain is a
# cross-compiler that most people building this project will not have, and
# wiring it into the main build would make `cmake --preset release` depend on
# whether Emscripten happens to be installed.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-${ROOT}/web/dist}"

command -v em++ >/dev/null || {
    echo "em++ not found. Install Emscripten: brew install emscripten" >&2
    exit 1
}

mkdir -p "${OUT}"

# The journal sources are linked although the demo attaches no journal: the
# engine references JournalWriter::append on its command path, guarded by a null
# check. Linking them is honest -- the alternative is an #ifdef that makes the
# WebAssembly build a different engine from the one under test.
#
# -O3 and no exceptions on the engine's side: the matching path does not throw,
# and exception support costs size and speed in WebAssembly. -sMODULARIZE is
# left off so the page can use the plain `Module` global, which keeps app.js
# readable for anyone reading this as an example rather than as a product.
em++ -std=c++20 -O3 \
    -I "${ROOT}/include" \
    "${ROOT}/web/bridge.cpp" \
    "${ROOT}"/src/core/*.cpp \
    "${ROOT}"/src/risk/*.cpp \
    "${ROOT}"/src/journal/*.cpp \
    "${ROOT}"/src/util/crc32c.cpp \
    "${ROOT}"/src/util/histogram.cpp \
    "${ROOT}"/src/util/build_info.cpp \
    -lembind \
    -sENVIRONMENT=web \
    -sALLOW_MEMORY_GROWTH=1 \
    -sINITIAL_MEMORY=33554432 \
    -sEXPORT_NAME=Module \
    -sSINGLE_FILE=0 \
    -DXC_VERSION='"0.1.0"' \
    -DXC_COMPILER_ID='"Emscripten"' \
    -DXC_COMPILER_VERSION="\"$(em++ -dumpversion)\"" \
    -DXC_BUILD_TYPE='"Release"' \
    -DXC_SANITIZER_NAME='"none"' \
    -o "${OUT}/exchange_core.js"

cp "${ROOT}/web/index.html" "${ROOT}/web/app.js" "${OUT}/"
echo "built -> ${OUT}"
ls -la "${OUT}"
