#include <gtest/gtest.h>

#include <vector>

#include "order_book_fixture.hpp"
#include "xc/core/order_book.hpp"

namespace xc {
namespace {

using testing::OrderFactory;
using testing::test_instrument;

class DepthTest : public ::testing::Test {
  protected:
    OrderBook book{test_instrument()};
    OrderFactory make;
    std::vector<Fill> fills;

    void submit(const Order& order) { book.submit(order, fills); }
};

TEST_F(DepthTest, TopOfBookIsEmptyOnAnEmptyBook) {
    const TopOfBook top = book.top_of_book();
    EXPECT_FALSE(top.has_bid());
    EXPECT_FALSE(top.has_ask());

    // A spread against nothing is unknown, not zero. Reporting zero would make
    // an empty book look like the tightest market on the venue.
    EXPECT_FALSE(top.spread().has_value());
    EXPECT_FALSE(top.mid_price_x2().has_value());
}

TEST_F(DepthTest, TopOfBookAggregatesEveryOrderAtTheTouch) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Buy, 100, 15));
    submit(make.limit(3, Side::Buy, 99, 100));
    submit(make.limit(4, Side::Sell, 103, 20));

    const TopOfBook top = book.top_of_book();
    EXPECT_EQ(top.bid_price, 100);
    EXPECT_EQ(top.bid_quantity, 25u) << "both orders at the touch, not just the first";
    EXPECT_EQ(top.ask_price, 103);
    EXPECT_EQ(top.ask_quantity, 20u);
    EXPECT_EQ(top.spread(), 3);
}

TEST_F(DepthTest, MidPriceIsExactAcrossAnOddSpread) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Sell, 101, 10));

    // The true mid is 100.5. Integer division would report 100 and inject half
    // a tick of error into everything downstream, so the doubled value is
    // published and the caller decides how to round.
    EXPECT_EQ(book.top_of_book().mid_price_x2(), 201);
}

TEST_F(DepthTest, DepthIsOrderedBestPriceFirstOnBothSides) {
    submit(make.limit(1, Side::Buy, 98, 10));
    submit(make.limit(2, Side::Buy, 100, 20));
    submit(make.limit(3, Side::Buy, 99, 30));
    submit(make.limit(4, Side::Sell, 105, 40));
    submit(make.limit(5, Side::Sell, 103, 50));
    submit(make.limit(6, Side::Sell, 104, 60));

    DepthSnapshot snapshot;
    book.depth(10, snapshot);

    ASSERT_EQ(snapshot.bids.size(), 3u);
    EXPECT_EQ(snapshot.bids[0], (DepthLevel{100, 20, 1}));
    EXPECT_EQ(snapshot.bids[1], (DepthLevel{99, 30, 1}));
    EXPECT_EQ(snapshot.bids[2], (DepthLevel{98, 10, 1}));

    ASSERT_EQ(snapshot.asks.size(), 3u);
    EXPECT_EQ(snapshot.asks[0], (DepthLevel{103, 50, 1}));
    EXPECT_EQ(snapshot.asks[1], (DepthLevel{104, 60, 1}));
    EXPECT_EQ(snapshot.asks[2], (DepthLevel{105, 40, 1}));
}

TEST_F(DepthTest, AggregatesQuantityAndOrderCountPerLevel) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Buy, 100, 20));
    submit(make.limit(3, Side::Buy, 100, 30));

    DepthSnapshot snapshot;
    book.depth(5, snapshot);
    ASSERT_EQ(snapshot.bids.size(), 1u);
    EXPECT_EQ(snapshot.bids[0], (DepthLevel{100, 60, 3}));
}

TEST_F(DepthTest, TruncatesToTheRequestedNumberOfLevels) {
    for (Price price = 100; price > 90; --price) {
        submit(make.limit(static_cast<std::uint64_t>(price), Side::Buy, price, 10));
    }

    DepthSnapshot snapshot;
    book.depth(3, snapshot);
    ASSERT_EQ(snapshot.bids.size(), 3u);
    EXPECT_EQ(snapshot.bids[0].price, 100);
    EXPECT_EQ(snapshot.bids[2].price, 98) << "the three best levels, not the three cheapest";
}

TEST_F(DepthTest, ReflectsPartialFillsWithoutWalkingQueues) {
    submit(make.limit(1, Side::Buy, 100, 100));
    submit(make.limit(2, Side::Buy, 100, 100));
    submit(make.limit(3, Side::Sell, 100, 150));

    DepthSnapshot snapshot;
    book.depth(5, snapshot);
    ASSERT_EQ(snapshot.bids.size(), 1u);
    EXPECT_EQ(snapshot.bids[0], (DepthLevel{100, 50, 1})) << "one order remains, partially filled";
}

TEST_F(DepthTest, ReusesItsBuffersAcrossSnapshots) {
    for (Price price = 100; price > 80; --price) {
        submit(make.limit(static_cast<std::uint64_t>(price), Side::Buy, price, 10));
    }

    DepthSnapshot snapshot;
    book.depth(20, snapshot);
    const auto* first_buffer = snapshot.bids.data();
    const std::size_t first_capacity = snapshot.bids.capacity();

    // A publisher snapshots on every book change. If each one reallocated, the
    // hot path would allocate thousands of times a second.
    for (int i = 0; i < 100; ++i) {
        book.depth(20, snapshot);
    }
    EXPECT_EQ(snapshot.bids.data(), first_buffer) << "storage was reused, not reallocated";
    EXPECT_EQ(snapshot.bids.capacity(), first_capacity);
    EXPECT_EQ(snapshot.bids.size(), 20u);
}

TEST_F(DepthTest, ClearsStaleLevelsWhenTheBookShrinks) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Buy, 99, 10));

    DepthSnapshot snapshot;
    book.depth(5, snapshot);
    ASSERT_EQ(snapshot.bids.size(), 2u);

    book.cancel(OrderId{2});
    book.depth(5, snapshot);
    ASSERT_EQ(snapshot.bids.size(), 1u) << "the cancelled level must not linger in the buffer";
    EXPECT_EQ(snapshot.bids[0].price, 100);

    book.cancel(OrderId{1});
    book.depth(5, snapshot);
    EXPECT_TRUE(snapshot.bids.empty());
    EXPECT_TRUE(snapshot.asks.empty());
}

}  // namespace
}  // namespace xc
