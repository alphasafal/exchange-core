#include <gtest/gtest.h>

#include <vector>

#include "xc/core/order_pool.hpp"
#include "xc/core/price_level.hpp"

namespace xc {
namespace {

Order make_order(std::uint64_t id, Quantity quantity, Price price = 100) {
    Order order;
    order.id = OrderId{id};
    order.account = AccountId{1};
    order.instrument = InstrumentId{1};
    order.price = price;
    order.quantity = quantity;
    order.remaining = quantity;
    order.sequence = id;
    return order;
}

// --- OrderPool -------------------------------------------------------------

TEST(OrderPool, ReusesReleasedNodesInsteadOfGrowing) {
    OrderPool pool(4);
    const std::size_t capacity_before = pool.capacity();

    // Churn far more orders than the slab can hold at once. Because each is
    // released before the next is acquired, the free list should absorb all of
    // them without a single reallocation -- this is the steady-state behaviour
    // the matching path depends on.
    for (std::uint64_t i = 1; i <= 1000; ++i) {
        const OrderHandle handle = pool.acquire(make_order(i, 10));
        EXPECT_EQ(pool[handle].id, OrderId{i});
        pool.release(handle);
    }

    EXPECT_EQ(pool.live(), 0u);
    EXPECT_EQ(pool.growth_events(), 0u);
    EXPECT_EQ(pool.capacity(), capacity_before);
    EXPECT_EQ(pool.high_water_mark(), 1u);
}

TEST(OrderPool, TracksLiveCountAndHighWaterMark) {
    OrderPool pool(16);
    std::vector<OrderHandle> handles;
    for (std::uint64_t i = 1; i <= 10; ++i) {
        handles.push_back(pool.acquire(make_order(i, 5)));
    }
    EXPECT_EQ(pool.live(), 10u);
    EXPECT_EQ(pool.high_water_mark(), 10u);

    for (const OrderHandle handle : handles) {
        pool.release(handle);
    }
    EXPECT_EQ(pool.live(), 0u);
    EXPECT_EQ(pool.high_water_mark(), 10u) << "high water mark must not decay";
}

TEST(OrderPool, GrowsWhenTheReservationIsExhaustedAndKeepsHandlesValid) {
    OrderPool pool(2);
    std::vector<OrderHandle> handles;
    for (std::uint64_t i = 1; i <= 64; ++i) {
        handles.push_back(pool.acquire(make_order(i, i)));
    }

    EXPECT_GT(pool.growth_events(), 0u) << "the slab was expected to outgrow its reservation";

    // Growth relocates the slab's storage. Handles are indices, so every one
    // issued before the move must still resolve to the order it was given.
    for (std::uint64_t i = 1; i <= 64; ++i) {
        const Order& order = pool[handles[i - 1]];
        EXPECT_EQ(order.id, OrderId{i});
        EXPECT_EQ(order.remaining, i);
    }
}

// --- PriceLevel ------------------------------------------------------------

std::vector<OrderId> queue_order(const PriceLevel& level, const OrderPool& pool) {
    std::vector<OrderId> ids;
    for (OrderHandle h = level.front(); h != kNullHandle; h = pool.node(h).next) {
        ids.push_back(pool[h].id);
    }
    return ids;
}

TEST(PriceLevel, PreservesArrivalOrder) {
    OrderPool pool(8);
    PriceLevel level;
    for (std::uint64_t i = 1; i <= 4; ++i) {
        level.push_back(pool, pool.acquire(make_order(i, 10)));
    }

    EXPECT_EQ(queue_order(level, pool),
              (std::vector<OrderId>{OrderId{1}, OrderId{2}, OrderId{3}, OrderId{4}}));
    EXPECT_EQ(level.total_quantity(), 40u);
    EXPECT_EQ(level.order_count(), 4u);
}

TEST(PriceLevel, UnlinksFromTheMiddleWithoutDisturbingPriority) {
    OrderPool pool(8);
    PriceLevel level;
    std::vector<OrderHandle> handles;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        handles.push_back(pool.acquire(make_order(i, 10)));
        level.push_back(pool, handles.back());
    }

    level.unlink(pool, handles[2]);  // order 3, in the middle

    EXPECT_EQ(queue_order(level, pool),
              (std::vector<OrderId>{OrderId{1}, OrderId{2}, OrderId{4}, OrderId{5}}));
    EXPECT_EQ(level.total_quantity(), 40u);
    EXPECT_EQ(level.order_count(), 4u);
}

TEST(PriceLevel, UnlinksHeadAndTailCorrectly) {
    OrderPool pool(8);
    PriceLevel level;
    std::vector<OrderHandle> handles;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        handles.push_back(pool.acquire(make_order(i, 10)));
        level.push_back(pool, handles.back());
    }

    level.unlink(pool, handles[0]);
    EXPECT_EQ(queue_order(level, pool), (std::vector<OrderId>{OrderId{2}, OrderId{3}}));

    level.unlink(pool, handles[2]);
    EXPECT_EQ(queue_order(level, pool), (std::vector<OrderId>{OrderId{2}}));

    level.unlink(pool, handles[1]);
    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.total_quantity(), 0u);
    EXPECT_EQ(level.order_count(), 0u);
}

TEST(PriceLevel, RefillsAfterBeingEmptied) {
    OrderPool pool(8);
    PriceLevel level;
    const OrderHandle first = pool.acquire(make_order(1, 10));
    level.push_back(pool, first);
    level.unlink(pool, first);
    ASSERT_TRUE(level.empty());

    // A level that has been drained and reused is a normal occurrence at the
    // top of book. Stale head/tail links would corrupt the queue here.
    const OrderHandle second = pool.acquire(make_order(2, 25));
    level.push_back(pool, second);
    EXPECT_EQ(queue_order(level, pool), (std::vector<OrderId>{OrderId{2}}));
    EXPECT_EQ(level.total_quantity(), 25u);
}

TEST(PriceLevel, ReduceTracksPartialFills) {
    OrderPool pool(8);
    PriceLevel level;
    const OrderHandle handle = pool.acquire(make_order(1, 100));
    level.push_back(pool, handle);

    pool[handle].remaining -= 30;
    level.reduce(30);
    EXPECT_EQ(level.total_quantity(), 70u);

    // The level total and the order's remaining quantity must stay in step, or
    // published depth drifts from what is actually resting.
    level.unlink(pool, handle);
    EXPECT_EQ(level.total_quantity(), 0u);
}

}  // namespace
}  // namespace xc
