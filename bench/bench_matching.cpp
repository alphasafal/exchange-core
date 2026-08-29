// Measures the matching engine in isolation: no sockets, no journal unless
// asked for, one thread.
//
//   bench_matching [--orders N] [--warmup N] [--levels N] [--seed N]
//                  [--journal DIR] [--durability none|interval|always]
//
// Runs the same workload twice, because one measurement cannot answer both
// questions honestly.
//
// Reading the clock costs tens of nanoseconds and these operations take
// hundreds, so a per-operation timestamp is a large fraction of what it
// measures. The throughput pass therefore never touches the clock inside the
// loop, and gives the true average cost per command. The latency pass times
// every command and gives the shape of the distribution -- including its tail,
// which is the part that matters and the part an average cannot show.
//
// The latency figures include the timer's own cost, and it is reported rather
// than subtracted: deducting an estimate from a measurement produces a number
// nobody else can reproduce.
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "xc/core/matching_engine.hpp"
#include "xc/journal/writer.hpp"
#include "xc/util/build_info.hpp"
#include "xc/util/histogram.hpp"

namespace {

using Clock = std::chrono::steady_clock;

std::uint64_t nanos_since(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

/// Cost of reading the clock, measured the same way everything else is.
///
/// Published rather than subtracted. These operations take hundreds of
/// nanoseconds and the timer costs tens, so the reader needs the figure to
/// judge the measurement -- but a benchmark that silently deducts an estimate
/// is reporting a number nobody else can reproduce.
xc::Histogram measure_clock_overhead(int samples) {
    xc::Histogram histogram(1'000'000, 10);
    for (int i = 0; i < samples; ++i) {
        const auto start = Clock::now();
        histogram.record(nanos_since(start));
    }
    return histogram;
}

void print_percentiles(std::string_view label, const xc::Histogram& histogram) {
    std::cout << "  " << std::left << std::setw(22) << label << std::right << " n=" << std::setw(9)
              << histogram.count() << "  p50=" << std::setw(7) << histogram.value_at(50.0)
              << "  p90=" << std::setw(7) << histogram.value_at(90.0) << "  p99=" << std::setw(7)
              << histogram.value_at(99.0) << "  p99.9=" << std::setw(8) << histogram.value_at(99.9)
              << "  p99.99=" << std::setw(9) << histogram.value_at(99.99)
              << "  max=" << std::setw(9) << histogram.max() << "  mean=" << std::fixed
              << std::setprecision(0) << histogram.mean() << std::defaultfloat << "\n";
}

void print_usage(const char* program) {
    std::cout << "usage: " << program << " [options]\n\n"
              << "  --orders N        commands to measure (default 1000000)\n"
              << "  --warmup N        commands before measuring (default 200000)\n"
              << "  --levels N        distinct price levels to spread across (default 40)\n"
              << "  --seed N          random seed (default 1)\n"
              << "  --journal DIR     journal to DIR while measuring\n"
              << "  --durability P    none | interval | always (default none)\n"
              << "  --help\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t order_count = 1'000'000;
    std::uint64_t warmup = 200'000;
    std::int64_t levels = 40;
    std::uint64_t seed = 1;
    std::string journal_directory;
    xc::journal::Durability durability = xc::journal::Durability::None;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--orders") {
            order_count = std::stoull(next());
        } else if (arg == "--warmup") {
            warmup = std::stoull(next());
        } else if (arg == "--levels") {
            levels = std::stoll(next());
        } else if (arg == "--seed") {
            seed = std::stoull(next());
        } else if (arg == "--journal") {
            journal_directory = next();
        } else if (arg == "--durability") {
            const std::string policy = next();
            if (policy == "none") {
                durability = xc::journal::Durability::None;
            } else if (policy == "interval") {
                durability = xc::journal::Durability::Interval;
            } else if (policy == "always") {
                durability = xc::journal::Durability::Always;
            } else {
                std::cerr << "unrecognised durability: " << policy << '\n';
                return 2;
            }
        } else {
            std::cerr << "unrecognised argument: " << arg << "\n\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    std::cout << xc::build_info().to_string();

    const xc::Histogram clock_overhead = measure_clock_overhead(100'000);
    std::cout << "\nclock\n";
    print_percentiles("steady_clock::now x2", clock_overhead);

    xc::SteadyClock clock;
    xc::MatchingEngine engine{clock};

    xc::Instrument instrument;
    instrument.id = xc::InstrumentId{1};
    instrument.symbol = "BENCH";
    instrument.tick_size = 1;
    instrument.lot_size = 1;
    instrument.min_quantity = 1;
    instrument.self_trade_policy = xc::SelfTradePolicy::Allow;
    // Sized from the workload, so growth does not appear as an outlier in the
    // tail of a measurement that is meant to be about matching.
    instrument.expected_resting_orders = static_cast<std::size_t>(order_count + warmup) / 2 + 4096;

    std::unique_ptr<xc::journal::JournalWriter> journal;
    if (!journal_directory.empty()) {
        journal = std::make_unique<xc::journal::JournalWriter>(
            xc::journal::WriterConfig{.directory = journal_directory, .durability = durability});
        if (!journal->open()) {
            std::cerr << "error: " << journal->last_error() << '\n';
            return 1;
        }
        engine.set_journal(journal.get());
    }
    engine.add_instrument(instrument);

    std::mt19937_64 rng(seed);
    std::uint64_t next_id = 1;

    const auto make_order = [&](std::uint64_t id) {
        xc::NewOrder command;
        command.id = xc::OrderId{id};
        command.account = xc::AccountId{1};
        command.instrument = xc::InstrumentId{1};
        command.side = (rng() % 2 == 0) ? xc::Side::Buy : xc::Side::Sell;
        command.type = xc::OrderType::Limit;
        command.price = 10'000 +
                        static_cast<xc::Price>(rng() % static_cast<std::uint64_t>(levels)) -
                        levels / 2;
        command.quantity = 10;
        return command;
    };

    // Warm up before measuring: the slab, the index, the level pool and the
    // instruction cache all reach steady state here, and a measurement that
    // included them would describe the venue's first second rather than its
    // working life.
    for (std::uint64_t i = 0; i < warmup; ++i) {
        engine.submit(make_order(next_id++));
    }

    xc::Histogram submit_latency;
    xc::Histogram cancel_latency;
    std::vector<std::uint64_t> resting_ids;
    resting_ids.reserve(order_count);

    // Pass one: throughput, with no clock read inside the loop at all.
    std::uint64_t untimed_commands = 0;
    const auto throughput_start = Clock::now();
    for (std::uint64_t i = 0; i < order_count; ++i) {
        const std::uint64_t id = next_id++;
        engine.submit(make_order(id));
        ++untimed_commands;

        if (!resting_ids.empty() && i % 4 == 3) {
            const std::size_t victim = rng() % resting_ids.size();
            engine.cancel(xc::CancelOrder{xc::OrderId{resting_ids[victim]}, xc::AccountId{1},
                                          xc::InstrumentId{1}});
            ++untimed_commands;
            resting_ids[victim] = resting_ids.back();
            resting_ids.pop_back();
        }
        if (engine.book(xc::InstrumentId{1})->find(xc::OrderId{id}) != nullptr) {
            resting_ids.push_back(id);
        }
    }
    const std::uint64_t elapsed_ns = nanos_since(throughput_start);

    // Pass two: the distribution, timed per command.
    for (std::uint64_t i = 0; i < order_count; ++i) {
        const std::uint64_t id = next_id++;
        const xc::NewOrder command = make_order(id);

        const auto start = Clock::now();
        const xc::SubmitOutcome outcome = engine.submit(command);
        submit_latency.record(nanos_since(start));

        if (outcome.rested) {
            resting_ids.push_back(id);
        }

        // Cancel an older resting order every few commands, so the measurement
        // reflects a book that is being worked rather than one that only grows.
        if (!resting_ids.empty() && i % 4 == 3) {
            const std::size_t victim = rng() % resting_ids.size();
            const xc::CancelOrder cancel{xc::OrderId{resting_ids[victim]}, xc::AccountId{1},
                                         xc::InstrumentId{1}};
            const auto cancel_start = Clock::now();
            engine.cancel(cancel);
            cancel_latency.record(nanos_since(cancel_start));
            resting_ids[victim] = resting_ids.back();
            resting_ids.pop_back();
        }
    }

    if (journal) {
        journal->close();
    }

    const xc::OrderBook* book = engine.book(xc::InstrumentId{1});
    const double seconds = static_cast<double>(elapsed_ns) / 1e9;

    std::cout << "\nthroughput, measured without a clock read in the loop\n"
              << "  price levels         " << levels << '\n'
              << "  commands             " << untimed_commands << '\n'
              << "  wall time            " << std::fixed << std::setprecision(3) << seconds
              << " s\n"
              << "  throughput           " << std::setprecision(0)
              << static_cast<double>(untimed_commands) / seconds << " commands/s\n"
              << "  cost per command     " << std::setprecision(1)
              << static_cast<double>(elapsed_ns) / static_cast<double>(untimed_commands) << " ns\n"
              << std::defaultfloat;

    std::cout << "\nlatency, nanoseconds per command, timed individually\n"
              << "  (includes the clock overhead reported above, which is not subtracted)\n";
    print_percentiles("submit", submit_latency);
    if (cancel_latency.count() > 0) {
        print_percentiles("cancel", cancel_latency);
    }

    std::cout << "\nbook at the end\n"
              << "  resting orders       " << book->resting_order_count() << '\n'
              << "  price levels         " << book->bid_level_count() + book->ask_level_count()
              << '\n'
              << "  slab high water      " << book->pool().high_water_mark() << '\n'
              << "  slab growth events   " << book->pool().growth_events() << '\n'
              << "  level pool chunks    " << book->level_node_pool().chunks() << '\n';

    if (journal) {
        std::cout << "\njournal\n"
                  << "  records              " << journal->records_written() << '\n'
                  << "  bytes                " << journal->bytes_written() << '\n'
                  << "  persist calls        " << journal->syncs() << '\n';
    }

    if (book->pool().growth_events() > 0) {
        // Said out loud rather than left in a counter nobody reads: growth
        // during a measured run means allocations landed in the tail.
        std::cout << "\nnote: the order slab grew during the run, so some samples include a\n"
                     "      reallocation. Raise --warmup or the instrument's expected order\n"
                     "      count to measure steady state.\n";
    }
    return 0;
}
