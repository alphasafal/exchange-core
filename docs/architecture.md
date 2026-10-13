# Architecture

![The command path from socket to fill](architecture.svg)

The source is [architecture.mmd](architecture.mmd). The SVG is committed
alongside it so the diagram renders on GitHub without a mermaid plugin, and is
regenerated with:

```bash
npx -p @mermaid-js/mermaid-cli mmdc \
    -i docs/architecture.mmd -o docs/architecture.svg \
    -c docs/mermaid-config.json -b white
```

The config is not decoration. It sets `htmlLabels: false`, which makes mermaid
emit real `<text>` elements instead of `<foreignObject>` wrappers around HTML.
GitHub serves an SVG in markdown through an `<img>`, and browsers do not render
foreignObject content in that context — the default output produces a diagram
whose every label is invisible on the page it was made for. The white
background is for the same reason: the edge labels sit on a half-transparent
light chip that disappears into GitHub's dark theme.

Keep the two files in step. A diagram that disagrees with its source is worse
than no diagram, because nobody can tell which one is stale.

## Shape of the thing

One process. One thread does the matching. Everything else is arranged so that
the matching thread never has to wait for anything, and so that what it did can
be reconstructed exactly.

```
apps/            exchange_sim, xc_client, replay, xc_info
  venue.cpp      composition: wires the parts together, owns no logic
    net/         event_loop, tcp_gateway, udp_feed
    protocol/    messages, gap_detector
    core/        matching_engine -> order_book -> price_level, order_pool
    risk/        risk_engine, kill_switch, limits, exposure
    journal/     writer, reader, replayer, record
    util/        crc32c, byte_codec, histogram, flat_hash_map, node_pool, digest
```

Dependencies point inward only. `core/` knows nothing about sockets, journals
or risk; the engine holds pointers to a risk engine, a kill switch and a
journal, any of which may be null, and `venue.cpp` is the only place that
decides what is plugged in. That is why the order book can be diffed against a
reference model in a unit test with no venue at all, and why the engine can be
replayed from a journal with no sockets.

## The command path

A `NewOrder` arriving on a socket passes through these stages in this order.
The order is the design; most of it is about what must happen before the
command is given a number.

| # | stage | can reject with | where |
|---|---|---|---|
| 1 | frame and decode | connection closed | `protocol/messages.cpp` |
| 2 | account pinning | connection closed | `net/tcp_gateway.cpp` |
| 3 | instrument lookup | `UnknownInstrument`, `UnknownOrder` | `matching_engine.cpp` |
| 4 | clock read | — | `core/clock.hpp` |
| 5 | kill switch | `Halted` | `risk/kill_switch.cpp` |
| 6 | pre-trade risk | `RiskLimit`, `PriceCollar`, `RateLimit` | `risk/risk_engine.cpp` |
| **7** | **sequence number assigned** | — | `matching_engine.cpp` |
| 8 | journal append | `Halted` | `journal/writer.cpp` |
| 9 | match | `DuplicateOrderId`, `InvalidPrice`, `InvalidQuantity`, `FillOrKillUnfillable`, `PostOnlyWouldCross`, `SelfTrade` | `core/order_book.cpp` |
| 10 | risk settlement | — | `risk/risk_engine.cpp` |
| 11 | listener notification | — | `venue.cpp` |

### Why the sequence number is assigned at step 7

Everything above step 7 can turn a command away without touching the book, and
nothing above it is recorded. Everything from step 7 down is numbered and
written.

Assigning the number earlier would consume one for every rejected command, so
the journal's numbering would have holes in it. A hole in a journal has to mean
data loss — that is the only reading that makes recovery safe. If it can *also*
mean "that one was rejected", then recovery cannot distinguish a lost record
from a rejected command, and the one guarantee the journal exists to provide is
gone. `JournalReader` reports `SequenceGap` for exactly this reason, and it
would fire constantly on a journal numbered any other way.

The corollary, which is easy to get wrong: `add_instrument` consumes a sequence
number **unconditionally**, whether or not a journal is attached. Otherwise a
journalled run and an un-journalled run of the same commands would number them
differently, and replay would not reproduce the original.

### Why the journal is written before the match

A record written after matching describes something that already happened. If
the process dies between the two, the journal is missing an event the world may
already have seen — a fill that reached a client over the wire but exists
nowhere durable. Writing first means the opposite failure: a record for a
command that may not have been matched. That one is recoverable, because replay
is deterministic and re-executing the command produces the same result.

### What step 10 settles

Self-trade prevention can remove or shrink a **resting** order during someone
else's command, with no fill and no cancel request involved. The resting
order's owner never asked for anything, so nothing in the ordinary paths
releases their risk exposure, and it would leak permanently — an account would
slowly lose the ability to trade because of orders that no longer exist.

`OrderBook::submit` therefore reports those removals through an optional
`std::vector<Withdrawal>*`, and `settle_risk` releases exposure against it.
This was a live bug, found while wiring risk into the engine.

## Threading

**The engine is single-threaded, and that is a design decision rather than an
unfinished one.**

Determinism depends on it. Replay has to reproduce a bit-identical state
digest, and that requires that commands were applied in one defined order with
no interleaving to reconstruct. A sharded engine could keep that property per
instrument; one with a shared book and locks could not.

The transport is single-threaded too. `EventLoop` multiplexes sockets with
`kqueue` on macOS and `epoll` on Linux, level-triggered, rather than spawning a
thread per connection. One slow client therefore cannot block another, and
there is no shared mutable state between connections to protect.

The only concurrent code in the repository is `bench/bench_wire.cpp`, which
runs the venue on one thread and an open-loop load generator on another so the
generator is never blocked by the system it is measuring. That is what the
ThreadSanitizer CI job checks, because it is the only thing TSan has to look
at.

What this costs is in [limitations.md](limitations.md): one process, one core
for matching, no horizontal scaling, no failover.

## Data structures, and why these

| structure | where | why not the obvious thing |
|---|---|---|
| `std::map<Price, PriceLevel>` with a pooled node allocator | `order_book.cpp` | Prices are sparse and the best price must be found in O(1); a sorted vector moves on every new level. `std::map` allocates a node per level, so the allocator is a `NodePool` sized from the instrument. |
| Intrusive doubly-linked list of orders within a level | `price_level.cpp` | Price-time priority needs FIFO per level and O(1) removal from the middle for cancels. Intrusive links mean no per-order container node. |
| Slab + free list, 32-bit handles | `order_pool.cpp` | Orders are fixed size and churn constantly. A handle is half a pointer, so an index entry is smaller and the book's hot structures fit in less cache. |
| `FlatHashMap<OrderId, OrderHandle>` | `order_book.cpp` | `std::unordered_map` allocates a node per insert — `reserve` sizes buckets, not nodes — so the order index allocated on every resting order. Open addressing in one array allocates once. |
| Log-linear histogram | `util/histogram.hpp` | Latency spans orders of magnitude and the tail is the point. Linear buckets fine enough for a sub-microsecond median need millions to reach a millisecond outlier; keeping every sample makes a percentile a sort. |

The zero-allocation claim on the matching path is asserted by tests, not
asserted in prose — and it was false when first measured. The sequence
50,002 → 23,780 → 26 → 0 allocations per 50,000 orders is in
[benchmarks.md](benchmarks.md), along with what fixed each step.

## Durability and replay

`JournalWriter` appends CRC-32C-framed records to segment files, rotating by
size. Three durability policies: `None`, `Interval`, `Always`. On macOS
`Always` uses `F_FULLFSYNC`, which costs about 2.5 ms per command — plain
`fsync` would look far better in a benchmark and would not actually survive a
power cut.

`JournalReader` distinguishes a **torn tail** — a record cut short by a crash,
which is expected and recoverable by truncation — from **damage** in the middle
of a segment, which is not. It stops at the first corrupt record rather than
reading past it, because a CRC failure means the next length field cannot be
trusted either.

`Replayer` drives a fresh engine from the journal with a `ManualClock` fed from
the recorded timestamps, so nothing in replay reads the wall clock. The
resulting state is compared by FNV-1a digest against the original. See
[determinism.md](determinism.md).

## Where the invariants live

| invariant | enforced in | proved by |
|---|---|---|
| The book never crosses | `order_book.cpp` | `test_invariants.cpp` |
| Quantity is conserved across every fill | `order_book.cpp` | `test_invariants.cpp` |
| A level's total equals the sum of its orders | `price_level.cpp` | `test_invariants.cpp` |
| Matching agrees with the specification | — | `test_differential.cpp` against `model/reference_book.cpp` |
| Replay reproduces state exactly | `replayer.cpp` | `test_replay.cpp` |
| The matching path allocates nothing | — | `test_allocations.cpp` |
| Risk exposure returns to zero | `risk_engine.cpp` | `test_engine_risk.cpp` |

The reference model in `tests/model/` is a deliberately naive O(N²) matcher
that exists to be obviously correct rather than fast. It is the executable
specification: when it and the real book disagree on randomised flow, one of
them is wrong, and three of the project's bugs were found that way.
