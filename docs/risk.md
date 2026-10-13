# Risk model and kill switch

Everything here is **pre-trade**: it runs before an order reaches the book and
rejects it outright. That ordering is the design, not an implementation detail.
A limit enforced after matching is not a limit, it is a report — the trade has
happened, the position exists, and the only remaining question is how to unwind
it.

Where these sit in the command path, and why they sit before the sequence
number is assigned, is in [architecture.md](architecture.md).

## Per-account limits

Every limit defaults to unlimited, so adding a new one to `AccountLimits`
cannot silently start rejecting an existing account's orders. An operator opts
into each constraint deliberately.

| limit | unit | catches |
|---|---|---|
| `max_order_quantity` | lots | a mistyped quantity |
| `max_order_notional` | ticks × lots | a large order on an expensive instrument |
| `max_open_orders` | count, across instruments | a runaway quoting loop, and the memory one account can occupy in the book |
| `max_position` | lots, per instrument | position accumulation |
| `max_messages_per_second` | messages | a client saturating the matching thread |

Quantity and notional are both kept because they catch different mistakes. A
quantity limit alone lets an enormous order through on a high-priced
instrument; a notional limit alone lets a huge quantity through on a cheap one.

### Position is checked against the projected position

`max_position` is checked against the position the account **would** hold if
everything it currently has working were to fill — not against the position it
holds now.

An account that is flat with a thousand lots of resting bids is one fill away
from being long a thousand. Treating it as flat would let it build an unlimited
position one resting order at a time, each of which looks harmless on its own.
This is the single most load-bearing choice in the file.

## Price collar

`InstrumentControls::collar_bps` bounds how far from the reference price an
order may be priced. Zero disables it.

This is the check that stops a misplaced decimal point from sweeping the book,
and it protects the market rather than the account — which is why it lives on
the instrument and not on the account. A fat-fingered price hurts everyone
resting on the book.

It **rejects** rather than adjusts. Silently moving a client's price to the
edge of the collar is a worse failure than refusing the order: the client
believes it has a resting order where it put one, and it does not.

The comparison is done in bounded 64-bit arithmetic rather than with
`__int128`. GCC rejects `__int128` under `-Wpedantic`, which the project builds
with, so the bound is checked against `kMax / kBasisPoints` before multiplying
instead of being computed in a wider type that is not portable. That was a real
CI failure, not a hypothetical.

## Rate limiting

A generic cell rate algorithm: an average rate (`max_messages_per_second`) with
a burst allowance (`message_burst`).

Real order flow is bursty. A market maker requoting a whole ladder after a
price move sends a dozen messages in a microsecond and then nothing for a
second. A limiter with no burst tolerance rejects exactly the legitimate
behaviour it should permit.

**Budget is charged even for rejected messages.** A client that is being
rejected is usually a client that is malfunctioning, and a limiter that only
charges for accepted work lets a malfunctioning client consume the matching
thread for free — which is the case the limiter exists for.

This is the one control that protects the venue rather than the account. A
client stuck in a quote-cancel loop adds latency to everyone else's orders
without ever breaching a position or notional limit.

## Kill switch

Two scopes: the whole venue, and one account.

### A halt blocks new orders. It never blocks cancellation.

That asymmetry is the most important property here, and getting it backwards
turns a safety control into the cause of the loss.

An account is halted precisely when something has gone wrong — which is exactly
when it most needs to withdraw the orders it already has resting. A switch that
froze an account's existing quotes would leave them exposed to the market with
no way to pull them: the operator would have stopped the account from reducing
its risk while leaving every position it already had live.

Halting with cancel-on-trip goes further and actively pulls the account's
resting orders rather than stranding them.

### Halts are sticky

Nothing resumes on its own, including an automatic trip. Whatever caused it has
not been diagnosed yet, and a control that un-trips itself flaps through the
same failure repeatedly.

### Trip reasons

| reason | |
|---|---|
| `Manual` | an operator pulled it |
| `RepeatedRejects` | tripped automatically after consecutive rejected orders |
| `RiskBreach` | tripped automatically after a limit was breached |

The consecutive-reject trip exists because a client sending a steady stream of
rejected orders is not a client having an unlucky moment; it is a client
operating on a view of the world that disagrees with the venue's, and the
cheapest safe response is to stop it.

## What risk does *not* do

**It does not cancel on its own**, outside cancel-on-trip. Nothing in the risk
engine reaches into the book during ordinary operation.

**It does not know about self-trade prevention's effects**, which is why STP
removals are reported back as `Withdrawal`s and released explicitly — see
[architecture.md](architecture.md). STP can shrink a resting order during
someone else's command, with no fill and no cancel request, and the exposure
would otherwise leak permanently.

**It is not margin.** There is no collateral, no mark-to-market, no funding, no
cross-instrument netting and no portfolio view. Position is tracked per
account per instrument as a signed lot count, and that is all.

**It is not reconstructed on replay.** Commands refused by risk never reached
the book and were never journaled, so replay does not install a risk engine at
all. See [determinism.md](determinism.md).

## Where it is tested

| | |
|---|---|
| `tests/test_risk_limits.cpp` | each limit, at and either side of its boundary |
| `tests/test_risk_collar_and_rate.cpp` | collar arithmetic, GCRA burst and refill, charging on reject |
| `tests/test_kill_switch.cpp` | venue and per-account halts, cancellation still permitted, stickiness, auto-trip |
| `tests/test_engine_risk.cpp` | the engine wiring: exposure acquired and released across submit, cancel, replace, fill and STP withdrawal |
