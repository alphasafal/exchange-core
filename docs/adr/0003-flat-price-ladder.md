# 0003. Index price levels with a flat ladder instead of `std::map`

- **Status:** Accepted. It replaced the original `std::map` design.
- **Date:** 2026-10
- **Related:** [`price_ladder.hpp`](../../include/xc/core/price_ladder.hpp),
  [benchmarks.md § book shape](../benchmarks.md#how-cost-scales-with-the-shape-of-the-book),
  [limitations.md](../limitations.md),
  [comparison.md](../comparison.md)

## Context

Every rest and every cancel has to find a price level, and every match has to
find the best one. The first version kept each side in a `std::map<Price,
PriceLevel>`, which is correct, ordered, and allocation-friendly once its nodes
are pooled.

Measurement showed its cost rising with the number of live levels: 112.8 ns per
command at 10 levels, 143.3 ns at 5,000, about 5 ns per doubling. That is the
signature of a balanced tree's depth. A book with many quoted prices was paying
for it on every command, and the cost couldn't be stated without asking how
deep the book was.

## Options considered

### A. Keep `std::map`, with pooled nodes

It's unbounded in price, and the code is obviously correct. Its cost is
O(log levels) per operation, with pointer-chasing through nodes that may not be
in cache.

It lost on the measurement above. The 27% rise across the range was real and
would grow with deeper books.

### B. Sorted vector of levels

It has good locality for scanning near the top of book. But inserting or
removing a level in the middle shifts everything after it. That makes a new
price deep in the book cost O(levels), which is worse than the tree in exactly
the case that prompted this change.

### C. Hash map from price to level, plus a heap for the best price

Lookup is O(1), but the best price is O(log levels) to maintain on every level
add and remove. Two structures also have to agree on every change, which is a
new class of bug in the hottest code in the system.

### D. Flat ladder indexed by tick, with a hierarchical bitmap (chosen)

One `PriceLevel` slot per tick in a fixed band, per side. A level is found by
array index. Occupancy is a three-level bitmap of 64-bit words: one bit per
tick, one bit per word of that, and one bit per word of the middle level. The
best price takes one `countr_zero` (or `countl_zero`) per level, so three
instructions. The cost doesn't depend on how many levels are live.

## Decision

Option D. An instrument declares its price band. Prices outside it are rejected
with `InvalidPrice`.

## Consequences

- **The book is bounded in price.** That's a real constraint a general-purpose
  book wouldn't impose. An instrument whose price can move arbitrarily far
  can't be represented without re-centring the band, which isn't implemented.
- **Memory is spent up front.** At the default 65,535 ticks, that's about
  1.5 MB per side, allocated once. A wide band costs memory, not time.
- **The band became part of the journal format.** The band decides which
  prices a book accepts, so a replay that rebuilt an instrument with a default
  band could accept orders the original rejected. The instrument record gained
  the band, and `kRecordVersion` went to 2 (see [0002](0002-journal-before-matching.md)).
- **The result is a flat cost curve:** 106, 110, 110, 111, 111 ns from 10 to
  5,000 levels, measured against the map in the same session. A flat cost
  matters more than the percentage saved, because it means the engine's cost
  can be stated without knowing the book's depth.

## Evidence

- The ladder is diffed against `std::map` directly, with the same comparator,
  over 6,000 random inserts and erases per seed in both directions.
- The whole existing suite ran unchanged across the swap: the differential
  harness against the O(N²) reference matcher, the structural invariants, and
  the fuzz corpora.
- [benchmarks.md](../benchmarks.md) has the map-versus-ladder table.
  [comparison.md](../comparison.md) shows the other half of the story: the
  order-id index makes cancels O(1) even when ~12,000 orders queue at one
  price, where Liquibook's multimap scan reaches a 95 µs p99.

## Revisit if

- **Instruments need unbounded or fast-moving prices**, for example a crypto
  pair that can move 10× in a day with a fine tick. Then the band has to move:
  either re-centre the ladder when the price nears an edge (amortised, but a
  latency spike at the moment it happens), or go back to a tree for prices
  outside a hot window around the touch. The second option is the more likely
  design.
- **Memory per instrument becomes the limit.** At thousands of instruments,
  1.5 MB a side is gigabytes. Sizing the band per instrument from its real
  trading range would be the first fix.
