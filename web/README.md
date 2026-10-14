# Browser demo

The matching engine compiled to WebAssembly, matching live on a page.

**Live: https://alphasafal.github.io/exchange-core/**

## What it is

`xc_core` — the same `OrderBook`, `MatchingEngine`, `RiskEngine` and
`KillSwitch` that the native binaries and the 383-test suite run against,
compiled by Emscripten and driven from JavaScript. It is not a model of the
engine written for the web. If the book is wrong here, it is wrong there.

`bridge.cpp` generates synthetic order flow over a narrow price band with three
accounts, so orders actually collide, and steers the submit/cancel mix toward a
target depth so the book reaches a steady state instead of growing forever.

## What it cannot tell you

**Latency.** Browsers deliberately coarsen their clocks to defeat timing side
channels — `performance.now()` typically resolves somewhere between 100 µs and
1 ms, against operations that take tens of nanoseconds. Per-operation
percentiles are not noisy here, they are unmeasurable.

So the page times a whole batch between two clock reads and divides, which the
resolution does support, and reports that as a browser number. WebAssembly also
runs several times slower than native. The native p50/p99/p99.9 — measured with
a timer that resolves 41.67 ns, reported with the CPU, OS, compiler and flags
that produced them — are in [../docs/benchmarks.md](../docs/benchmarks.md).

## Building it

```bash
brew install emscripten          # or install the emsdk
./web/build.sh                   # -> web/dist/
cd web/dist && python3 -m http.server 8000
```

The build is a script rather than a CMake target on purpose: Emscripten is a
cross-compiler most people building this project will not have, and wiring it
into the main build would make `cmake --preset release` depend on whether it
happens to be installed.

CI builds it from source on every push that touches `web/`, `src/` or
`include/`, and deploys to Pages. Nothing in `web/dist/` is committed — a
checked-in binary drifts from the source that produced it, and the only thing
this demo claims is that it is the real engine.

## Things worth trying on the page

**Trip the kill switch.** New orders are refused and the book drains to zero,
because cancellation keeps working. That asymmetry is deliberate and is the
single most important property in [../docs/risk.md](../docs/risk.md): an
account is halted exactly when it most needs to withdraw what it has resting.

**Set a price collar.** Orders priced outside the band are rejected rather than
adjusted — silently moving a client's price is a worse failure than refusing it.

**Change the self-trade policy.** Five of them, and they produce visibly
different books.
