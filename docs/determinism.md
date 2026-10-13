# Determinism and replay

The claim is narrow and worth stating exactly, because the loose version of it
is not true.

> Replaying a journal into a fresh engine produces a bit-identical state
> digest, on the same binary, on the same machine.

`tests/test_replay.cpp` asserts it. It is not asserted by this document.

## What a digest covers

`digest(const OrderBook&)` fingerprints every resting order in matching order —
identity, ownership, side, price, quantity, remaining quantity, sequence — with
FNV-1a.

It deliberately **excludes derived state**: level totals, aggregated depth, top
of book. Including them would let the digest report a difference that no future
command could ever expose, which turns a determinism check into a check that
two implementations compute a cache the same way.

`digest(const MatchingEngine&)` covers the command sequence and every book,
walked in **instrument-id order**. The instruments are sorted explicitly rather
than iterated out of the hash map they live in: hash iteration order is
unspecified and can differ between runs of the same binary, which would make
the digest disagree with itself and turn the determinism test into a coin toss.

## What makes replay exact

Three things, each of which is a bug if it is got wrong.

**The clock is driven from the journal.** `Replayer` installs a `ManualClock`
and sets it from each record's recorded timestamp. Nothing in replay reads the
wall clock. A replay that did would stamp different times from the run it is
reconstructing, and the two could not be compared at all.

**Sequence numbers are restored, not reissued.** Queue priority is derived from
the sequence number a command was given. A replay that renumbered commands
would match every trade correctly and still leave the surviving resting orders
in a different order — a failure that produces correct trade history and a
wrong book, which is the hardest kind to notice.

**Risk is not reinstalled.** Commands refused by risk never reached the book
and were never journaled, so what the journal holds is exactly the stream the
book saw. Replay therefore does not need a copy of the risk configuration to be
exact, and cannot drift because that configuration changed since.

## The numbering rule that makes it work

The sequence number is assigned **after** every gate that can reject a command
without touching the book, and immediately **before** the command is journaled.
See [architecture.md](architecture.md) for where that sits in the path.

The consequence is that journal sequence numbers are gap-free. That is what
lets `JournalReader` treat a gap as `SequenceGap` — data loss — rather than as
an ambiguity. Numbered any other way, every rejected command would leave a hole
and recovery could not distinguish "that one was rejected" from "that one is
gone".

The corollary is easy to get wrong and was: **`add_instrument` consumes a
sequence number unconditionally**, whether or not a journal is attached.
Without that, a journalled run and an un-journalled run of the same commands
number them differently, and the digests do not match.

## What determinism does *not* mean here

It is not reproducibility across builds. The digest depends on the exact
sequence of resting orders, which depends on matching behaviour, which can
legitimately change between versions. Comparing a digest from one commit
against another is meaningless.

It is not reproducibility across machines in any sense beyond the above — the
digest is computed from engine state, not from timing, but nothing here
promises that two different compilers produce identical floating-point or
layout-dependent behaviour, because the engine deliberately contains no
floating point on the matching path.

It is not fault tolerance. Replay reconstructs what a single node did. There is
no second node, no consensus, and no failover; see
[limitations.md](limitations.md).

It says nothing about wall-clock timing. Replay reproduces *state*, not
latency. The timestamps it restores are the original run's, so a replayed run
tells you nothing about how long anything took.

## Reproducing it

```bash
cmake --preset release && cmake --build --preset release -j
./build/release/apps/exchange_sim --journal /tmp/xc-journal   # then send it orders
./build/release/apps/replay /tmp/xc-journal
```

`replay` prints the recovery outcome, the record counts and the state digest.
Running it twice over the same journal must print the same digest; that is the
cheapest version of the test, and `scripts/demo.sh` does exactly this as its
last step.
