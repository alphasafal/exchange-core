# Benchmarks

Every number here was produced by `./scripts/run_bench.sh` on the machine
described below, and can be regenerated with one command. Nothing in this
document is estimated, extrapolated, or carried over from a previous run.

Read [limitations.md](limitations.md) first if you are comparing these figures
to anything. In particular: this is a developer laptop, not a tuned server, and
the wire figures are loopback.

## Environment

```
date            2026-08-29 17:54:59Z
cpu             Apple M2
cores           8 physical, 8 logical
memory          8 GiB
os              macOS 26.2 (Darwin 25.2.0)
preset          native (Release, tuned for this machine)
compiler        AppleClang 17.0.0.17000013, C++20
flags           -O3 -DNDEBUG -std=c++20 -arch arm64 -march=native
                -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
                -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
                -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough -Werror
commit          14853f6
```

Reproduce:

```bash
cmake --preset native && cmake --build --preset native -j
./scripts/run_bench.sh --orders 1000000 --journal
```

## The most important caveat: timer resolution

`steady_clock` on this machine ticks at 24 MHz, so it resolves **41.67 ns** and
nothing finer. Measuring the clock against itself confirms it: the samples are
0 ns or 42 ns and never anything between.

Every individually-timed latency below is therefore a multiple of ~42 ns, and a
p50 of 83 ns means *two ticks*, not a measurement accurate to the nanosecond.
Differences smaller than one tick cannot be seen with this timer at all.

This is why the throughput figure is measured in a separate pass with no clock
read inside the loop. That pass divides one timestamp difference across a
million commands, so its resolution limit is a millionth of a tick, and it is
the more trustworthy number for *average* cost. The timed pass is what shows
the shape of the tail, which no average can.

The clock's cost is reported and never subtracted. Deducting an estimate from a
measurement produces a figure nobody else can reproduce.

## Matching engine, in process

One thread, no sockets, no journal. 40 price levels, one order cancelled for
every four submitted, so the book is worked rather than only grown. 200,000
commands of warm-up before anything is measured.

**Throughput, no clock read in the loop:**

| | |
|---|---|
| Commands | 1,250,000 |
| Wall time | 0.094 s |
| Throughput | **13.3 M commands/s** |
| Cost per command | **75.1 ns** |

**Latency, timed individually** (includes ~42 ns of timer, not subtracted):

| Operation | n | p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|---|---|
| submit | 1,000,000 | 83 ns | 125 ns | 167 ns | 333 ns | 3,711 ns | 39,000 ns |
| cancel | 250,000 | 42 ns | 250 ns | 334 ns | 916 ns | 4,003 ns | 20,834 ns |

The book ended with 285,319 resting orders across 24 price levels, with **zero
slab growth events** — so no sample in the measurement includes a reallocation.

The tail beyond p99.9 is dominated by the operating system, not the engine: this
is a laptop running a desktop, and a 39 µs maximum is a scheduler preemption,
not a matching operation. That is a property of the measurement environment and
would not be improved by changing the code.

## Allocation

The matching path performs **zero allocations in steady state**. This is
asserted by a test rather than claimed here: `tests/test_allocations.cpp`
replaces the global `operator new` and fails if a single allocation occurs
across 50,000 orders, 20,000 cancellations, a matching sweep, or 10,000 depth
snapshots.

It was not true when it was first measured. The counters found roughly one
allocation per order, which is recorded in the history:

| | Allocations per 50,000 orders |
|---|---|
| Original | 50,002 |
| After replacing the `unordered_map` order index | 23,780 |
| After pooling the price-level map nodes | 26 |
| After sizing the book from the workload | **0** |

## Journal durability

The same workload under each policy. The differences are the cost of the
guarantee.

Sample counts differ by policy on purpose: persisting on every record puts a
device round trip on the command path, so a run sized for the in-memory case
would take hours without being more accurate.

| Policy | Commands | Throughput | Cost per command | Persist calls |
|---|---|---|---|---|
| none | 125,000 | 3.78 M/s | 264 ns | 1 |
| interval (100 ms) | 125,000 | 3.53 M/s | 284 ns | 2 |
| always | 2,500 | **400/s** | **2.50 ms** | 5,386 |

Three things to take from this.

**Journalling at all costs about 190 ns per command** — 75 ns unjournaled
against 264 ns with the journal on. That is message encoding plus a buffered
write, and it is the price of being able to reconstruct the venue.

**Interval durability is nearly free** relative to no durability: 284 ns against
264 ns, because the cost is amortised across everything written between
persists. This is the setting a venue would actually run.

**Persisting every record costs four orders of magnitude.** 2.5 ms per command,
against 264 ns. On macOS this uses `F_FULLFSYNC`, which makes the drive flush
its own write cache — plain `fsync` returns sooner and does not actually
guarantee the data survives a power cut, so using it here would have made the
durability claim false while making this number look far better. The
measurement is what the guarantee actually costs.

## What has not been measured

**Hardware performance counters** — cache misses, branch misses, IPC. These
need `perf` on bare-metal Linux or Instruments on macOS. CI runners virtualise
the performance monitoring unit and do not report trustworthy counters, so
nothing of the kind is published here. `scripts/perf_stat.sh` ships the Linux
recipe for anyone with a suitable machine.

**End-to-end wire latency** — order in over TCP to execution out on the UDP
feed, measured open-loop against intended send times. The transport is built and
tested; the measurement is not yet done, and no figure is given for it.

**Sustained multi-hour behaviour.** Every run here is seconds long. Nothing is
claimed about memory or latency drift over a trading day.

**Any comparison to a real venue.** These numbers describe this software's logic
on this laptop. They are not comparable to a production matching engine and no
such comparison is made.
