#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <vector>

#include "order_book_fixture.hpp"
#include "xc/core/matching_engine.hpp"
#include "xc/util/allocation_counter.hpp"
#include "xc/util/histogram.hpp"

namespace xc {
namespace {

using testing::test_instrument;

}  // namespace

/// External linkage on purpose: the compiler cannot prove nothing else reads
/// it, so an allocation stored here cannot be elided.
std::unique_ptr<std::vector<int>> escaping_sink;

namespace {

Instrument venue_instrument(std::size_t expected_orders = 200'000) {
    Instrument instrument = test_instrument();
    instrument.id = InstrumentId{1};
    instrument.symbol = "AAA";
    instrument.self_trade_policy = SelfTradePolicy::Allow;
    // Sized for the workload, which is what a venue does from its measured
    // high-water mark. Growth is the one thing left that allocates, so a book
    // running inside its reservation is the case the claim is about.
    instrument.expected_resting_orders = expected_orders;
    return instrument;
}

NewOrder order(std::uint64_t id, Side side, Price price, Quantity quantity) {
    NewOrder command;
    command.id = OrderId{id};
    command.account = AccountId{1};
    command.instrument = InstrumentId{1};
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    return command;
}

TEST(Allocations, TheCounterIsActuallyEnabledInThisBuild) {
    // Every other test in this file would pass trivially if counting were off,
    // reporting zero allocations because nothing was counting them. This is the
    // assertion that stops the rest from being vacuous -- and it has already
    // caught exactly that, when the instrumentation was defined on the test
    // target while the counter compiled into the library.
    ASSERT_TRUE(allocation_counting_enabled());

    // The allocation has to escape to be observed at all. C++14 permits the
    // compiler to elide allocations whose memory does not escape, and clang at
    // -O2 duly removes both `new int` and a local vector -- so a naive version
    // of this test reports zero allocations and looks like broken
    // instrumentation when the counter is working perfectly.
    const std::size_t size = static_cast<std::size_t>(std::rand() % 8 + 8);
    AllocationScope scope;
    escaping_sink = std::make_unique<std::vector<int>>(size, 1);
    EXPECT_GE(scope.allocations(), 1u);
    EXPECT_GE(scope.bytes(), size * sizeof(int));
    escaping_sink.reset();
}

TEST(Allocations, SubmittingAnOrderAllocatesNothingInSteadyState) {
    ManualClock clock;
    MatchingEngine engine{clock};
    ASSERT_TRUE(engine.add_instrument(venue_instrument()));

    // Warm up: the slab, the index and the fill buffer all grow to their
    // working size here. Steady state is what the claim is about, not the first
    // few messages of a venue's life.
    std::uint64_t id = 1;
    for (int i = 0; i < 20'000; ++i) {
        engine.submit(order(id++, i % 2 == 0 ? Side::Buy : Side::Sell, 10'000 + (i % 40) - 20, 10));
    }
    for (std::uint64_t cancel_id = 1; cancel_id < id; ++cancel_id) {
        engine.cancel(CancelOrder{OrderId{cancel_id}, AccountId{1}, InstrumentId{1}});
    }

    AllocationScope scope;
    for (int i = 0; i < 50'000; ++i) {
        engine.submit(order(id++, i % 2 == 0 ? Side::Buy : Side::Sell, 10'000 + (i % 40) - 20, 10));
    }

    // The claim the matching path makes, asserted rather than described. One
    // std::string, one vector past its reservation, one std::function assigned
    // in the wrong place, and this fails.
    EXPECT_EQ(scope.allocations(), 0u)
        << scope.allocations() << " allocations across 50,000 orders (" << scope.bytes()
        << " bytes)";
}

TEST(Allocations, CancellingAllocatesNothing) {
    ManualClock clock;
    MatchingEngine engine{clock};
    ASSERT_TRUE(engine.add_instrument(venue_instrument()));

    std::uint64_t id = 1;
    for (int i = 0; i < 20'000; ++i) {
        engine.submit(order(id++, Side::Buy, 10'000 - (i % 50), 10));
    }

    AllocationScope scope;
    for (std::uint64_t cancel_id = 1; cancel_id < id; ++cancel_id) {
        engine.cancel(CancelOrder{OrderId{cancel_id}, AccountId{1}, InstrumentId{1}});
    }
    EXPECT_EQ(scope.allocations(), 0u) << "cancellation is the most frequent operation there is";
}

TEST(Allocations, MatchingAndFillReportingAllocateNothing) {
    ManualClock clock;
    MatchingEngine engine{clock};
    ASSERT_TRUE(engine.add_instrument(venue_instrument()));

    std::uint64_t id = 1;
    for (int i = 0; i < 30'000; ++i) {
        engine.submit(order(id++, Side::Sell, 10'000, 1));
    }
    // Warm the fill buffer with a sweep before measuring.
    engine.submit(order(id++, Side::Buy, 10'000, 200));

    AllocationScope scope;
    for (int i = 0; i < 100; ++i) {
        engine.submit(order(id++, Side::Buy, 10'000, 50));
    }
    EXPECT_GT(engine.last_fills().size(), 0u) << "the workload must actually have traded";
    EXPECT_EQ(scope.allocations(), 0u);
}

TEST(Allocations, TakingADepthSnapshotAllocatesNothingAfterWarmup) {
    ManualClock clock;
    MatchingEngine engine{clock};
    ASSERT_TRUE(engine.add_instrument(venue_instrument()));
    for (int i = 0; i < 2'000; ++i) {
        engine.submit(order(static_cast<std::uint64_t>(i + 1), Side::Buy, 10'000 - (i % 100), 10));
    }

    DepthSnapshot snapshot;
    const OrderBook* book = engine.book(InstrumentId{1});
    ASSERT_NE(book, nullptr);
    book->depth(20, snapshot);  // Warms the level vectors.

    AllocationScope scope;
    for (int i = 0; i < 10'000; ++i) {
        book->depth(20, snapshot);
    }
    // Publishing depth on every book change is the single most frequent thing a
    // venue does. Allocating there would allocate thousands of times a second.
    EXPECT_EQ(scope.allocations(), 0u);
}

TEST(Allocations, RecordingALatencySampleAllocatesNothing) {
    Histogram histogram;
    histogram.record(1);  // Buckets are sized at construction.

    AllocationScope scope;
    for (std::uint64_t i = 1; i <= 200'000; ++i) {
        histogram.record((i * 37) % 5'000'000);
    }
    // An instrument that allocates perturbs exactly the thing it is measuring.
    EXPECT_EQ(scope.allocations(), 0u);
}

}  // namespace
}  // namespace xc
