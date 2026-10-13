#!/usr/bin/env bash
#
# Runs the demo end to end, unattended and repeatably.
#
# This is the script docs/demo.md narrates over. It is written to be run by a
# person reading along, by asciinema while recording, and by anyone who wants
# to check that the claims in the README hold on their own machine -- which is
# the real reason it exists. A demo that only works when its author drives it
# is a video, not evidence.
#
# Every step either proves something or sets up the step that does. The last
# one is the point: kill the venue mid-flight, replay its journal, and show the
# reconstructed state digest matching the original.

set -euo pipefail

readonly ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
readonly PRESET="${PRESET:-release}"
readonly BUILD="${ROOT}/build/${PRESET}"
readonly WORK="$(mktemp -d "${TMPDIR:-/tmp}/xc-demo-XXXXXX")"
readonly JOURNAL="${WORK}/journal"

VENUE_PID=""

cleanup() {
    if [[ -n "${VENUE_PID}" ]] && kill -0 "${VENUE_PID}" 2>/dev/null; then
        kill -9 "${VENUE_PID}" 2>/dev/null || true
        wait "${VENUE_PID}" 2>/dev/null || true
    fi
    rm -rf "${WORK}"
}
trap cleanup EXIT

step() {
    printf '\n\033[1;36m==> %s\033[0m\n' "$*"
}

note() {
    printf '    \033[2m%s\033[0m\n' "$*"
}

# --- 1. what this binary actually is ---------------------------------------
step "Build, and report exactly what was built"
cmake --preset "${PRESET}" >/dev/null
cmake --build --preset "${PRESET}" -j >/dev/null
"${BUILD}/apps/xc_info"
note "Compiler, flags and build type are compiled into the binary, not"
note "reported by the script. A demo cannot misreport what it is running."

# --- 2. the suite ----------------------------------------------------------
step "Run the test suite"
ctest --preset "${PRESET}" --output-on-failure 2>&1 | tail -4
note "Includes the differential harness against the O(N^2) reference model"
note "and the structural invariants. Both have found real defects."

# --- 3. start the venue ----------------------------------------------------
step "Start the venue, journalling every command"
mkdir -p "${JOURNAL}"
"${BUILD}/apps/exchange_sim" --journal "${JOURNAL}" --durability always --port 0 >"${WORK}/venue.log" 2>&1 &
VENUE_PID=$!

# Wait for the gateway to report its port rather than sleeping a fixed time,
# which is the difference between a demo that works on a loaded machine and one
# that works on the author's.
PORT=""
for _ in $(seq 1 100); do
    PORT="$(sed -n 's/.*order entry:  127\.0\.0\.1:\([0-9]*\).*/\1/p' "${WORK}/venue.log" | head -1)"
    [[ -n "${PORT}" ]] && break
    sleep 0.1
done
if [[ -z "${PORT}" ]]; then
    echo "venue did not report a port; log follows:" >&2
    cat "${WORK}/venue.log" >&2
    exit 1
fi
sed -n '1,8p' "${WORK}/venue.log"
note "Port 0 means the OS picks one, so concurrent runs cannot collide."

# --- 4. trade --------------------------------------------------------------
step "Send crossing orders and watch them match"
"${BUILD}/apps/xc_client" --port "${PORT}" --orders 200 2>&1 | tail -12
note "Price-time priority, partial fills, and the acknowledgements that"
note "report them, over the real binary protocol on a real socket."

# --- 5. kill it ------------------------------------------------------------
step "Kill the venue outright -- SIGKILL, no shutdown, no flush"
kill -9 "${VENUE_PID}"
wait "${VENUE_PID}" 2>/dev/null || true
VENUE_PID=""
note "No shutdown hook runs, nothing is flushed on the way out. Durability"
note "is 'always', so every acknowledged command is already on the drive --"
note "that is the claim being tested, and SIGKILL is how you test it."
note "Under the default 'interval' policy most of this run would be gone:"
note "the 100ms timer never fires in a demo this short."

# --- 6. recover ------------------------------------------------------------
step "Replay the journal into a fresh engine"
"${BUILD}/apps/replay" --journal "${JOURNAL}"
note "The recovery outcome above says whether the tail was torn, and the"
note "state digest fingerprints every resting order in matching order."

step "Replay again and require the same digest"
DIGEST="$("${BUILD}/apps/replay" --journal "${JOURNAL}" --quiet)"
printf '    digest: %s\n' "${DIGEST}"
"${BUILD}/apps/replay" --journal "${JOURNAL}" --verify "${DIGEST}" >/dev/null
printf '\n\033[1;32m==> Replay is deterministic: --verify %s passed.\033[0m\n' "${DIGEST}"

note "--verify is the binary's own check, so the comparison is not this"
note "script's to get wrong. This is the weak form: a digest equal to"
note "itself. test_replay.cpp asserts the strong one -- that the replayed"
note "digest equals the digest of the engine that produced the journal."
