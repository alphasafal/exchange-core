# Wire protocol

Version 1. This document is the specification: it is complete enough to write
an independent client against, and `tests/test_protocol.cpp` holds the decoder
cases that keep it honest.

Two transports carry the same framing.

| | transport | direction | reliability |
|---|---|---|---|
| Order entry | TCP | client ↔ venue | ordered, no loss |
| Market data | UDP | venue → subscribers | may drop, duplicate, reorder |

## Frame

Every message, on both transports, begins with the same 12 bytes.

```
offset  size  field
     0     2  payload length, excluding this header
     2     1  message type
     3     1  protocol version
     4     8  sequence number
```

The length comes first so a reader can frame the stream before it interprets
anything. The sequence number is in the header rather than the payload so gap
detection never has to parse a body — a subscriber can spot a gap in a message
type it does not implement.

All integers are **little-endian, fixed width, unaligned**. Little-endian
because every machine this runs on is, so a byte order chosen for tradition
would cost a swap on every field. Fixed width because a variable-length
encoding makes a message's size depend on its values, and that turns a size
bound into a thing you have to reason about rather than check. Unaligned
because the reader copies field by field and never casts a pointer into the
buffer, which also means the decoder has no alignment requirement on the
receive buffer.

`payload_length` is bounded at **8192** (`FrameHeader::kMaxPayload`). A reader
rejects anything larger without allocating for it.

`version` is checked on every frame. A peer that meets a version it does not
know closes the connection instead of guessing at the layout.

## Message types

| value | type | direction |
|---|---|---|
| 1 | `NewOrder` | client → venue |
| 2 | `CancelOrder` | client → venue |
| 3 | `ReplaceOrder` | client → venue |
| 16 | `OrderAccepted` | venue → the client that sent the command |
| 17 | `OrderRejected` | venue → that client |
| 18 | `OrderCancelled` | venue → that client |
| 19 | `OrderReplaced` | venue → that client |
| 20 | `Execution` | venue → that client |
| 32 | `Trade` | venue → every subscriber |
| 33 | `TopOfBook` | venue → every subscriber |
| 34 | `DepthSnapshot` | venue → every subscriber |
| 48 | `Heartbeat` | venue → every subscriber |

The numbering is grouped rather than consecutive — 1–3 inbound, 16–20 session
replies, 32–34 market data, 48 session-level — so a new message in one group
never has to be given a number that sorts it among another group's.

Any other value is `Unreadable`. There is no "ignore unknown types" rule on
order entry: a client sending a type the venue does not implement is not a
client whose other messages should be trusted.

## Payloads

Field order is the order the bytes appear. `i64` is two's-complement signed.

### `NewOrder` (1) — 44 bytes

| size | field | notes |
|---|---|---|
| u64 | `id` | client-assigned, must be unique per account |
| u64 | `account` | |
| u64 | `instrument` | |
| u8 | `side` | 0 Buy, 1 Sell |
| u8 | `type` | 0 Limit, 1 Market |
| u8 | `tif` | 0 Day, 1 IOC, 2 FOK, 3 GTC |
| u8 | `post_only` | 0 or 1 |
| i64 | `price` | in ticks; ignored for Market |
| u64 | `quantity` | in lots |

### `CancelOrder` (2) — 24 bytes

`u64 id`, `u64 account`, `u64 instrument`.

### `ReplaceOrder` (3) — 40 bytes

`u64 id`, `u64 account`, `u64 instrument`, `i64 new_price`, `u64 new_quantity`.

### `OrderAccepted` (16), `OrderCancelled` (18), `OrderReplaced` (19) — 48 bytes

The same acknowledgement payload in all three cases; the type says which event
it is.

| size | field |
|---|---|
| u64 | `order` |
| u64 | `account` |
| u64 | `instrument` |
| u64 | `filled` |
| u64 | `remaining` |
| i64 | `timestamp` (nanoseconds) |

### `OrderRejected` (17) — 25 bytes

`u64 order`, `u64 account`, `u8 reason`, `i64 timestamp`.

`reason` values:

| | | | |
|---|---|---|---|
| 0 `None` | 1 `UnknownInstrument` | 2 `UnknownOrder` | 3 `DuplicateOrderId` |
| 4 `InvalidPrice` | 5 `InvalidQuantity` | 6 `InvalidSide` | 7 `FillOrKillUnfillable` |
| 8 `PostOnlyWouldCross` | 9 `SelfTrade` | 10 `RiskLimit` | 11 `PriceCollar` |
| 12 `Halted` | 13 `RateLimit` | | |

### `Execution` (20) and `Trade` (32) — 74 bytes

Identical payloads, different audiences. `Execution` goes to the client whose
order was involved; `Trade` goes to every market data subscriber.

| size | field | notes |
|---|---|---|
| u64 | `id` | trade id |
| u64 | `instrument` | |
| i64 | `price` | the resting order's price — the aggressor gets the resting side's price, which is where price improvement comes from |
| u64 | `quantity` | |
| u64 | `aggressor_order` | |
| u64 | `resting_order` | |
| u64 | `aggressor_account` | |
| u64 | `resting_account` | |
| u8 | `aggressor_side` | |
| u8 | `resting_filled` | 1 if the resting order is now fully filled and gone |
| i64 | `timestamp` | |

`Trade` carries both account ids, which a real venue would not publish. This
is a simulator; `docs/limitations.md` says so.

### `TopOfBook` (33) — 40 bytes

`u64 instrument`, `i64 bid_price`, `u64 bid_quantity`, `i64 ask_price`,
`u64 ask_quantity`.

An empty side is `price = 0` (`kNoPrice`) with `quantity = 0`. Zero is not a
valid price, so it does not collide with a real level.

### `DepthSnapshot` (34) — variable

```
u64  instrument
u16  bid level count
     bid levels, best first:  i64 price, u64 quantity, u32 order_count
u16  ask level count
     ask levels, best first:  i64 price, u64 quantity, u32 order_count
```

12 bytes with both sides empty, plus 20 per level.

Each count is bounded at **256** (`kMaxDepthLevels`). A length-prefixed
repeating group is where a malformed message does the most damage: a count
claiming four billion levels would have a naive decoder reserve memory for
them before discovering the bytes were never there. The bound is checked
*before* anything is reserved.

### `Heartbeat` (48) — 0 bytes

Header only. Sent on an otherwise idle feed, because without it a subscriber
cannot tell a quiet market from a dead socket and will sit contentedly on a
feed that stopped an hour ago. It carries the current sequence number in its
header, so a subscriber that missed the last message before the market went
quiet still finds out.

## Decoding

`decode` returns one of three statuses, and the distinction between the last
two is the whole design:

| status | meaning | what a reader does |
|---|---|---|
| `Ok` | a complete message was decoded | process it |
| `Incomplete` | the bytes so far are a valid prefix | wait for more |
| `Unreadable` | these bytes cannot be a message | close the connection |

`Incomplete` is routine on TCP, which delivers bytes rather than messages. A
reader that treated a partial message as an error would fail on nothing worse
than a segment boundary.

`Unreadable` is fatal *by design*. There is no resynchronisation: once the
framing is wrong there is no reliable way to find the start of the next
message, and a decoder that goes hunting for a plausible-looking header will
eventually find one inside a payload and resume reading garbage with
confidence. Closing the connection is the only honest response.

Reads use `ByteReader`, whose failure is **sticky**: the first read that would
run past the end of the buffer sets a flag, and every subsequent read is a
no-op returning zero. Nothing has to be checked field by field, and a decoder
cannot act on a value that was never in the message. A message also has to be
`fully_consumed()` — trailing bytes inside a frame mean the sender and this
decoder disagree about the layout, which is exactly as bad as a short message.

Enum fields are decoded through `reader.enumeration<T>(max)`, which rejects
any value above the highest the build knows. An out-of-range enum that reached
the engine would be undefined behaviour in the first `switch` that saw it.

## Sequence numbers and gaps

Order entry runs over TCP, so loss is not possible and the sequence number is
there for correlation and journalling.

Market data runs over UDP, which may drop, duplicate or reorder. None of that
can be prevented, so the protocol makes it **detectable**. `GapDetector`
classifies every message:

| | |
|---|---|
| `InOrder` | the expected sequence. Process it. |
| `Gap` | messages are missing. The subscriber's book is now wrong and no incremental update can repair it, because an increment describes a change to a state the subscriber no longer has. Resynchronise from a snapshot, then apply the message that revealed the gap. |
| `Duplicate` | already seen. Dropped silently — applying an increment twice leaves a book that is wrong in a way nothing downstream can detect. |

On a gap the expectation jumps *past* the missing range rather than waiting for
messages that will never arrive. A subscriber still expecting a lost sequence
would classify the entire rest of the session as out of order.

Silent loss is the failure this exists to prevent. A subscriber trading on a
book that quietly diverged from the venue's is worse off than one that knows it
is blind, because it keeps acting with confidence on a view that is wrong.

## Not in this protocol

No authentication, no encryption, no logon sequence, no resend request, no
snapshot-on-demand, and no FIX compatibility. A client is identified by the
connection it arrives on, and the gateway pins one account per connection. See
[limitations.md](limitations.md).
