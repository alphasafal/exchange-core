# Failure modes

What goes wrong, how the system notices, what it does, and the test that says
so. A failure mode with no test next to it is a hope, so where there is no
test, this file says that instead of implying one.

Read with [limitations.md](limitations.md), which covers what the system does
not attempt at all.

## Durability

### Power loss or kill mid-write — torn tail

A record cut short by the process dying. **Expected**, and the ordinary case.

`JournalReader` reaches a record whose declared length runs past the end of the
file, or whose CRC-32C fails at the very end, and reports `TornTail` with
`good_bytes_in_damaged_segment` set to where the good data ends. Every complete
record before the tear is recovered and delivered.

Truncating the segment to that offset leaves a journal that reads `Clean`. That
is the contract: the report is an instruction that works, not advice.

→ `JournalIo.RecoversEveryCompleteRecordBeforeATornTail`,
`JournalIo.TruncatingToTheReportedOffsetLeavesACleanJournal`, and
`fuzz/fuzz_journal.cpp`, which asserts the truncation property on arbitrary
corrupted bytes.

### Corruption in the middle of a segment

A bad CRC that is not at the end of the file. **Not** recoverable by
truncation, because good records may follow it.

Recovery stops at the first corrupt record and reports `Damaged`. It does not
read past it: a CRC failure means the bytes are wrong, so the next length field
is wrong too, and a reader that trusted it would frame on garbage and deliver
records that were never written.

Truncation is reported but **not applied automatically**. Discarding records
that may be intact is an operator's decision.

→ `JournalIo.StopsAtACorruptRecordRatherThanReadingPastIt`

### A gap in the sequence numbering

Records 1, 2, 5. Means a record is **gone** — never "that one was rejected",
because rejected commands never consume a sequence number. See
[determinism.md](determinism.md).

Reported as `SequenceGap`.

→ `fuzz/fuzz_journal.cpp` asserts the delivered records are strictly
increasing; `test_replay.cpp` covers the numbering rule end to end.

### A stray file in the journal directory

`segment-00000003.xcj.bak` from a backup, or `segment-00000007.tmp` from a
half-finished copy.

Ignored. The segment-name parser requires the exact form — eight digits, no
sign, no whitespace, exact prefix and suffix, exact length.

**This was a bug.** The previous `sscanf("segment-%08u.xcj")` predicate
returned success as soon as the number was assigned and never checked the
trailing literal, so a backup was read as a live segment and recovery reported
a gap on a healthy journal. `segment--1.xcj` parsed as 4294967295, and the
writer's next roll wrapped to zero and created `segment-00000000.xcj` holding
the newest records — which the reader then sorted to the front and replayed
first.

→ `SegmentName.RejectsFilenamesThatOnlyLookLikeSegments`,
`JournalIo.IgnoresNonSegmentFilesWhenRecovering`,
`JournalIo.DoesNotTakeItsNextIndexFromANonSegmentFile`

### The journal directory does not exist

Reported as `Unreadable`, not as `Clean` with zero records. A missing journal
and an empty one are completely different situations, and conflating them
would let a misconfigured path look like a clean start.

→ `JournalIo.ReportsAMissingDirectoryRatherThanCallingItEmpty`

### A write fails mid-run

`write()` returning short or failing is handled rather than assumed away: a
short write that went unnoticed would leave a truncated record in the *middle*
of the journal rather than at its end, where recovery expects damage and knows
what to do with it.

The writer marks itself unhealthy and the engine rejects subsequent commands
with `Halted`. It does not continue accepting orders it cannot record.

→ `JournalIo.ReportsFailureRatherThanLosingRecordsSilently`

### An existing segment name is taken

`O_EXCL` on segment creation. An existing segment is never silently
overwritten; if the name is taken, something is wrong with the caller's
assumptions about the directory, and destroying a previous run's journal is the
worst possible way to find that out.

### Segment index exhaustion

Rolling past `kMaxSegmentIndex` (99,999,999) fails cleanly rather than growing
a ninth digit and breaking the symmetry with the parser.

→ `SegmentName.RoundTripsEveryRepresentableIndex`

## Transport

### A client sends bytes that are not a message

The connection is closed. There is deliberately no resynchronisation: once
framing is wrong there is no reliable way to find the next message boundary,
and a decoder hunting for a plausible-looking header will eventually find one
inside a payload and resume reading garbage with confidence. See
[protocol.md](protocol.md).

→ `fuzz/fuzz_protocol.cpp` on arbitrary bytes; `test_protocol.cpp` for the
specific malformed cases.

### A client claims a different account mid-session

The connection is closed. The gateway pins one account per connection — the
only thing resembling authentication that exists here.

→ `test_tcp_gateway.cpp`

### A slow consumer on order entry

A client that cannot read its own acknowledgements fast enough accumulates
unsent data. Past `max_outbound_bytes` (4 MiB) it is **disconnected**.

Buffering without limit would let one slow reader consume the venue's memory
until every other client suffers. Disconnecting contains the damage to the
party causing it.

→ `test_tcp_gateway.cpp`

### A slow or absent market data subscriber

Nothing happens. UDP does not care, and the feed does not track subscribers.
Loss is the subscriber's problem to **detect**, via the sequence number in
every frame header, and to repair by resynchronising from a snapshot.

Silent loss is the failure that matters: a subscriber trading on a book that
quietly diverged is worse off than one that knows it is blind.

→ `test_udp_feed.cpp`, `GapDetector` in `test_protocol.cpp`

### A quiet feed versus a dead feed

Indistinguishable without help, so the feed sends `Heartbeat` on an otherwise
idle socket, carrying the current sequence number in its header.

### A client disconnects with orders resting

**The orders stay.** Nothing cancels on disconnect.

This is a deliberate choice and arguably the wrong default for a real venue,
which would offer cancel-on-disconnect as a per-session option. It is recorded
here rather than silently either way.

## Engine and risk

### An account breaches a limit

Rejected pre-trade with the specific reason — `RiskLimit`, `PriceCollar`,
`RateLimit`. The order never reaches the book. See [risk.md](risk.md).

### An account sends a stream of rejected orders

The kill switch trips on consecutive rejects and halts the account. Halts are
sticky; nothing resumes on its own.

→ `test_kill_switch.cpp`

### An account is halted while holding resting orders

**Cancellation still works.** A halt blocks new orders only. Getting this
backwards turns the safety control into the cause of the loss — the account
would be unable to withdraw exposure at exactly the moment it most needs to.

→ `test_kill_switch.cpp`, `test_engine_risk.cpp`

### Self-trade prevention removes someone else's resting order

Reported back as a `Withdrawal` and the exposure released explicitly. Without
that the account's risk usage leaks permanently, because nothing it did
triggered a fill or a cancel.

**This was a live bug**, found while wiring risk into the engine.

→ `test_engine_risk.cpp`

### The order slab is exhausted

It grows. Refusing orders because of an internal capacity choice would be a
worse failure than a single reallocation.

Growth is counted in `growth_events()` so a benchmark can prove it reserved
enough and stayed at zero. Handles survive growth; **references into the slab
do not**, which is why the book links by 32-bit handle and never holds a
reference across `acquire()`.

→ `test_order_storage.cpp`, `test_allocations.cpp`

### A command arrives for an unknown instrument

Rejected with `UnknownInstrument` before anything else happens, and before a
sequence number is consumed.

## Measurement

### A sample above the histogram's range

**Counted, not clamped.** `overflow_count()` reports them, and `value_at`
returns the true maximum rather than a bucket ceiling when overflow is present.

A clamped outlier is indistinguishable from a sample that genuinely landed at
the top of the range, which would let the histogram quietly understate its own
worst case.

→ `test_histogram.cpp`

### The load generator cannot keep up with its own schedule

Reported. `bench_wire` records both the actual send time and the intended send
time and prints both rows, because a closed-loop harness that stalls with the
system it measures never samples the stall. At 20,000 orders/s that
understates p99.9 by **5.2×**. See [benchmarks.md](benchmarks.md).

### Allocation counting is unavailable

Under any sanitizer the counter is compiled out, because the sanitizer runtime
owns global `operator new`. The allocation tests **skip with the reason
printed** rather than observing zero allocations and passing.

That exact vacuous pass has happened once in this project, which is why
`allocation_counting_enabled()` exists.

## Not handled

Honestly, because a failure-modes document that lists only the handled cases is
marketing.

- **Disk full.** The write fails and the journal reports unhealthy, but there
  is no monitoring, no pre-emptive check and no graceful degradation.
- **Clock going backwards.** `steady_clock` is monotonic, so this is assumed
  away rather than defended against.
- **The process being killed between journal write and match.** Correct by
  construction — replay re-executes the command — but there is no test that
  kills a process at that exact point.
- **Memory exhaustion.** `std::bad_alloc` escapes to `main`, which now reports
  it and exits 1. Nothing attempts to continue.
- **Byzantine or malicious clients beyond malformed bytes.** There is no
  authentication; see [limitations.md](limitations.md).
- **Any failure requiring a second node.** There is no replication, no
  failover, no consensus, and no second node.
