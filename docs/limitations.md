# Limitations

This document exists so that nothing in this repository has to be read
generously. It is linked from the README and is kept current as the system
grows — an item is removed only when it stops being true, never because it
became inconvenient.

## What this system is

A single-node exchange core: a price-time-priority limit order book, a matching
engine, pre-trade risk checks, a write-ahead journal with deterministic replay,
a binary wire protocol, and a TCP/UDP gateway that speaks it. It is built to be
correct, measurable, and reproducible.

## What this system is not

**Not a production trading venue.** There is no clearing, no settlement, no
regulatory reporting, no market-maker obligation tracking, no fee schedule, no
audit trail suitable for a regulator, and no operational tooling of the kind a
real venue needs. It has never handled real money or real customer orders.

**Not a trading strategy, and not a source of profit.** There is no PnL, no
backtest, no signal, and no claim of profitability anywhere in this repository.
A matching engine is infrastructure; it takes the other side of nobody's trade.

**Not colocated-HFT latency.** Every timing number here is measured on a single
developer machine, with the client and the exchange on loopback. Loopback
elides the network interface, the switch, the cable, and the kernel bypass
stack that dominate real trading latency. These numbers describe the cost of
this software's logic, not the latency of a trading system. They are not
comparable to published exchange or vendor figures, and no such comparison is
made.

**Not multi-threaded matching.** The matching core is deliberately
single-threaded, because determinism is worth more here than parallelism.
Throughput scales by symbol partitioning, not by threading a single book.

**Not hardware-accelerated.** No kernel bypass, no DPDK, no FPGA, no
busy-poll NIC configuration.

## Data

All order flow in this repository is **synthetic**. Where it is derived from
public OHLCV bars, that means real historical *prices* driving *invented*
order-by-order flow — actual exchange order flow is not public data and is not
present here. See [data.md](data.md) for exactly how each dataset is produced.

## Measurement caveats

- Numbers are produced by `./scripts/run_bench.sh`, which records the CPU, OS,
  compiler, flags and build type alongside the results. Any figure quoted
  without that context should be treated as unverified.
- Latency is measured open-loop against intended send times, so that a stalled
  system cannot hide its tail behind reduced offered load.
- Hardware performance counters (cache misses, branch misses, IPC) require a
  bare-metal Linux host or macOS Instruments. CI runners virtualise the
  performance monitoring unit and do not report trustworthy counters, so no
  counter is published from a CI run.
- Benchmarks run on a developer machine with an active desktop. Absolute
  figures move with thermal state and background load; relative comparisons
  within a single run are the meaningful signal.

## Deliberately out of scope

FIX protocol support, iceberg and pegged orders, opening and closing auctions,
multi-leg and spread instruments, cross-venue routing, position netting across
accounts, and margin calculation. Each is a substantial system in its own
right, and stubbing them would add surface area without adding evidence.
