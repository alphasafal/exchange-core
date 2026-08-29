# exchange-core

[![ci](https://github.com/alphasafal/exchange-core/actions/workflows/ci.yml/badge.svg)](https://github.com/alphasafal/exchange-core/actions/workflows/ci.yml)

A single-node exchange core in C++20: price-time-priority limit order book,
matching engine, pre-trade risk and kill switch, write-ahead journal with
deterministic replay, a binary wire protocol, and a TCP/UDP exchange simulator.

The engineering goal is not feature count. It is that every claim in this
repository can be reproduced by a stranger with one command, and that the
things which are hard to get right — correctness under adversarial input,
crash recovery, tail latency — are demonstrated rather than asserted.

## How correctness is established

- **A reference model.** A deliberately naive O(N²) matcher serves as an
  executable specification. The optimised book is diffed against it on
  randomised order flow; any divergence is a bug in one of them.
- **Property-based invariants.** The book may never cross, quantity is
  conserved across every fill, and level totals must equal the sum of their
  resting orders.
- **Fuzzing.** The wire decoder is fuzzed on arbitrary bytes, the engine on
  generated command streams, and journal recovery on deliberately corrupted
  files.
- **Sanitizers.** AddressSanitizer and UndefinedBehaviorSanitizer across the
  suite; ThreadSanitizer over the transport layer, which is where the
  concurrency actually lives.
- **Deterministic replay.** Replaying the journal must reproduce a
  bit-identical engine state hash. This is asserted by a test, not by prose.

## How performance is reported

Latency is reported as p50/p90/p99/p99.9/p99.99/max from a log-linear
histogram, never as a bare average. Load is generated open-loop against
intended send times so that coordinated omission cannot flatter the tail. Every
figure ships with the CPU, OS, compiler, flags and build type that produced it,
and the command that regenerates it.

See [docs/limitations.md](docs/limitations.md) for what this system is not, and
why the numbers here are not comparable to production trading latencies.

## Build

Requires CMake 3.22+ and a C++20 compiler. Dependencies are fetched by CMake.

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release
```

Presets: `release` (portable `-O2`), `native` (tuned to the building machine,
used for benchmarks), `debug`, and `asan` / `ubsan` / `tsan`.

To see exactly what a build was produced with:

```bash
./build/release/apps/xc_info
```

## Status

Under active construction. Components land in dependency order — order book,
matching engine, risk, journal, protocol, network, measurement — and every
commit builds and passes CI on GCC, Clang and Apple Clang across Release and
Debug. Sections of this README describing measurement will carry real numbers
once the benchmark harness lands; until then this repository publishes no
performance figures at all.

## Licence

MIT — see [LICENSE](LICENSE).
