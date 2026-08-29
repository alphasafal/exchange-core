#include <gtest/gtest.h>

#include <vector>

#include "order_book_fixture.hpp"
#include "xc/core/order_book.hpp"

namespace xc {
namespace {

using testing::OrderFactory;
using testing::test_instrument;

Instrument with_policy(SelfTradePolicy policy) {
    Instrument instrument = test_instrument();
    instrument.self_trade_policy = policy;
    return instrument;
}

constexpr std::uint64_t kFirm = 7;
constexpr std::uint64_t kOther = 9;

// --- Self-trade prevention -------------------------------------------------

TEST(SelfTrade, AllowLetsAnAccountTradeWithItself) {
    OrderBook book{with_policy(SelfTradePolicy::Allow)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kFirm), fills);
    const SubmitResult result = book.submit(make.limit(2, Side::Buy, 100, 10, kFirm), fills);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(result.filled, 10u);
    EXPECT_EQ(result.stp_cancelled, 0u);
}

TEST(SelfTrade, PreventionOnlyAppliesWithinAnAccount) {
    OrderBook book{with_policy(SelfTradePolicy::CancelIncoming)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kOther), fills);
    const SubmitResult result = book.submit(make.limit(2, Side::Buy, 100, 10, kFirm), fills);

    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(result.filled, 10u);
    EXPECT_EQ(result.stp_cancelled, 0u);
}

TEST(SelfTrade, CancelIncomingDropsTheAggressorAndKeepsTheRestingQueuePosition) {
    OrderBook book{with_policy(SelfTradePolicy::CancelIncoming)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kFirm), fills);
    book.submit(make.limit(2, Side::Sell, 101, 50, kOther), fills);

    const SubmitResult result = book.submit(make.limit(3, Side::Buy, 101, 30, kFirm), fills);

    EXPECT_TRUE(fills.empty()) << "nothing traded";
    EXPECT_EQ(result.filled, 0u);
    EXPECT_EQ(result.stp_cancelled, 30u);
    EXPECT_FALSE(result.rested) << "the aggressor was cancelled, not rested";

    // The firm's resting order keeps the queue position it paid for, and the
    // liquidity behind it is untouched -- the aggressor stopped dead.
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 10u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 101), 50u);
}

TEST(SelfTrade, CancelIncomingKeepsFillsMadeBeforeTheCollision) {
    OrderBook book{with_policy(SelfTradePolicy::CancelIncoming)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kOther), fills);
    book.submit(make.limit(2, Side::Sell, 101, 10, kFirm), fills);

    const SubmitResult result = book.submit(make.limit(3, Side::Buy, 101, 30, kFirm), fills);

    ASSERT_EQ(fills.size(), 1u) << "the trade against the other account stands";
    EXPECT_EQ(fills[0].price, 100);
    EXPECT_EQ(result.filled, 10u);
    EXPECT_EQ(result.stp_cancelled, 20u);
    EXPECT_EQ(result.filled + result.stp_cancelled, 30u) << "the order is fully accounted for";
}

TEST(SelfTrade, CancelRestingRemovesTheBookSideAndCarriesOn) {
    OrderBook book{with_policy(SelfTradePolicy::CancelResting)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kFirm), fills);
    book.submit(make.limit(2, Side::Sell, 100, 10, kOther), fills);

    const SubmitResult result = book.submit(make.limit(3, Side::Buy, 100, 10, kFirm), fills);

    ASSERT_EQ(fills.size(), 1u) << "the aggressor moved past its own order and traded";
    EXPECT_EQ(fills[0].resting_order, OrderId{2});
    EXPECT_EQ(result.filled, 10u);
    EXPECT_EQ(result.stp_cancelled, 0u);
    EXPECT_EQ(book.find(OrderId{1}), nullptr) << "the firm's resting order was pulled";
    EXPECT_EQ(book.resting_order_count(), 0u);
}

TEST(SelfTrade, CancelBothRemovesTheRestingOrderAndTheAggressor) {
    OrderBook book{with_policy(SelfTradePolicy::CancelBoth)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kFirm), fills);
    book.submit(make.limit(2, Side::Sell, 101, 10, kOther), fills);

    const SubmitResult result = book.submit(make.limit(3, Side::Buy, 101, 10, kFirm), fills);

    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(result.stp_cancelled, 10u);
    EXPECT_EQ(book.find(OrderId{1}), nullptr);
    EXPECT_EQ(book.quantity_at(Side::Sell, 101), 10u) << "the other account is unaffected";
}

TEST(SelfTrade, DecrementBothShrinksEachSideWithoutPrintingATrade) {
    OrderBook book{with_policy(SelfTradePolicy::DecrementBoth)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 30, kFirm), fills);
    const SubmitResult result = book.submit(make.limit(2, Side::Buy, 100, 10, kFirm), fills);

    // Nothing changed hands, so nothing may reach the tape or a position
    // calculation -- but both orders must shrink by the overlap.
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(result.filled, 0u);
    EXPECT_EQ(result.stp_cancelled, 10u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 20u);
    EXPECT_EQ(book.find(OrderId{1})->remaining, 20u);
}

TEST(SelfTrade, DecrementBothRemovesARestingOrderItFullyConsumes) {
    OrderBook book{with_policy(SelfTradePolicy::DecrementBoth)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kFirm), fills);
    book.submit(make.limit(2, Side::Sell, 100, 10, kOther), fills);

    const SubmitResult result = book.submit(make.limit(3, Side::Buy, 100, 25, kFirm), fills);

    ASSERT_EQ(fills.size(), 1u) << "after decrementing its own order it trades with the other";
    EXPECT_EQ(fills[0].quantity, 10u);
    EXPECT_EQ(result.filled, 10u);
    EXPECT_EQ(result.stp_cancelled, 10u);
    EXPECT_TRUE(result.rested) << "5 remains and rests";
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 5u);
}

TEST(SelfTrade, DecrementBothWithdrawsQuantityRatherThanReportingItAsFilled) {
    OrderBook book{with_policy(SelfTradePolicy::DecrementBoth)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 100, kFirm), fills);
    book.submit(make.limit(2, Side::Buy, 100, 30, kFirm), fills);
    ASSERT_TRUE(fills.empty());

    // The resting order lost 30 lots to prevention, and traded none of them.
    // Reporting them as filled would put the client's position 30 lots away
    // from the venue's, with no execution anywhere to explain the difference.
    const Order* resting = book.find(OrderId{1});
    ASSERT_NE(resting, nullptr);
    EXPECT_EQ(resting->remaining, 70u);
    EXPECT_EQ(resting->quantity, 70u) << "the working quantity shrank with the remainder";
    EXPECT_EQ(resting->filled(), 0u) << "nothing traded, so nothing is filled";

    // Once it does trade, filled() counts only that.
    book.submit(make.limit(3, Side::Buy, 100, 20, kOther), fills);
    ASSERT_EQ(fills.size(), 1u);
    EXPECT_EQ(book.find(OrderId{1})->filled(), 20u);
    EXPECT_EQ(book.find(OrderId{1})->remaining, 50u);
}

// --- Self-trade prevention interacting with fill-or-kill -------------------

TEST(SelfTrade, FillOrKillDoesNotCountLiquidityBlockedByCancelIncoming) {
    OrderBook book{with_policy(SelfTradePolicy::CancelIncoming)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kOther), fills);
    book.submit(make.limit(2, Side::Sell, 101, 100, kFirm), fills);
    book.submit(make.limit(3, Side::Sell, 102, 100, kOther), fills);
    fills.clear();

    // Reading level totals would see 210 available and accept. But matching
    // stops dead at the firm's own order on 101, so only 10 is truly
    // reachable -- and accepting would fill 10 of a 50 lot all-or-nothing
    // order, which is exactly the inconsistency two-phase fill-or-kill exists
    // to prevent.
    const SubmitResult result = book.submit(
        make.with_tif(make.limit(4, Side::Buy, 102, 50, kFirm), TimeInForce::FillOrKill), fills);

    EXPECT_EQ(result.reject, RejectReason::FillOrKillUnfillable);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 10u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 101), 100u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 102), 100u);
}

TEST(SelfTrade, FillOrKillSkipsOwnLiquidityButCountsWhatIsBehindItUnderCancelResting) {
    OrderBook book{with_policy(SelfTradePolicy::CancelResting)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kOther), fills);
    book.submit(make.limit(2, Side::Sell, 101, 100, kFirm), fills);
    book.submit(make.limit(3, Side::Sell, 102, 100, kOther), fills);
    fills.clear();

    // Here the aggressor pulls its own order and keeps going, so the size at
    // 102 really is reachable and the order can fill.
    const SubmitResult result = book.submit(
        make.with_tif(make.limit(4, Side::Buy, 102, 50, kFirm), TimeInForce::FillOrKill), fills);

    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(result.filled, 50u);
    EXPECT_EQ(book.find(OrderId{2}), nullptr) << "own resting order was pulled";
    EXPECT_EQ(book.quantity_at(Side::Sell, 102), 60u);
}

TEST(SelfTrade, FillOrKillRejectsWhenOnlyItsOwnLiquidityWouldFillIt) {
    OrderBook book{with_policy(SelfTradePolicy::CancelResting)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 100, kFirm), fills);
    fills.clear();

    const SubmitResult result = book.submit(
        make.with_tif(make.limit(2, Side::Buy, 100, 50, kFirm), TimeInForce::FillOrKill), fills);

    EXPECT_EQ(result.reject, RejectReason::FillOrKillUnfillable);
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 100u) << "the resting order must not be pulled";
}

TEST(SelfTrade, FillOrKillRejectsWhenDecrementBothWouldDestroyPartOfIt) {
    OrderBook book{with_policy(SelfTradePolicy::DecrementBoth)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10, kFirm), fills);
    book.submit(make.limit(2, Side::Sell, 100, 50, kOther), fills);
    fills.clear();

    // Found by the differential harness. There are 60 lots resting at a price
    // this order will pay, so counting tradeable liquidity says "fillable".
    // But decrement-both consumes 10 of the aggressor's own 50 against its own
    // resting order without printing a trade, leaving only 40 that can actually
    // fill. Accepting would fill 40 of an all-or-nothing 50 -- the exact
    // outcome fill-or-kill exists to rule out.
    const SubmitResult result = book.submit(
        make.with_tif(make.limit(3, Side::Buy, 100, 50, kFirm), TimeInForce::FillOrKill), fills);

    EXPECT_EQ(result.reject, RejectReason::FillOrKillUnfillable);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(result.filled, 0u);
    EXPECT_EQ(result.stp_cancelled, 0u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 60u) << "nothing may move";
    EXPECT_EQ(book.find(OrderId{1})->remaining, 10u);
    EXPECT_EQ(book.find(OrderId{2})->remaining, 50u);
}

TEST(SelfTrade, FillOrKillSucceedsWhenItFillsBeforeReachingItsOwnOrder) {
    OrderBook book{with_policy(SelfTradePolicy::DecrementBoth)};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 50, kOther), fills);
    book.submit(make.limit(2, Side::Sell, 101, 10, kFirm), fills);
    fills.clear();

    // The own order sits a tick behind and is never reached, so the order fills
    // completely. The check has to be precise in both directions: rejecting
    // this would be just as wrong as accepting the case above.
    const SubmitResult result = book.submit(
        make.with_tif(make.limit(3, Side::Buy, 101, 50, kFirm), TimeInForce::FillOrKill), fills);

    EXPECT_TRUE(result.accepted());
    EXPECT_EQ(result.filled, 50u);
    EXPECT_EQ(result.stp_cancelled, 0u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 101), 10u) << "the own order is untouched";
}

// --- Post-only -------------------------------------------------------------

Order post_only(Order order) {
    order.post_only = true;
    return order;
}

TEST(PostOnly, RestsWhenItAddsLiquidity) {
    OrderBook book{test_instrument()};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 101, 10), fills);
    const SubmitResult result = book.submit(post_only(make.limit(2, Side::Buy, 100, 10)), fills);

    EXPECT_TRUE(result.accepted());
    EXPECT_TRUE(result.rested);
    EXPECT_EQ(book.best_bid(), 100);
}

TEST(PostOnly, IsRejectedRatherThanFilledWhenItWouldCross) {
    OrderBook book{test_instrument()};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10), fills);
    const SubmitResult result = book.submit(post_only(make.limit(2, Side::Buy, 100, 10)), fills);

    // A market maker sends post-only because paying the spread inverts its
    // economics. Trading it and reporting the fact afterwards is the one
    // outcome it cannot use.
    EXPECT_EQ(result.reject, RejectReason::PostOnlyWouldCross);
    EXPECT_TRUE(fills.empty());
    EXPECT_EQ(result.filled, 0u);
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 10u);
    EXPECT_EQ(book.resting_order_count(), 1u);
}

TEST(PostOnly, RestsAtTheTouchWithoutCrossingIt) {
    OrderBook book{test_instrument()};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 101, 10), fills);
    // Joining the bid one tick inside the offer adds liquidity; it does not
    // take any. This is the case post-only exists to allow.
    EXPECT_TRUE(book.submit(post_only(make.limit(2, Side::Buy, 100, 10)), fills).rested);
    EXPECT_TRUE(fills.empty());
}

TEST(PostOnly, RejectsAMarketOrderBecauseItAlwaysTakes) {
    OrderBook book{test_instrument()};
    OrderFactory make;
    std::vector<Fill> fills;

    book.submit(make.limit(1, Side::Sell, 100, 10), fills);
    EXPECT_EQ(book.submit(post_only(make.market(2, Side::Buy, 10)), fills).reject,
              RejectReason::PostOnlyWouldCross);
}

TEST(PostOnly, RestsAgainstAnEmptyBook) {
    OrderBook book{test_instrument()};
    OrderFactory make;
    std::vector<Fill> fills;
    EXPECT_TRUE(book.submit(post_only(make.limit(1, Side::Buy, 100, 10)), fills).rested);
}

}  // namespace
}  // namespace xc
