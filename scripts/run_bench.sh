#!/usr/bin/env bash
# Runs the benchmarks and prints them with the environment that produced them.
#
# The environment block is not decoration. A latency figure without the CPU,
# the compiler and the flags behind it cannot be checked by anyone, cannot be
# compared with anything, and cannot be reproduced -- which makes it an
# assertion rather than a measurement. This script emits both together, in
# markdown, so the two cannot be separated by accident when results are copied
# into a document.
#
#   ./scripts/run_bench.sh [--orders N] [--journal] [--out FILE]
set -euo pipefail

cd "$(dirname "$0")/.."

ORDERS=1000000
WARMUP=200000
RUN_JOURNAL=0
OUT=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --orders) ORDERS="$2"; shift 2 ;;
        --warmup) WARMUP="$2"; shift 2 ;;
        --journal) RUN_JOURNAL=1; shift ;;
        --out) OUT="$2"; shift 2 ;;
        --help)
            sed -n '2,12p' "$0"
            exit 0 ;;
        *) echo "unrecognised argument: $1" >&2; exit 2 ;;
    esac
done

# The native preset, not release: benchmarks are the one place where tuning for
# the building machine is the right choice, and the preset records that choice
# rather than leaving it implicit in someone's shell history.
PRESET=native
BUILD_DIR="build/${PRESET}"

if [[ ! -x "${BUILD_DIR}/bench/bench_matching" ]]; then
    echo "building the ${PRESET} preset..." >&2
    cmake --preset "${PRESET}" >/dev/null
    cmake --build --preset "${PRESET}" -j >/dev/null
fi

emit() {
    if [[ -n "${OUT}" ]]; then
        tee -a "${OUT}"
    else
        cat
    fi
}

if [[ -n "${OUT}" ]]; then
    : > "${OUT}"
fi

{
    echo "## Environment"
    echo
    echo '```'
    echo "date            $(date -u '+%Y-%m-%d %H:%M:%SZ')"

    case "$(uname -s)" in
        Darwin)
            echo "cpu             $(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)"
            echo "cores           $(sysctl -n hw.physicalcpu) physical, $(sysctl -n hw.logicalcpu) logical"
            echo "memory          $(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )) GiB"
            echo "os              macOS $(sw_vers -productVersion) (Darwin $(uname -r))"
            ;;
        Linux)
            echo "cpu             $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
            echo "cores           $(nproc) logical"
            echo "memory          $(( $(grep MemTotal /proc/meminfo | awk '{print $2}') / 1024 / 1024 )) GiB"
            echo "os              $(uname -sr)"
            # Frequency scaling changes results run to run, so its state is
            # recorded rather than assumed.
            if [[ -r /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]]; then
                echo "governor        $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor)"
            fi
            ;;
        *)
            echo "os              $(uname -sr)"
            ;;
    esac

    echo "preset          ${PRESET} (Release, tuned for this machine)"
    echo "flags           $(grep -m1 'CXX_FLAGS = ' "${BUILD_DIR}/CMakeFiles/xc_core.dir/flags.make" 2>/dev/null | cut -d= -f2- | sed 's/^ *//' || echo unknown)"
    echo "commit          $(git rev-parse --short HEAD 2>/dev/null || echo unknown)$( git diff --quiet 2>/dev/null || echo ' (working tree modified)')"
    echo '```'
    echo
    echo "Reproduce with:"
    echo
    echo '```bash'
    echo "cmake --preset ${PRESET} && cmake --build --preset ${PRESET} -j"
    echo "./scripts/run_bench.sh --orders ${ORDERS}"
    echo '```'
    echo
    echo "## Matching engine, in process"
    echo
    echo '```'
    "${BUILD_DIR}/bench/bench_matching" --orders "${ORDERS}" --warmup "${WARMUP}"
    echo '```'

    if [[ "${RUN_JOURNAL}" -eq 1 ]]; then
        echo
        echo "## Journal durability"
        echo
        echo "The same workload under each durability policy. The difference is the"
        echo "cost of the guarantee, measured rather than estimated."
        echo
        for policy in none interval always; do
            # Each policy gets a sample count matched to its cost. Persisting on
            # every record puts a device round trip of roughly a millisecond on
            # the command path, so a run sized for the in-memory case would take
            # hours -- and would not be more accurate. A few thousand samples
            # characterise a millisecond operation perfectly well; the sample
            # count is printed with the result so the difference is visible
            # rather than hidden.
            case "${policy}" in
                always)   policy_orders=$(( ORDERS / 500 )); ;;
                interval) policy_orders=$(( ORDERS / 10 )); ;;
                *)        policy_orders=$(( ORDERS / 10 )); ;;
            esac
            [[ "${policy_orders}" -lt 1000 ]] && policy_orders=1000

            journal_dir="$(mktemp -d)"
            echo "### durability: ${policy}"
            echo
            echo '```'
            "${BUILD_DIR}/bench/bench_matching" --orders "${policy_orders}" \
                --warmup 1000 --journal "${journal_dir}" \
                --durability "${policy}" | sed -n '/^throughput/,$p'
            echo '```'
            echo
            rm -rf "${journal_dir}"
        done
    fi
} | emit

if [[ -n "${OUT}" ]]; then
    echo "written to ${OUT}" >&2
fi
