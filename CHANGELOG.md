# Changelog

Notable changes, newest first. Follows [Keep a Changelog](https://keepachangelog.com/)
loosely and [semantic versioning](https://semver.org/) strictly.

Entries say what changed and why it mattered. Where a defect is listed, it is a
defect this project actually had, not a category it might have had.

## [Unreleased]

## [0.1.0] — 2026-10-14

First tagged release. Everything in it builds and passes twelve CI jobs on GCC
13, Clang 18 and Apple Clang, across Release and Debug, under AddressSanitizer,
UndefinedBehaviorSanitizer and ThreadSanitizer, with clang-tidy enforced as
errors.

### Engine

- Price-time-priority limit order book: slab-allocated orders, intrusive
  per-level FIFO queues, 32-bit handles, O(1) cancel.
- Limit and market orders across Day, GTC, IOC and FOK, plus post-only.
  Fill-or-kill decides fillability **before** mutating anything, so the
  rejection path is a genuine no-op rather than an unwind.
- Five self-trade-prevention policies: allow, cancel incoming, cancel resting,
  cancel both, decrement both.
- Cancel/replace with exchange queue-priority rules — reducing quantity at the
  same price keeps priority, increasing it or repricing goes to the back.
- Matching engine routing across instruments under one total order.

### Risk

- Pre-trade limits on order quantity, notional, open orders and position.
  Position is checked against the position the account **would** hold if
  everything working filled, not what it holds now.
- Price collars in basis points, rejecting rather than adjusting.
- GCRA rate limiting with burst tolerance, charged on rejected messages too.
- Venue and per-account kill switches. A halt blocks new orders and never
  blocks cancellation.

### Durability

- Write-ahead journal, CRC-32C framed, with segment rotation and three
  durability policies. `Always` uses `F_FULLFSYNC` on macOS.
- Recovery distinguishes a torn tail from damage mid-segment, and reports
  where the good data ends; truncating there leaves a journal that reads clean.
- Deterministic replay reproducing a bit-identical FNV-1a state digest.

### Protocol and transport

- Versioned binary wire format, 12-byte frame header, specified byte for byte
  in `docs/protocol.md`.
- Gap detection over UDP market data.
- Portable readiness loop over `kqueue` and `epoll`, non-blocking TCP gateway
  with per-connection backpressure, UDP feed.

### Verification

- 374 tests: unit, integration, property-based invariants, and a differential
  harness against an O(N²) reference matcher used as an executable specification.
- Three libFuzzer targets with a committed corpus, whose oracles also run as
  ordinary tests on every platform.
- Zero allocations on the matching path, asserted by a test that replaces global
  `operator new`.

### Measurement

- Log-linear latency histogram reporting p50 through p99.99, never a bare average.
- Open-loop load generation against intended send times.
- `scripts/run_bench.sh` captures CPU, OS, compiler, flags and build type
  alongside every figure.

### Demo

- The engine compiled to WebAssembly, matching live at
  https://alphasafal.github.io/exchange-core/

### Defects found and fixed during development

Listed because the history is the interesting part, and each was fixed in the
commit that found it with a regression test checked against the broken code.

- **Fill-or-kill counted liquidity it could not trade.** Under `DecrementBoth`,
  an aggressor's own resting order consumes quantity without trading, so a
  50-lot all-or-nothing order passed a check that saw 60 and filled 40. Found by
  the differential harness.
- **`Order::filled()` counted self-trade-destroyed quantity as traded**, so a
  client reconciling fills disagreed with the venue. Found by the invariants.
- **Self-trade prevention leaked risk exposure.** It removes resting orders
  during someone else's command with no fill and no cancel, so nothing released
  the owner's exposure and an account slowly lost the ability to trade.
- **Journal segment names were parsed with `sscanf`**, which reports success
  before checking the trailing literal. A stray `.bak` was read as a live
  segment and failed recovery on a healthy journal; `segment--1.xcj` parsed as
  4294967295 and wrapped the writer's next index to zero. Found by clang-tidy.
- **The histogram's bucket exponent could wrap** on an out-of-range index,
  shifting by 2^64−1, which is undefined. Found by the static analyser.
- **The allocation counter collided with every sanitizer runtime** — both
  replace global `operator new`. The allocation tests now skip under a
  sanitizer with the reason printed, rather than observing zero and passing.
- **Allocation counting had been switched off** while five tests asserting zero
  allocations passed vacuously.
- **CI ran three of six build jobs and reported success**, because `include:`
  entries merge into every matrix combination and overwrote one another.
- **`.clang-tidy`'s exclusions never took effect** — they were written inside a
  YAML folded scalar where `#` is text, not a comment.

[Unreleased]: https://github.com/alphasafal/exchange-core/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/alphasafal/exchange-core/releases/tag/v0.1.0
