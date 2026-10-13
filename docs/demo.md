# Demo

**There is no recording in this repository.** There is a script that produces
one, and this document says how to record it. If you are reading this looking
for a video and cannot find one, that is the honest state of things rather than
a broken link.

The script is [`scripts/demo.sh`](../scripts/demo.sh). It runs unattended,
exits non-zero if anything it asserts fails, and takes about ninety seconds on
a warm build. Run it yourself before trusting anything below:

```bash
./scripts/demo.sh
```

It is deliberately the same script whether a person is watching or not. A demo
that only works when its author drives it is a video, not evidence.

## What it proves, in order

| # | step | what it is evidence of |
|---|---|---|
| 1 | build, then `xc_info` | the binary reports its own compiler, flags and build type — the script cannot misreport what it is running |
| 2 | `ctest` | 371 tests, including the differential harness against the reference model |
| 3 | start `exchange_sim` with `--durability always` | a real venue on a real socket, journalling every command |
| 4 | `xc_client` sends 200 orders | matching over the binary protocol, with acknowledgements coming back |
| 5 | `kill -9` | no shutdown, no flush, no cooperation |
| 6 | `replay` | every acknowledged command recovered, and a state digest |
| 7 | `replay --verify` | the digest reproduces |

## Storyboard — about two minutes

Timings assume a warm build; step 1 is much longer on a cold one, which is why
the recording below builds first and starts the capture after.

**0:00–0:12 — what this is.**
Title card or voice: a single-node exchange core in C++20. On screen:
`xc_info` output. Point at `compiler`, `build type`, `assertions`. The line to
say is that every number in this repository ships with the build that produced
it.

**0:12–0:30 — the tests.**
`ctest` scrolling to `371 tests passed`. Do not linger on the scroll; stop on
the summary. If you say one thing here, say that the suite contains an O(N²)
reference matcher that the real book is diffed against, and that three of the
project's defects were found that way — not by writing tests for behaviour that
already worked.

**0:30–0:50 — the venue is real.**
The `exchange_sim` banner: order entry port, market data port, instruments,
journal directory and durability policy. Then `xc_client` output: orders sent,
executions, quantity filled. This is a socket, a binary protocol and a matching
engine, not a function call in a test.

**0:50–1:05 — kill it.**
This is the shot that matters. `kill -9`, nothing graceful, no handler. Say
out loud that durability is set to `always` here, and that under the default
`interval` policy most of this run would simply be gone — the 100 ms timer
never fires in a demo this short. Being specific about that is more convincing
than the recovery itself.

**1:05–1:35 — recover.**
`replay` output: `recovery: clean`, `records: 201`, the instrument count, the
order count, the state digest, and the reconstructed book. Let the depth ladder
sit on screen for a beat — it is the thing that was rebuilt.

**1:35–1:50 — prove it twice.**
`--verify` passing. Say that this is the *weak* form of the check, a digest
equal to itself, and that `tests/test_replay.cpp` asserts the strong one: the
replayed digest equals the digest of the engine that produced the journal.
Claiming the weak check is the strong one is exactly the kind of thing this
project is trying not to do.

**1:50–2:00 — close.**
On screen: `docs/benchmarks.md`. Say that the latency figures there are p50
through p99.9 from a log-linear histogram, measured open-loop, with the CPU,
OS, compiler and flags recorded beside them — and that the timer on the machine
resolves 41.67 ns, which is stated there rather than hidden.

## Recording it

```bash
# asciinema produces a terminal cast: small, scrubable, copy-pasteable text.
brew install asciinema          # or: pipx install asciinema
cargo install --git https://github.com/asciinema/agg   # cast -> animated GIF

cmake --preset release && cmake --build --preset release -j   # warm the build first

asciinema rec docs/demo.cast \
    --cols 100 --rows 32 \
    --title "exchange-core: kill the venue, replay the journal" \
    --command ./scripts/demo.sh

agg --theme asciinema --font-size 16 docs/demo.cast docs/demo.gif
```

A cast is preferable to video for this: it is a few kilobytes, the text is
selectable, and nobody has to squint at a re-encoded terminal. If a GIF is
needed for a README, `agg` makes one from the same cast.

For a spoken version, record the voice separately over the cast's GIF rather
than capturing the screen with audio — the terminal output is deterministic and
the narration is not, and keeping them apart means a re-run does not cost a
re-record.

## If you re-record after changing anything

Re-run `./scripts/demo.sh` first and read its output. The script asserts; the
recording does not. A cast showing a passing run of a script that now fails is
worse than no cast at all.
