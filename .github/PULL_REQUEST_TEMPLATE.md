## What this changes, and why

<!-- The why is the part that matters. What did the code do, why was that
     wrong or insufficient, and what happens now? -->

## Evidence

<!-- Delete the rows that do not apply. Keep this honest: an unchecked box is
     more useful than a checked one that is not true. -->

- [ ] `./scripts/format.sh --check` passes
- [ ] `ctest --preset release` and `ctest --preset debug` pass
- [ ] `ctest --preset asan` passes
- [ ] Bug fix: the new test was checked against the unfixed code and fails there
- [ ] Performance change: before/after from `./scripts/run_bench.sh` on an idle
      machine, with the captured environment, below
- [ ] Wire format, journal or book touched: fuzzers run beyond CI's 60 s gate
- [ ] `docs/` updated, including `limitations.md` if anything there stopped
      being true

## Measurements

<!-- perf changes only. Paste the environment block and the before/after.
     p50/p99/p99.9, not an average. Say so if the win is inside noise. -->
