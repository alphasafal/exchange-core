#!/usr/bin/env bash
# Collects hardware performance counters for the matching benchmark.
#
# Linux only, and bare metal only. Counters are read from the CPU's performance
# monitoring unit, which virtual machines and cloud CI runners either emulate or
# do not expose at all -- so a number collected there describes the hypervisor
# as much as the code. That is why no counter measured this way is published in
# docs/benchmarks.md unless it came from a machine that could actually produce
# one.
#
# On macOS the equivalent is Instruments:
#
#   xcrun xctrace record --template 'CPU Counters' \
#       --launch ./build/native/bench/bench_matching -- --orders 1000000
#
#   ./scripts/perf_stat.sh [--orders N]
set -euo pipefail

cd "$(dirname "$0")/.."

ORDERS=1000000

while [[ $# -gt 0 ]]; do
    case "$1" in
        --orders) ORDERS="$2"; shift 2 ;;
        --help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unrecognised argument: $1" >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "perf is Linux-only. On macOS use Instruments:" >&2
    echo "  xcrun xctrace record --template 'CPU Counters' \\" >&2
    echo "      --launch ./build/native/bench/bench_matching -- --orders ${ORDERS}" >&2
    exit 1
fi

if ! command -v perf >/dev/null 2>&1; then
    echo "perf is not installed (try: apt-get install linux-tools-common)" >&2
    exit 1
fi

# Reading counters usually needs a relaxed paranoid level. Reported rather than
# changed: silently loosening a kernel security setting from a benchmark script
# is not this script's decision to make.
paranoid="$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo unknown)"
if [[ "${paranoid}" != "unknown" && "${paranoid}" -gt 1 ]]; then
    echo "note: /proc/sys/kernel/perf_event_paranoid is ${paranoid}; counters may be" >&2
    echo "      unavailable. Lowering it to 1 requires root and is your call:" >&2
    echo "      sudo sysctl kernel.perf_event_paranoid=1" >&2
fi

BUILD_DIR="build/native"
if [[ ! -x "${BUILD_DIR}/bench/bench_matching" ]]; then
    cmake --preset native >/dev/null
    cmake --build --preset native -j >/dev/null
fi

echo "## Hardware counters"
echo
echo '```'
perf stat -e task-clock,cycles,instructions,branches,branch-misses,\
cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses \
    "${BUILD_DIR}/bench/bench_matching" --orders "${ORDERS}" 2>&1 |
    sed -n '/Performance counter stats/,$p'
echo '```'
