// Runs bench_matching's workload through Liquibook, so the two engines can be
// compared on identical input rather than on numbers from different papers.
//
//   bench_liquibook [--orders N] [--warmup N] [--levels N] [--seed N]
//                   [--live-cancels]
//
// The workload is copied line for line from bench_matching.cpp: the same
// random sequence, the same price spread, one cancel for every four submits,
// and the same warm-up. If the two files drift apart the comparison is void,
// so change them together.
//
// What this does not make equal: exchange-core validates every order against
// its instrument (tick, lot, price band, self-trade policy) and Liquibook does
// not. That work is included in exchange-core's figures and absent from
// Liquibook's, so the comparison flatters Liquibook slightly.
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "book/order.h"
#include "book/order_book.h"
#include "book/order_listener.h"
#include "xc/util/histogram.hpp"

namespace {

using Clock = std::chrono::steady_clock;
namespace lb = liquibook::book;

std::uint64_t nanos_since(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}

// Liquibook's Order and OrderListener bases have non-virtual destructors, which
// this project's warning set rejects. Nothing here deletes through a base
// pointer, so the warning is silenced for these two derivations only.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
struct BenchOrder final : lb::Order {
    bool buy = false;
    lb::Price limit = 0;
    lb::Quantity quantity = 0;
    lb::Quantity open = 0;

    bool is_buy() const override { return buy; }
    lb::Price price() const override { return limit; }
    lb::Quantity order_qty() const override { return quantity; }
};

using Book = lb::OrderBook<BenchOrder*>;

// Keeps each order's open quantity, which is how the benchmark learns whether a
// submitted order rested. Liquibook reports fills through this listener.
struct FillTracker final : lb::OrderListener<BenchOrder*> {
    // Counted so the run can prove every cancel hit a resting order. A cancel
    // that missed would scan a whole level and fail, inflating the cancel cost
    // for a reason that is this harness's fault rather than Liquibook's.
    std::uint64_t cancelled = 0;
    std::uint64_t cancel_rejected = 0;

    void on_accept(BenchOrder* const&) override {}
    void on_reject(BenchOrder* const&, const char*) override {}
    void on_fill(BenchOrder* const& order, BenchOrder* const& matched, lb::Quantity qty,
                 lb::Price) override {
        order->open -= qty;
        matched->open -= qty;
    }
    void on_cancel(BenchOrder* const&) override { ++cancelled; }
    void on_cancel_reject(BenchOrder* const&, const char*) override { ++cancel_rejected; }
    void on_replace(BenchOrder* const&, const std::int64_t&, lb::Price) override {}
    void on_replace_reject(BenchOrder* const&, const char*) override {}
};
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void print_percentiles(std::string_view label, const xc::Histogram& histogram) {
    std::cout << "  " << std::left << std::setw(22) << label << std::right << " n=" << std::setw(9)
              << histogram.count() << "  p50=" << std::setw(7) << histogram.value_at(50.0)
              << "  p90=" << std::setw(7) << histogram.value_at(90.0) << "  p99=" << std::setw(7)
              << histogram.value_at(99.0) << "  p99.9=" << std::setw(8) << histogram.value_at(99.9)
              << "  p99.99=" << std::setw(9) << histogram.value_at(99.99)
              << "  max=" << std::setw(9) << histogram.max() << "  mean=" << std::fixed
              << std::setprecision(0) << histogram.mean() << std::defaultfloat << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t order_count = 1'000'000;
    std::uint64_t warmup = 200'000;
    std::int64_t levels = 40;
    std::uint64_t seed = 1;
    bool live_cancels = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--orders") {
            order_count = std::stoull(next());
        } else if (arg == "--warmup") {
            warmup = std::stoull(next());
        } else if (arg == "--levels") {
            levels = std::stoll(next());
        } else if (arg == "--seed") {
            seed = std::stoull(next());
        } else if (arg == "--live-cancels") {
            live_cancels = true;
        } else {
            std::cerr << "usage: " << argv[0]
                      << " [--orders N] [--warmup N] [--levels N] [--seed N] [--live-cancels]\n";
            return 2;
        }
    }

    // Every order the run will ever submit, allocated up front. Liquibook takes
    // orders by pointer, and allocating them inside the loop would charge
    // Liquibook for this harness's memory management rather than its own.
    std::vector<BenchOrder> orders(warmup + 2 * order_count + 1);

    Book book;
    FillTracker fills;
    book.set_order_listener(&fills);

    std::mt19937_64 rng(seed);
    std::uint64_t next_id = 1;

    const auto make_order = [&](std::uint64_t id) {
        BenchOrder& order = orders[id];
        order.buy = (rng() % 2 == 0);
        order.limit = static_cast<lb::Price>(
            10'000 + static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(levels)) -
            levels / 2);
        order.quantity = 10;
        order.open = 10;
        return &order;
    };

    for (std::uint64_t i = 0; i < warmup; ++i) {
        book.add(make_order(next_id++));
    }

    xc::Histogram submit_latency;
    xc::Histogram cancel_latency;
    std::vector<std::uint64_t> resting_ids;
    resting_ids.reserve(order_count);

    // Same victim selection as bench_matching: by default any id that once
    // rested, so filled orders are cancelled and rejected; with --live-cancels,
    // filled ids are dropped first, outside any timed region.
    const auto pick_victim = [&]() -> std::size_t {
        for (;;) {
            const std::size_t victim = rng() % resting_ids.size();
            if (!live_cancels || orders[resting_ids[victim]].open > 0) {
                return victim;
            }
            resting_ids[victim] = resting_ids.back();
            resting_ids.pop_back();
            if (resting_ids.empty()) {
                return resting_ids.size();
            }
        }
    };

    std::uint64_t untimed_commands = 0;
    const auto throughput_start = Clock::now();
    for (std::uint64_t i = 0; i < order_count; ++i) {
        const std::uint64_t id = next_id++;
        book.add(make_order(id));
        ++untimed_commands;

        if (!resting_ids.empty() && i % 4 == 3) {
            const std::size_t victim = pick_victim();
            if (victim < resting_ids.size()) {
                book.cancel(&orders[resting_ids[victim]]);
                ++untimed_commands;
                resting_ids[victim] = resting_ids.back();
                resting_ids.pop_back();
            }
        }
        if (orders[id].open > 0) {
            resting_ids.push_back(id);
        }
    }
    const std::uint64_t elapsed_ns = nanos_since(throughput_start);

    for (std::uint64_t i = 0; i < order_count; ++i) {
        const std::uint64_t id = next_id++;
        BenchOrder* order = make_order(id);

        const auto start = Clock::now();
        book.add(order);
        submit_latency.record(nanos_since(start));

        if (order->open > 0) {
            resting_ids.push_back(id);
        }
        if (!resting_ids.empty() && i % 4 == 3) {
            const std::size_t victim = pick_victim();
            if (victim == resting_ids.size()) {
                continue;
            }
            BenchOrder* target = &orders[resting_ids[victim]];
            const auto cancel_start = Clock::now();
            book.cancel(target);
            cancel_latency.record(nanos_since(cancel_start));
            resting_ids[victim] = resting_ids.back();
            resting_ids.pop_back();
        }
    }

    const double seconds = static_cast<double>(elapsed_ns) / 1e9;
    std::cout << "engine               liquibook\n"
              << "\nthroughput, measured without a clock read in the loop\n"
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
              << "  (includes the clock overhead, which is not subtracted)\n";
    print_percentiles("submit", submit_latency);
    print_percentiles("cancel", cancel_latency);

    std::cout << "\nbook at the end\n"
              << "  resting orders       " << book.bids().size() + book.asks().size() << '\n'
              << "  cancels accepted     " << fills.cancelled << '\n'
              << "  cancels rejected     " << fills.cancel_rejected << '\n';
    return 0;
}
