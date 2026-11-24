# 0001. Assign sequence numbers after validation, before the journal

- **Status:** Accepted
- **Date:** 2026-08
- **Related:** [`matching_engine.cpp`](../../src/core/matching_engine.cpp),
  [`journal/reader.cpp`](../../src/journal/reader.cpp),
  [architecture.md § command path](../architecture.md#the-command-path),
  [0002](0002-journal-before-matching.md)

## Context

Every command that changes the book needs a number. That number does three
jobs at once:

1. It gives a total order to replay in.
2. It sets time priority between resting orders at the same price.
3. It lets recovery find missing data: a gap between two journal records should
   mean something was lost.

The third job is the one that constrains where the number is assigned. Commands
can be turned away at several points before they reach the book: unknown
instrument, kill switch, risk limits, rate limits. So the question is which of
those rejections, if any, should use up a number.

## Options considered

### A. Number every command on arrival

Simple, and it gives a complete audit trail: every command that reached the
engine has a number, accepted or not.

It loses because of job 3. If rejected commands are numbered but not journaled,
the journal has holes that mean "rejected", and recovery can no longer tell a
lost record from a rejected command. The only way to keep gaps meaningful is to
journal the rejections too. That puts a write on the path of every command the
venue refuses, and during a rejection storm (a misbehaving client hitting a
rate limit) the journal grows at the attacker's rate. A rejected command never
touches the book, so it has no effect on state that replay would need to
reproduce.

### B. Number after every gate that rejects without touching the book (chosen)

Stages 1–6 can reject (decode, account pinning, instrument lookup, kill switch,
risk). None of them change the book, and none is recorded. Stage 7 assigns the
number, and everything from there on is numbered and journaled.

The journal then has no holes by construction, so a hole always means lost
data.

### C. No global sequence: per-session numbers plus timestamps

This is common in gateways, but it fails job 1 and job 2. Timestamps tie at
nanosecond resolution under load, and per-session counters give no order
between sessions. Replay would need a tie-break rule that the live engine
didn't use, so it couldn't be bit-identical.

## Decision

Assign the sequence number at stage 7, after every check that can reject
without touching the book, and immediately before the journal append.

## Consequences

- **Rejections before stage 7 leave no trace in the journal.** An audit of why
  an order was refused has to come from gateway logs, not from replay. A real
  venue would need a separate, non-authoritative rejection log. This one
  doesn't have one.
- **Rejections after stage 7 are numbered.** Matching can still reject, for
  example `PostOnlyWouldCross` or `SelfTrade`, because those depend on the
  book's state. They consume a number and are journaled, because replay has to
  reproduce the same rejection at the same point.
- **`add_instrument` takes a number unconditionally, journal or not.** If it
  didn't, a journaled and an unjournaled run of the same commands would number
  them differently, and replay couldn't be compared with the original. This was
  easy to get wrong and is the subtlest consequence of this decision.
- **Risk checks must not depend on anything that replay can't reconstruct.**
  They run before the number exists, so their results are never recorded. That
  works only because a risk rejection never changes state.

## Evidence

- [`Replay.ReportsAMissingSegmentAsASequenceGap`](../../tests/test_replay.cpp):
  a gap is reported as a gap.
- [`Replay.ARestartedEngineContinuesTheNumberingWithoutAGap`](../../tests/test_replay.cpp):
  numbering survives a restart.
- `test_replay.cpp` digest tests: replay reproduces a bit-identical state,
  which would fail if numbering differed between runs.

## Revisit if

- **Regulators or customers need a full audit trail of rejections.** Then add a
  separate rejection log. Rejections should still not get journal sequence
  numbers, which would bring back option A's problem.
- **The engine is sharded by instrument.** Each shard would need its own
  sequence, and the cross-shard order would have to come from a sequencer in
  front of the shards. That would make this record obsolete.
