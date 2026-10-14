---
name: Performance report
about: Something is slower than documented, or a measurement does not reproduce
title: ''
labels: performance
assignees: ''
---

## What you measured

<!-- Numbers from ./scripts/run_bench.sh, please, including the environment
     block it captures. A figure without its environment cannot be compared
     against the ones in docs/benchmarks.md. -->

```
```

## What you expected

<!-- Which figure in docs/benchmarks.md this disagrees with. -->

## Was the machine idle?

<!-- This matters more than it sounds. A package manager or a build running
     alongside the benchmark can move these numbers by nearly a factor of two,
     and it has happened while developing this project. -->

## Timer resolution

<!-- run_bench.sh reports the clock's resolution. If the difference you are
     reporting is smaller than one tick, it has not been shown to exist. -->
