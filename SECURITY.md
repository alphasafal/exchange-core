# Security

## What this software is

A single-node exchange core built to be read, measured and reproduced. **It has
never handled real money or real customer orders, and is not intended to.**
Please read [docs/limitations.md](docs/limitations.md) before deploying it
anywhere that matters — the short version is that it has no authentication, no
encryption, no clearing and no settlement.

The TCP gateway identifies a client by the connection it arrives on and pins one
account to it. That is the entire access control story. A login message that
checked nothing would be worse than none at all, because it would look like
security, so there is none.

## Reporting a vulnerability

Open a [draft security advisory](https://github.com/alphasafal/exchange-core/security/advisories/new)
rather than a public issue, and expect a reply within a week.

If you would rather not use GitHub, open a normal issue saying only that you
have something to report, with no detail, and I will arrange a private channel.

## What is in scope

The parts that read input somebody else controls, because those are the ones an
attacker reaches:

- the wire decoder (`src/protocol/`) — framing, bounds, enum validation;
- journal recovery (`src/journal/`) — behaviour on corrupted or hostile segment files;
- the gateway (`src/net/`) — connection handling, backpressure, account pinning;
- the risk engine and kill switch (`src/risk/`) — anything that lets an order past a limit.

Memory-safety bugs, infinite loops, unbounded allocation and undefined behaviour
on malformed input are all in scope even though this is not production software,
because the whole point of the fuzz targets is that those should not exist.

## What is out of scope

The absent features listed in `docs/limitations.md`: no authentication, no TLS,
no rate limiting at the connection level, no DoS resistance, no multi-node
anything. Reporting their absence is not a finding — it is documented, and
documented deliberately.

## What already runs against this

Every commit: AddressSanitizer and UndefinedBehaviorSanitizer over the suite,
clang-tidy as errors, and 60 seconds per fuzz target against a committed corpus.
Three libFuzzer targets cover the decoder on arbitrary bytes, order flow diffed
against a reference model, and journal recovery over corrupted segments.

If you find something these miss, the most useful thing you can include is the
input that triggers it — it goes straight into `fuzz/corpus/` and becomes a
regression test that runs on every platform.
