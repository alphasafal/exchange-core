# Contributing

Thanks for looking. This is a personal project, but it is built to the standard
where a stranger can verify everything in it, so contributions are welcome on
those terms.

## The bar

Every change has to keep three things true.

**CI stays green on every commit.** Twelve jobs: clang-format, GCC 13 and
Clang 18 on Linux and Apple Clang on macOS in Release and Debug, AddressSanitizer,
UndefinedBehaviorSanitizer, ThreadSanitizer, clang-tidy as errors, and a fuzz
smoke run. Not "green by the end of the branch" — green at each commit, because
a bisect through a red range tells you nothing.

**No claim outruns its evidence.** If a commit says something is faster, it
carries the measurement and the environment that produced it. If it says
something is correct, it carries the test. `docs/limitations.md` exists so that
nothing here has to be read generously, and it only ever loses an entry when the
entry stops being true.

**A bug fix ships with the test that fails without it.** Check that: revert the
fix, watch the test fail, put it back. Three of this project's defects were
found by the differential harness and each was fixed in the commit that found
it, with the regression test verified against the broken code first.

## Before you open a pull request

```bash
./scripts/format.sh --check          # pinned clang-format 17.0.6
cmake --preset release && cmake --build --preset release -j && ctest --preset release
cmake --preset debug   && cmake --build --preset debug   -j && ctest --preset debug
cmake --preset asan    && cmake --build --preset asan    -j && ctest --preset asan
```

If you touched the wire format, the journal or the book, also run the fuzzers
longer than CI's 60-second gate:

```bash
cmake --preset fuzz && cmake --build --preset fuzz -j
./build/fuzz/fuzz/fuzz_order_flow fuzz/corpus/order_flow -max_total_time=300
```

libFuzzer needs Clang, and on macOS its runtime ships with full Xcode rather
than the Command Line Tools. If you cannot build it, the corpus runners
(`ctest -R Corpus`) exercise the same oracles and run anywhere.

## Commit messages

Imperative subject under ~70 characters, conventional prefix (`feat`, `fix`,
`perf`, `refactor`, `test`, `docs`, `ci`, `chore`) with a scope where one helps.

The body explains **why**, and is where the value is. "Fixed a bug" tells the
next reader nothing; what they need is what the code did, why that was wrong,
and what would have happened in production. If a tool found it, say which one.

## Performance changes

A `perf` commit without a measurement is not a performance change, it is a
guess. It needs:

- before and after from `./scripts/run_bench.sh`, on an **idle** machine;
- the environment the script captures — CPU, OS, compiler, flags, build type;
- p50/p99/p99.9, not an average;
- an honest note if the win is inside measurement noise.

The timer on a typical development machine resolves tens of nanoseconds. A
change that moves a median by less than one tick has not been shown to do
anything. See `docs/benchmarks.md`.

## What will be turned down

Micro-optimisations with no measurement. Features that need a claim the tests
cannot back. Anything that makes the engine multi-threaded — determinism is
worth more here than parallelism, and `docs/architecture.md` explains why.
Dependencies, unless they earn their place loudly; the only one today is
GoogleTest, pinned to a tag.
