---
name: Bug report
about: Something behaves differently from what the docs or tests say
title: ''
labels: bug
assignees: ''
---

## What happened, and what you expected instead

## How to reproduce

<!-- The most useful possible report is an input file. If you can get this down
     to bytes that break a fuzz target or the corpus runner, paste them or
     attach the file -- it goes into fuzz/corpus/ and becomes a regression test
     that runs on every platform. -->

```
commands, or the input, here
```

## Build

<!-- Paste the output of ./build/<preset>/apps/xc_info -- it reports the
     compiler, version, flags, build type, assertions and sanitizer that the
     binary was actually produced with, which is more reliable than describing
     them. -->

```
```

## Anything else

<!-- Does it reproduce under `ctest --preset asan`? Is it deterministic? -->
