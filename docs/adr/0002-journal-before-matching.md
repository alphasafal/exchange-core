# 0002. Write the journal before matching, and journal inputs instead of outputs

- **Status:** Accepted
- **Date:** 2026-08
- **Related:** [`journal/writer.cpp`](../../src/journal/writer.cpp),
  [`replay/replayer.cpp`](../../src/journal/replayer.cpp),
  [determinism.md](../determinism.md),
  [benchmarks.md § journal durability](../benchmarks.md#journal-durability),
  [0001](0001-sequence-after-validation.md)

## Context

The venue has to survive `SIGKILL` and come back with the same book. Two
questions decide how:

1. **What gets written:** the commands that came in, or the results that came
   out (fills, acks, book changes)?
2. **When it gets written:** before the command touches the book, or after?

A wrong answer has a specific cost. If a client is told about a fill and the
venue then crashes, the fill must still exist after recovery. A fill the world
has seen but the venue has forgotten is the worst failure an exchange can have.

## Options considered

### A. Journal outputs, after matching

Write every fill and book change after it happens. Recovery replays the
outputs directly, so the engine doesn't need to be deterministic.

It loses on ordering. The result exists before it is durable. If the process
dies after a fill goes out over the wire but before the record is written, a
client holds a fill the venue has no record of. Closing that window means
holding every outbound message until its record is durable, which puts the
disk on the latency path of every reply. It also writes several records per
command (one per fill, plus acks and level changes) instead of one.

### B. Journal inputs, before matching (chosen)

Write the command, then match it. Recovery re-executes the commands.

The failure case is now a record for a command that may not have been matched
before the crash. That one is recoverable: replay re-executes it and gets the
same result, provided the engine is deterministic. One record per command,
whatever it produces.

### C. Journal inputs and outputs

Record the command before matching and the results after. It's the most
robust, because recovery can cross-check, but it costs the most write volume
and still needs determinism for the input half. I kept it as the upgrade path
for when an independent check against recorded outputs is worth the bytes.

## Decision

Journal each command (stage 8) after it gets its sequence number and before
the matcher sees it (stage 9). Recovery re-executes the journal through a fresh
engine.

## Consequences

- **The engine must be deterministic, and this is the main thing the decision
  costs.** That is why:
  - matching is single-threaded;
  - replay drives a `ManualClock` from the recorded timestamps instead of the
    wall clock;
  - sequence numbers are restored rather than reissued;
  - digests walk instruments in id order rather than hash order;
  - an instrument's price band is part of its journal record, because a
    replayed instrument with a default band would accept orders the original
    refused. That forced `kRecordVersion` to 2 when the ladder landed
    ([0003](0003-flat-price-ladder.md)).

  Every one of these was a bug or a near-bug, and [determinism.md](../determinism.md)
  lists them.
- **Durability has a price per policy.** Journaling costs ~190 ns per command
  buffered (75 → 264 ns). Interval persistence (100 ms) costs another ~20 ns.
  Persisting every record with `F_FULLFSYNC` costs 2.5 ms per command, so 400
  commands/s. The engine lets the operator pick, and the benchmark publishes all
  three instead of the flattering one.
- **A torn tail costs exactly the last command.** The reader treats a record
  cut short at the end of the last segment as a crash and truncates it, and
  treats damage anywhere else as corruption and stops.

## Open question

> **Resolved by [0004](0004-increasing-order-ids.md)**, which also found the
> case was worse than described here: a resend of an order that had already
> *filled* executed a second time.

**A command journaled but not acknowledged before a crash.** After recovery,
the order exists: replay re-executes it. But the client never received an ack,
so it might resend, or assume the order failed. A real venue resolves this with
client order ids that are idempotent across reconnects and an order-status
query on reconnect. exchange-core has neither. The window is small, the same
size as the gap between stages 8 and 11, but it is a real gap and it is not yet
in [failure-modes.md](../failure-modes.md).

## Evidence

- [`Replay.RecoversToTheLastCompleteCommandAfterATornTail`](../../tests/test_replay.cpp)
  and [`Replay.ATornTailCostsExactlyTheLastCommand`](../../tests/test_replay.cpp).
- [`JournalIo.RecoversEveryCompleteRecordBeforeATornTail`](../../tests/test_journal_io.cpp).
- The digest tests in `test_replay.cpp`: a replayed engine reproduces the
  original state bit for bit.
- [`scripts/demo.sh`](../../scripts/demo.sh): starts a venue, trades, kills it
  with `SIGKILL`, replays, and fails if the digests differ.
- [`fuzz/fuzz_journal.cpp`](../../fuzz/fuzz_journal.cpp): recovery over corrupted segments.

## Revisit if

- **The engine becomes multi-threaded or sharded.** Determinism across threads
  would need a sequencer in front, and the journal would move to that
  sequencer.
- **Recovery time matters.** Replaying from the beginning grows with history.
  Periodic snapshots plus a journal tail would bound it, and would need the
  snapshot to be covered by the digest.
- **Option C's cross-check becomes worth the write volume**, for example when
  there is a second, independent implementation to compare against.
