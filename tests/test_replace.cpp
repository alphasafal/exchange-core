#include <gtest/gtest.h>

#include <vector>

#include "order_book_fixture.hpp"
#include "xc/core/order_book.hpp"

namespace xc {
namespace {

using testing::OrderFactory;
using testing::test_instrument;

class ReplaceTest : public ::testing::Test {
  protected:
    OrderBook book{test_instrument()};
    OrderFactory make;
    std::vector<Fill> fills;
    SeqNum next_sequence = 1000;

    SubmitResult submit(const Order& order) { return book.submit(order, fills); }

    ReplaceResult replace(std::uint64_t id, Price price, Quantity quantity) {
        return book.replace(OrderId{id}, price, quantity, ++next_sequence, 0, fills);
    }
};

TEST_F(ReplaceTest, ReducingSizeAtTheSamePriceKeepsQueuePosition) {
    submit(make.limit(1, Side::Buy, 100, 100));
    submit(make.limit(2, Side::Buy, 100, 100));

    const ReplaceResult result = replace(1, 100, 50);
    ASSERT_TRUE(result.accepted());
    EXPECT_TRUE(result.priority_retained);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 150u);

    // Order 1 gave up size it had already earned position for, so it is still
    // first in the queue.
    submit(make.limit(3, Side::Sell, 100, 60));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].resting_order, OrderId{1});
    EXPECT_EQ(fills[0].quantity, 50u);
    EXPECT_EQ(fills[1].resting_order, OrderId{2});
}

TEST_F(ReplaceTest, IncreasingSizeGoesToTheBackOfTheQueue) {
    submit(make.limit(1, Side::Buy, 100, 50));
    submit(make.limit(2, Side::Buy, 100, 50));

    const ReplaceResult result = replace(1, 100, 200);
    ASSERT_TRUE(result.accepted());
    EXPECT_FALSE(result.priority_retained);

    // Without this rule an order could hold the front of the queue with a
    // single lot and inflate itself the moment it was about to trade.
    submit(make.limit(3, Side::Sell, 100, 60));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].resting_order, OrderId{2}) << "order 2 now has priority";
    EXPECT_EQ(fills[0].quantity, 50u);
    EXPECT_EQ(fills[1].resting_order, OrderId{1});
    EXPECT_EQ(fills[1].quantity, 10u);
}

TEST_F(ReplaceTest, ChangingPriceGoesToTheBackOfTheNewLevel) {
    submit(make.limit(1, Side::Buy, 99, 50));
    submit(make.limit(2, Side::Buy, 100, 50));

    ASSERT_TRUE(replace(1, 100, 50).accepted());
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 100u);
    EXPECT_EQ(book.quantity_at(Side::Buy, 99), 0u);
    EXPECT_EQ(book.bid_level_count(), 1u);

    submit(make.limit(3, Side::Sell, 100, 60));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].resting_order, OrderId{2}) << "the order already at 100 keeps priority";
}

TEST_F(ReplaceTest, AmendingIntoTheSpreadCanTradeImmediately) {
    submit(make.limit(1, Side::Sell, 105, 40));
    submit(make.limit(2, Side::Buy, 100, 40));
    fills.clear();

    // Moving the bid up to the offer is a new arrival at a crossing price, so
    // it trades rather than resting above the ask.
    const ReplaceResult result = replace(2, 105, 40);

    ASSERT_TRUE(result.accepted());
    EXPECT_EQ(result.filled, 40u);
    EXPECT_FALSE(result.rested);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].price, 105);
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST_F(ReplaceTest, AmendedQuantityIsCountedFromOriginalSubmissionNotFromTheRemainder) {
    submit(make.limit(1, Side::Buy, 100, 100));
    submit(make.limit(2, Side::Sell, 100, 30));
    ASSERT_EQ(book.find(OrderId{1})->remaining, 70u);

    // 30 has already traded. Amending the total to 50 is a request to trade 20
    // more, not 50 more -- reading it the other way would silently double the
    // client's exposure.
    const ReplaceResult result = replace(1, 100, 50);
    ASSERT_TRUE(result.accepted());
    EXPECT_TRUE(result.priority_retained);

    const Order* amended = book.find(OrderId{1});
    ASSERT_NE(amended, nullptr);
    EXPECT_EQ(amended->quantity, 50u);
    EXPECT_EQ(amended->remaining, 20u);
    EXPECT_EQ(amended->filled(), 30u);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 20u);
}

TEST_F(ReplaceTest, AmendingBelowWhatAlreadyTradedCancelsTheOrder) {
    submit(make.limit(1, Side::Buy, 100, 100));
    submit(make.limit(2, Side::Sell, 100, 60));
    ASSERT_EQ(book.find(OrderId{1})->filled(), 60u);

    // The client no longer wants more than 40, and already has 60. The fills
    // cannot be undone, so the only honourable response is to stop trading.
    const ReplaceResult result = replace(1, 100, 40);

    EXPECT_TRUE(result.accepted());
    EXPECT_FALSE(result.rested);
    EXPECT_EQ(book.find(OrderId{1}), nullptr);
    EXPECT_EQ(book.resting_order_count(), 0u);
}

TEST_F(ReplaceTest, ARequeuedAmendmentCarriesItsEarlierFills) {
    submit(make.limit(1, Side::Buy, 100, 100));
    submit(make.limit(2, Side::Sell, 100, 40));
    fills.clear();

    // Moving the price re-queues the order. Its 40 already-filled lots must
    // travel with it, or the amendment silently re-arms quantity the client
    // has already traded.
    ASSERT_TRUE(replace(1, 99, 100).accepted());

    const Order* amended = book.find(OrderId{1});
    ASSERT_NE(amended, nullptr);
    EXPECT_EQ(amended->price, 99);
    EXPECT_EQ(amended->quantity, 100u);
    EXPECT_EQ(amended->remaining, 60u);
    EXPECT_EQ(amended->filled(), 40u);
    EXPECT_EQ(book.quantity_at(Side::Buy, 99), 60u);
}

TEST_F(ReplaceTest, RejectsAnUnknownOrder) {
    EXPECT_EQ(replace(42, 100, 10).reject, RejectReason::UnknownOrder);
}

TEST_F(ReplaceTest, RejectsAnInvalidPriceAndLeavesTheOrderAlone) {
    submit(make.limit(1, Side::Buy, 100, 50));
    EXPECT_EQ(replace(1, 0, 50).reject, RejectReason::InvalidPrice);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 50u) << "a rejected amendment changes nothing";
    EXPECT_EQ(book.find(OrderId{1})->remaining, 50u);
}

TEST_F(ReplaceTest, RejectsAnInvalidQuantityAndLeavesTheOrderAlone) {
    submit(make.limit(1, Side::Buy, 100, 50));
    EXPECT_EQ(replace(1, 100, 0).reject, RejectReason::InvalidQuantity);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 50u);
}

TEST_F(ReplaceTest, EmptiesThePriceLevelItLeaves) {
    submit(make.limit(1, Side::Buy, 100, 50));
    ASSERT_EQ(book.bid_level_count(), 1u);
    ASSERT_TRUE(replace(1, 98, 50).accepted());
    EXPECT_EQ(book.bid_level_count(), 1u);
    EXPECT_EQ(book.best_bid(), 98);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 0u);
}

TEST_F(ReplaceTest, DoesNotLeakSlabNodesAcrossRepeatedAmendments) {
    submit(make.limit(1, Side::Buy, 100, 50));
    for (Price price = 100; price > 60; --price) {
        ASSERT_TRUE(replace(1, price, 50).accepted()) << "at price " << price;
    }
    EXPECT_EQ(book.pool().live(), 1u);
    EXPECT_EQ(book.resting_order_count(), 1u);
}

}  // namespace
}  // namespace xc
