# exchange-core vs Liquibook

Both engines were run on identical input: the same random sequence, the same
price spread, one cancel for every four submits, and the same warm-up.
`bench/compare/bench_liquibook.cpp` is a line-for-line copy of
`bench/bench_matching.cpp`'s workload, driving [Liquibook] instead. Liquibook is
pinned to commit `2427613` so the comparison can be rerun against the same code.

[Liquibook]: https://github.com/enewhuis/liquibook

## Environment

```
date            2026-11-24 03:45Z
cpu             Intel Xeon @ 2.10GHz, 2 vCPUs (shared cloud VM)
memory          7 GiB
os              Linux 6.18
compiler        GCC 13.3.0, C++20
flags           Release, -march=native, LTO
exchange-core   662c50f + this change
liquibook       2427613b32f1667abae68a01df6af9ba8270f8e7
```

This is a shared cloud VM, not the Apple M2 used in [benchmarks.md](benchmarks.md),
so absolute numbers are higher here. Both engines ran back to back in the same
session, so the comparison between them holds.

Reproduce:

```bash
cmake -S . -B build/compare -DCMAKE_BUILD_TYPE=Release \
      -DXC_NATIVE_ARCH=ON -DXC_LTO=ON -DXC_BENCH_LIQUIBOOK=ON
cmake --build build/compare -j --target bench_matching bench_liquibook
build/compare/bench/bench_matching  --orders 1000000 --live-cancels
build/compare/bench/bench_liquibook --orders 1000000 --live-cancels
```

## Result: 40 price levels, cancels of live orders only

Median of three runs. 1,000,000 submits and 250,000 cancels, about 12,000
orders resting at each price level by the end.

| | exchange-core | Liquibook |
|---|---|---|
| Cost per command, untimed pass | **165 ns** | 8,034 ns |
| Submit p50 / p99 / p99.9 | 153 / 397 / 535 ns | **137 / 433 / 757 ns** |
| Cancel p50 / p99 / p99.9 | **123 / 286 / 360 ns** | 273 / 95,359 / 129,791 ns |

**Submits are a draw.** Liquibook is about 10% faster at the median. It does
less work per order: exchange-core checks every order against its instrument
(tick size, lot size, price band, self-trade policy) and Liquibook checks none
of these. The tails are close, with exchange-core slightly tighter.

**Cancels are where the designs differ.** Liquibook keeps each side of the book
in a `std::multimap` keyed by price and has no index from order to position.
To cancel, it finds the price and then walks every order resting there until
it reaches the right one. So its cancel cost grows with the number of orders
at that price. exchange-core holds an order-id index into a slab, so a cancel
is a lookup and an unlink, whatever the queue length.

## How the gap changes with book shape

Same workload, varying only the number of distinct price levels. One run each,
live cancels only. More levels means fewer orders per level, so Liquibook's
scan gets shorter.

| Price levels | Orders per level, approx. | exchange-core cost/cmd | Liquibook cost/cmd | Liquibook cancel p99 |
|---|---|---|---|---|
| 40 | 12,000 | 165 ns | 8,034 ns | 95,359 ns |
| 1,000 | 290 | 171 ns | 533 ns | 4,959 ns |
| 5,000 | 60 | 167 ns | 239 ns | 1,305 ns |

exchange-core is flat across the range, matching the ladder result in
[benchmarks.md](benchmarks.md). Liquibook is competitive on a thinly spread book
and falls behind as queues lengthen at each price. In real markets the queues
at the best prices are usually the long ones, and that is the case this table
cannot make look better for exchange-core than it is.

## A correction to the original benchmark

Building this comparison found a flaw in `bench_matching`. It cancels a random
id from every order that ever rested, without checking whether the order has
since been filled. About two thirds of its cancels named orders that no longer
existed and were rejected:

| | Timed cancels | Rejected |
|---|---|---|
| Default workload | 250,000 | **164,116 (66%)** |
| `--live-cancels` | 249,984 | 0 |

So the cancel latency in [benchmarks.md](benchmarks.md) mostly measures the
rejection path, a lookup that misses, not the removal of a resting order. On
this machine, live-only cancels measure p50 123 ns and p99 286 ns. That is
faster at the tail than the mixed figure (p99 ~990 ns). Why the rejection path
has the heavier tail has not been investigated yet, and it is the next thing
to profile.

The default stays as it was, so earlier numbers can still be compared.
`--live-cancels` is the mode to use when the question is the cost of removing
an order. A cancel that loses a race with a fill is a real event at a venue,
so neither mode is wrong, but they answer different questions and the
published figures did not say which one they answered.

## What this comparison does not show

- **One instrument, one thread, no network, no journal.** It compares matching
  cores, not venues.
- **Liquibook is a library, not a venue.** It has no risk checks, journal or
  protocol, and that is not a criticism. The comparison is narrow on purpose.
- **A shared cloud VM.** Tails beyond p99.9 include the hypervisor and are not
  comparable between runs.
- **Liquibook's best case is a book spread thinly across many prices.** The
  5,000-level row shows it close to exchange-core there.
