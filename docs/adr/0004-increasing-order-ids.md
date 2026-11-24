# 0004. Make resends safe with increasing order ids per account

- **Status:** Accepted. It resolves the open question in [0002](0002-journal-before-matching.md#open-question).
- **Date:** 2026-11
- **Related:** [`matching_engine.cpp`](../../src/core/matching_engine.cpp),
  [`order_book.cpp`](../../src/core/order_book.cpp),
  [`tests/test_resend.cpp`](../../tests/test_resend.cpp),
  [protocol.md](../protocol.md), [0001](0001-sequence-after-validation.md)

## Context

Writing ADR 0002 exposed a case nothing handled. The venue journals an order,
then dies before it sends the acknowledgement. After recovery the order exists,
because replay re-executes it, but the client never heard about it. A client
that hasn't heard back does the natural thing and sends the order again.

It turned out to be worse than ADR 0002 said. Clients choose their own order
ids, and the book refused a duplicate id only while the original was still
resting. If the original had **filled** and left the book, the resend was
accepted as a new order and **executed a second time**. A test now
demonstrates exactly that on an instrument without this protection.

The fix had three constraints:

1. **It must survive replay.** The resend usually arrives at a recovered venue.
2. **It must not allocate per order or grow without bound.** The matching path
   is zero-allocation by test, and a venue runs all day.
3. **It must not slow the hot path.**

## Options considered

### A. Remember every order id the book has ever accepted

This catches every resend, and a status query could report exactly how each
order ended. It lost on constraint 2. Memory grows with the day's order count,
and the table rehashes, and so allocates, on the matching path, which breaks
a guarantee a test enforces. Bounding it with a window of recent ids would make
the guarantee quietly conditional, and that's worse than not having it.

### B. Session-level retransmission

The venue stores outbound messages and replays them from a client's last-seen
sequence on reconnect, the way FIX resend requests and SoupBinTCP work. This is
the complete production answer: the client gets the lost ack itself and has no
reason to resend. But it needs per-session outbound storage that survives a
crash, which means a second journal. That's a project of its own, and it
doesn't stop a buggy client from resending anyway.

### C. Require ids to increase per account, and keep one high-water mark each (chosen)

An order whose id isn't above the highest id that account has had accepted is
refused as `DuplicateOrderId`. The book stores one id per account, not one
per order, so memory scales with the number of accounts. That's small, known
in advance, and allocates only the first time an account trades. Several
binary exchange protocols impose exactly this rule on client order ids.

## Decision

Option C, opt-in per instrument (`Instrument::increasing_order_ids`) and
switched on in the venue simulator. It's opt-in so the matching core can still
be checked against the reference model on arbitrary ids. A read-only
`OrderStatusRequest` lets a reconnecting client ask where an order stands.

**Where the check lives** was decided by measurement, not by taste. The first
version put it inside `OrderBook::submit`. That made every submission about 12%
slower on the matching benchmark (median 206.6 vs 181.2 ns over eight
interleaved runs), **even with the option switched off**. I bisected this
before changing anything:

- The new `Instrument` field, on its own, cost nothing.
- Moving the new member to the end of the class didn't help.
- Marking the checked path cold and never-inlined didn't help.
- Forcing function and loop alignment left the gap, so it wasn't code
  placement.
- Restoring `OrderBook::submit`'s original one-line body, while keeping every
  other change, removed it.

`OrderBook::submit` is the innermost call on the hot path, and adding a second
branch to it was enough. The check moved up to `MatchingEngine::submit`, next
to the unknown-instrument, halt and risk gates. The final version measures
175.2 vs 173.5 ns, which is within noise.

That placement also turned out to be the right design. A resend changes
nothing, so it's refused **before** it's numbered, like every other refusal
that doesn't touch the book ([0001](0001-sequence-after-validation.md)). A
client resending a backlog after a reconnect adds no journal records.

## Consequences

- **`NotWorking` is all the status query can say about a finished order.**
  The book knows the id is at or below the account's mark and not resting. It
  can't tell whether the order filled, was cancelled, expired or was refused.
  The client's executions say whether anything traded. Option A could have
  told it more, at the cost described above.
- **Clients must generate increasing ids per account.** A client that reuses
  ids, or sends them out of order from several threads, will have orders
  refused. That's a protocol requirement, and [protocol.md](../protocol.md)
  states it.
- **The marks are state.** They decide future acceptances, so the replay
  digest covers them, and the instrument's flag is in the journal
  (`kRecordVersion` 3).
- **A refused order doesn't raise the mark.** It never executed, so the client
  may send it again.

## Evidence

- [`ResendDeathTest.CrashAfterJournalBeforeAckLeavesAResendSafeVenue`](../../tests/test_resend.cpp):
  a child process journals an order with `Always` durability and `_Exit`s
  inside the accept callback, after the journal and before any ack. The parent
  recovers from the journal, finds the order `Working`, and has its resend
  refused.
- [`Resend.AResentFilledOrderDoesNotExecuteTwice`](../../tests/test_resend.cpp):
  shows the double execution without the protection and its absence with it.
- `Resend.TheMarksSurviveReplay`, `Resend.IsRefusedWithoutBeingNumberedOrJournaled`
  and `StateDigest.CoversTheOrderIdMarks`.
- [`VenueEndToEnd.RefusesAResendAndAnswersAStatusQuery`](../../tests/test_venue_end_to_end.cpp):
  the client's side, over a real socket.
- Mutation checks: disabling either the refusal or the mark update fails five
  or more of these tests.

## Revisit if

- **Clients need the lost acknowledgement itself**, not just the order's
  state. That's option B, session retransmission, and it would sit alongside
  this rather than replace it.
- **The number of accounts gets large enough for the marks to matter.** It
  won't before the price ladder's per-instrument memory does
  ([0003](0003-flat-price-ladder.md)).
