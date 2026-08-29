#include <gtest/gtest.h>

#include <vector>

#include "order_book_fixture.hpp"
#include "xc/core/order_book.hpp"

namespace xc {
namespace {

using testing::OrderFactory;
using testing::test_instrument;

class OrderBookTest : public ::testing::Test {
  protected:
    OrderBook book{test_instrument()};
    OrderFactory make;
    std::vector<Fill> fills;

    SubmitResult submit(const Order& order) { return book.submit(order, fills); }
};

// --- Resting ---------------------------------------------------------------

TEST_F(OrderBookTest, RestsAnOrderThatDoesNotCross) {
    const SubmitResult result = submit(make.limit(1, Side::Buy, 100, 50));

    EXPECT_TRUE(result.accepted());
    EXPECT_TRUE(result.rested);
    EXPECT_EQ(result.filled, 0u);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 50u);
}

TEST_F(OrderBookTest, KeepsBestBidHighestAndBestAskLowest) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Buy, 102, 10));
    submit(make.limit(3, Side::Buy, 101, 10));
    submit(make.limit(4, Side::Sell, 110, 10));
    submit(make.limit(5, Side::Sell, 108, 10));
    submit(make.limit(6, Side::Sell, 109, 10));

    EXPECT_EQ(book.best_bid(), 102);
    EXPECT_EQ(book.best_ask(), 108);
    EXPECT_EQ(book.bid_level_count(), 3u);
    EXPECT_EQ(book.ask_level_count(), 3u);
}

TEST_F(OrderBookTest, RejectsDuplicateOrderIds) {
    ASSERT_TRUE(submit(make.limit(1, Side::Buy, 100, 10)).accepted());

    // Accepting this would overwrite the index entry and strand the original on
    // its level: unreachable by cancel, removable only by being filled.
    const SubmitResult duplicate = submit(make.limit(1, Side::Buy, 101, 10));
    EXPECT_EQ(duplicate.reject, RejectReason::DuplicateOrderId);
    EXPECT_EQ(book.resting_order_count(), 1u);
    EXPECT_EQ(book.best_bid(), 100);
}

TEST_F(OrderBookTest, RejectsPricesOffTheTickGrid) {
    OrderBook coarse{test_instrument(/*tick_size=*/25)};
    std::vector<Fill> out;
    EXPECT_EQ(coarse.submit(make.limit(1, Side::Buy, 110, 10), out).reject,
              RejectReason::InvalidPrice);
    EXPECT_TRUE(coarse.submit(make.limit(2, Side::Buy, 100, 10), out).accepted());
}

TEST_F(OrderBookTest, RejectsZeroQuantity) {
    EXPECT_EQ(submit(make.limit(1, Side::Buy, 100, 0)).reject, RejectReason::InvalidQuantity);
}

// --- Crossing --------------------------------------------------------------

TEST_F(OrderBookTest, TradesAtTheRestingPriceNotTheAggressorPrice) {
    submit(make.limit(1, Side::Sell, 100, 50));

    // The buyer is willing to pay 105 but the offer is 100. The resting order
    // set the terms, so the trade prints at 100 and the buyer keeps the rest.
    const SubmitResult result = submit(make.limit(2, Side::Buy, 105, 50));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[0].quantity, 50u);
    EXPECT_EQ(fills[0].aggressor_side, Side::Buy);
    EXPECT_EQ(fills[0].aggressor_order, OrderId{2});
    EXPECT_EQ(fills[0].resting_order, OrderId{1});
    EXPECT_TRUE(fills[0].resting_filled);
    EXPECT_EQ(result.filled, 50u);
    EXPECT_FALSE(result.rested);
    EXPECT_EQ(book.resting_order_count(), 0u);
}

TEST_F(OrderBookTest, DoesNotCrossWhenPricesDoNotOverlap) {
    submit(make.limit(1, Side::Sell, 101, 10));
    const SubmitResult result = submit(make.limit(2, Side::Buy, 100, 10));

    EXPECT_TRUE(fills.empty());
    EXPECT_TRUE(result.rested);
    EXPECT_EQ(book.best_bid(), 100);
    EXPECT_EQ(book.best_ask(), 101);
}

TEST_F(OrderBookTest, MatchesExactlyAtTheTouch) {
    submit(make.limit(1, Side::Sell, 100, 10));
    submit(make.limit(2, Side::Buy, 100, 10));

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST_F(OrderBookTest, NeverLeavesTheBookCrossed) {
    submit(make.limit(1, Side::Sell, 100, 10));
    submit(make.limit(2, Side::Sell, 101, 10));
    submit(make.limit(3, Side::Buy, 105, 15));

    // The aggressor took all of 100 and part of 101; whatever rests must not
    // sit at or above the remaining offer.
    ASSERT_TRUE(book.best_ask().has_value());
    if (book.best_bid().has_value()) {
        EXPECT_LT(*book.best_bid(), *book.best_ask());
    }
}

TEST_F(OrderBookTest, SweepsMultipleLevelsInPriceOrder) {
    submit(make.limit(1, Side::Sell, 100, 10));
    submit(make.limit(2, Side::Sell, 101, 10));
    submit(make.limit(3, Side::Sell, 102, 10));

    const SubmitResult result = submit(make.limit(4, Side::Buy, 102, 25));

    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[0].quantity, 10u);
    EXPECT_EQ(fills[1].price, 101);
    EXPECT_EQ(fills[1].quantity, 10u);
    EXPECT_EQ(fills[2].price, 102);
    EXPECT_EQ(fills[2].quantity, 5u) << "the last level is only partially consumed";
    EXPECT_EQ(result.filled, 25u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 102), 5u);
}

TEST_F(OrderBookTest, SellAggressorSweepsBidsFromTheHighestPrice) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Buy, 102, 10));
    submit(make.limit(3, Side::Buy, 101, 10));

    submit(make.limit(4, Side::Sell, 100, 25));

    ASSERT_EQ(fills.size(), 3u);
    EXPECT_EQ(fills[0].price, 102) << "best bid trades first";
    EXPECT_EQ(fills[1].price, 101);
    EXPECT_EQ(fills[2].price, 100);
    EXPECT_EQ(fills[2].quantity, 5u);
}

// --- Time priority ---------------------------------------------------------

TEST_F(OrderBookTest, FillsTheEarliestOrderFirstWithinAPriceLevel) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Buy, 100, 10));
    submit(make.limit(3, Side::Buy, 100, 10));

    submit(make.limit(4, Side::Sell, 100, 15));

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].resting_order, OrderId{1}) << "first in, first filled";
    EXPECT_EQ(fills[0].quantity, 10u);
    EXPECT_EQ(fills[1].resting_order, OrderId{2});
    EXPECT_EQ(fills[1].quantity, 5u);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 15u);
}

TEST_F(OrderBookTest, ANewOrderJoinsTheBackOfItsLevel) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Buy, 100, 10));
    // Cancelling the front order must not promote the newest ahead of order 2.
    ASSERT_TRUE(book.cancel(OrderId{1}).accepted());
    submit(make.limit(3, Side::Buy, 100, 10));

    submit(make.limit(4, Side::Sell, 100, 20));

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].resting_order, OrderId{2});
    EXPECT_EQ(fills[1].resting_order, OrderId{3});
}

// --- Partial fills ---------------------------------------------------------

TEST_F(OrderBookTest, RestsTheRemainderOfAPartiallyFilledAggressor) {
    submit(make.limit(1, Side::Sell, 100, 30));

    const SubmitResult result = submit(make.limit(2, Side::Buy, 100, 50));

    EXPECT_EQ(result.filled, 30u);
    EXPECT_TRUE(result.rested);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 20u);
    EXPECT_FALSE(book.best_ask().has_value());

    const Order* resting = book.find(OrderId{2});
    ASSERT_NE(resting, nullptr);
    EXPECT_EQ(resting->quantity, 50u) << "the submitted quantity is never rewritten";
    EXPECT_EQ(resting->remaining, 20u);
    EXPECT_EQ(resting->filled(), 30u);
}

TEST_F(OrderBookTest, KeepsAPartiallyFilledRestingOrderAtItsPlaceInTheQueue) {
    submit(make.limit(1, Side::Buy, 100, 100));
    submit(make.limit(2, Side::Buy, 100, 100));

    submit(make.limit(3, Side::Sell, 100, 40));
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(fills[0].resting_order, OrderId{1});
    EXPECT_FALSE(fills[0].resting_filled);

    // Order 1 was only partly filled, so it keeps priority over order 2.
    fills.clear();
    submit(make.limit(4, Side::Sell, 100, 100));
    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].resting_order, OrderId{1});
    EXPECT_EQ(fills[0].quantity, 60u);
    EXPECT_EQ(fills[1].resting_order, OrderId{2});
    EXPECT_EQ(fills[1].quantity, 40u);
}

TEST_F(OrderBookTest, ConservesQuantityAcrossEveryFill) {
    submit(make.limit(1, Side::Sell, 100, 37));
    submit(make.limit(2, Side::Sell, 101, 41));
    const SubmitResult result = submit(make.limit(3, Side::Buy, 101, 100));

    Quantity traded = 0;
    for (const Fill& fill : fills) {
        traded += fill.quantity;
    }
    EXPECT_EQ(traded, 78u);
    EXPECT_EQ(result.filled, traded);
    EXPECT_EQ(book.quantity_at(Side::Buy, 101), 100u - traded);
    EXPECT_EQ(book.total_quantity(Side::Sell), 0u);
}

// --- Cancellation ----------------------------------------------------------

TEST_F(OrderBookTest, CancelsARestingOrderAndReportsItsRemainder) {
    submit(make.limit(1, Side::Buy, 100, 50));
    submit(make.limit(2, Side::Sell, 100, 20));
    ASSERT_EQ(fills.size(), 1u);

    const CancelResult result = book.cancel(OrderId{1});
    ASSERT_TRUE(result.accepted());
    EXPECT_EQ(result.order.id, OrderId{1});
    EXPECT_EQ(result.order.quantity, 50u);
    EXPECT_EQ(result.order.remaining, 30u) << "the unfilled remainder is what gets released";
    EXPECT_EQ(book.resting_order_count(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST_F(OrderBookTest, RejectsCancellingAnUnknownOrder) {
    EXPECT_EQ(book.cancel(OrderId{999}).reject, RejectReason::UnknownOrder);
}

TEST_F(OrderBookTest, RejectsCancellingTheSameOrderTwice) {
    submit(make.limit(1, Side::Buy, 100, 10));
    ASSERT_TRUE(book.cancel(OrderId{1}).accepted());
    EXPECT_EQ(book.cancel(OrderId{1}).reject, RejectReason::UnknownOrder);
}

TEST_F(OrderBookTest, RejectsCancellingAnOrderThatAlreadyFilled) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Sell, 100, 10));
    ASSERT_EQ(book.resting_order_count(), 0u);

    // A cancel racing a fill is routine, and it must not resurrect the order.
    EXPECT_EQ(book.cancel(OrderId{1}).reject, RejectReason::UnknownOrder);
}

TEST_F(OrderBookTest, RemovesAPriceLevelOnceItsLastOrderIsCancelled) {
    submit(make.limit(1, Side::Buy, 100, 10));
    submit(make.limit(2, Side::Buy, 99, 10));
    ASSERT_EQ(book.bid_level_count(), 2u);

    ASSERT_TRUE(book.cancel(OrderId{1}).accepted());
    EXPECT_EQ(book.bid_level_count(), 1u);
    EXPECT_EQ(book.best_bid(), 99) << "the next level becomes the touch";
}

TEST_F(OrderBookTest, ReturnsSlabSpaceWhenOrdersLeaveTheBook) {
    // Cancels and fills must both return their node to the free list, or a
    // long-running venue leaks a node per order until the slab exhausts memory.
    for (std::uint64_t i = 1; i <= 500; ++i) {
        submit(make.limit(i, Side::Buy, 100, 10));
        ASSERT_TRUE(book.cancel(OrderId{i}).accepted());
    }
    EXPECT_EQ(book.pool().live(), 0u);
    EXPECT_EQ(book.pool().high_water_mark(), 1u);
}

// --- Market orders ---------------------------------------------------------

TEST_F(OrderBookTest, MarketOrderTakesEveryPriceUntilItIsFilled) {
    submit(make.limit(1, Side::Sell, 100, 10));
    submit(make.limit(2, Side::Sell, 500, 10));

    const SubmitResult result = submit(make.market(3, Side::Buy, 15));

    ASSERT_EQ(fills.size(), 2u);
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(fills[1].price, 500) << "a market order has no price limit";
    EXPECT_EQ(result.filled, 15u);
}

TEST_F(OrderBookTest, MarketOrderNeverRests) {
    submit(make.limit(1, Side::Sell, 100, 10));

    const SubmitResult result = submit(make.market(2, Side::Buy, 100));

    EXPECT_EQ(result.filled, 10u);
    EXPECT_FALSE(result.rested) << "there is no price at which a market order could rest";
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.resting_order_count(), 0u);
}

TEST_F(OrderBookTest, MarketOrderAgainstAnEmptyBookFillsNothing) {
    const SubmitResult result = submit(make.market(1, Side::Buy, 10));
    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(result.filled, 0u);
    EXPECT_FALSE(result.rested);
    EXPECT_TRUE(fills.empty());
}

// --- Immediate or cancel ---------------------------------------------------

TEST_F(OrderBookTest, ImmediateOrCancelFillsWhatItCanAndDiscardsTheRest) {
    submit(make.limit(1, Side::Sell, 100, 30));

    const SubmitResult result =
        submit(make.with_tif(make.limit(2, Side::Buy, 100, 50), TimeInForce::ImmediateOrCancel));

    EXPECT_EQ(result.filled, 30u);
    EXPECT_FALSE(result.rested);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.resting_order_count(), 0u);
}

// --- Fill or kill ----------------------------------------------------------

TEST_F(OrderBookTest, FillOrKillTradesWhenTheBookCanFillItCompletely) {
    submit(make.limit(1, Side::Sell, 100, 30));
    submit(make.limit(2, Side::Sell, 101, 30));

    const SubmitResult result =
        submit(make.with_tif(make.limit(3, Side::Buy, 101, 50), TimeInForce::FillOrKill));

    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(result.filled, 50u);
    EXPECT_FALSE(result.rested);
}

TEST_F(OrderBookTest, FillOrKillThatCannotFillLeavesTheBookExactlyAsItWas) {
    submit(make.limit(1, Side::Sell, 100, 30));
    submit(make.limit(2, Side::Sell, 101, 10));
    fills.clear();

    // Only 40 is available against a request for 50. The naive implementation
    // matches greedily and then discards the trades, which does not undo the
    // decrements it already applied: the resting orders come back short and the
    // caller is told nothing happened. Nothing here may move.
    const SubmitResult result =
        submit(make.with_tif(make.limit(3, Side::Buy, 101, 50), TimeInForce::FillOrKill));

    EXPECT_EQ(result.reject, RejectReason::FillOrKillUnfillable);
    EXPECT_EQ(result.filled, 0u);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 30u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 101), 10u);
    EXPECT_EQ(book.resting_order_count(), 2u);
    EXPECT_EQ(book.find(OrderId{1})->remaining, 30u);
    EXPECT_EQ(book.find(OrderId{2})->remaining, 10u);
}

TEST_F(OrderBookTest, FillOrKillIgnoresLiquidityBeyondItsLimitPrice) {
    submit(make.limit(1, Side::Sell, 100, 30));
    submit(make.limit(2, Side::Sell, 200, 100));

    // There is plenty of size in the book, but not at a price this order will
    // pay. Counting it would fill the order at a price it never agreed to.
    const SubmitResult result =
        submit(make.with_tif(make.limit(3, Side::Buy, 100, 50), TimeInForce::FillOrKill));

    EXPECT_EQ(result.reject, RejectReason::FillOrKillUnfillable);
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 30u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 200), 100u);
}

TEST_F(OrderBookTest, FillOrKillAgainstAnEmptyBookIsRejected) {
    const SubmitResult result =
        submit(make.with_tif(make.limit(1, Side::Buy, 100, 10), TimeInForce::FillOrKill));
    EXPECT_EQ(result.reject, RejectReason::FillOrKillUnfillable);
    EXPECT_EQ(book.resting_order_count(), 0u);
}

TEST_F(OrderBookTest, MarketFillOrKillNeedsTheWholeSizeAtAnyPrice) {
    submit(make.limit(1, Side::Sell, 100, 10));
    submit(make.limit(2, Side::Sell, 900, 10));

    EXPECT_TRUE(
        submit(make.with_tif(make.market(3, Side::Buy, 20), TimeInForce::FillOrKill)).accepted());

    fills.clear();
    EXPECT_EQ(submit(make.with_tif(make.market(4, Side::Buy, 1), TimeInForce::FillOrKill)).reject,
              RejectReason::FillOrKillUnfillable);
}

}  // namespace
}  // namespace xc
