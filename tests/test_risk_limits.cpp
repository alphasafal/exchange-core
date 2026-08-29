#include <gtest/gtest.h>

#include "order_book_fixture.hpp"
#include "xc/risk/risk_engine.hpp"

namespace xc::risk {
namespace {

using xc::testing::test_instrument;

constexpr std::uint64_t kAccount = 5;

NewOrder order(Side side, Price price, Quantity quantity, std::uint64_t id = 1,
               std::uint64_t account = kAccount) {
    NewOrder command;
    command.id = OrderId{id};
    command.account = AccountId{account};
    command.instrument = InstrumentId{1};
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    return command;
}

Order resting(Side side, Price price, Quantity quantity, std::uint64_t id = 1,
              std::uint64_t account = kAccount) {
    Order o;
    o.id = OrderId{id};
    o.account = AccountId{account};
    o.instrument = InstrumentId{1};
    o.side = side;
    o.price = price;
    o.quantity = quantity;
    o.remaining = quantity;
    return o;
}

class RiskTest : public ::testing::Test {
  protected:
    RiskEngine risk;
    Instrument instrument = test_instrument();

    /// Advances the clock on every call so that the rate limiter, which these
    /// tests do not configure, can never be the reason something is rejected.
    RejectReason check(const NewOrder& command) {
        now += 1'000'000;
        return risk.check(command, instrument, now);
    }

    Nanos now = 0;
};

TEST_F(RiskTest, AnUnconfiguredAccountIsUnconstrained) {
    // A venue must be able to run this component before every account has been
    // onboarded. Silently rejecting an unconfigured account would be a worse
    // failure than not checking it.
    EXPECT_EQ(check(order(Side::Buy, 100, 1'000'000)), RejectReason::None);
    EXPECT_EQ(risk.limits_for(AccountId{kAccount}), nullptr);
}

TEST_F(RiskTest, EnforcesMaxOrderQuantity) {
    AccountLimits limits;
    limits.max_order_quantity = 100;
    risk.configure(AccountId{kAccount}, limits);

    EXPECT_EQ(check(order(Side::Buy, 10, 100)), RejectReason::None);
    EXPECT_EQ(check(order(Side::Buy, 10, 101)), RejectReason::RiskLimit);
}

TEST_F(RiskTest, EnforcesMaxOrderNotional) {
    AccountLimits limits;
    limits.max_order_notional = 10'000;
    risk.configure(AccountId{kAccount}, limits);

    EXPECT_EQ(check(order(Side::Buy, 100, 100)), RejectReason::None);
    EXPECT_EQ(check(order(Side::Buy, 100, 101)), RejectReason::RiskLimit);

    // The quantity and notional limits catch different mistakes: this order is
    // small in lots but large in value.
    EXPECT_EQ(check(order(Side::Buy, 1'000'000, 1)), RejectReason::RiskLimit);
}

TEST_F(RiskTest, DoesNotLetANotionalOverflowSlipPastTheLimit) {
    AccountLimits limits;
    limits.max_order_notional = 1'000'000;
    risk.configure(AccountId{kAccount}, limits);

    // price * quantity wraps a 64-bit unsigned here. Wrapping to a small number
    // would pass a limit the order should fail by an astronomical margin.
    const Price huge_price = 1LL << 62;
    const Quantity huge_quantity = 1ULL << 10;
    EXPECT_EQ(check(order(Side::Buy, huge_price, huge_quantity)), RejectReason::RiskLimit);
}

TEST_F(RiskTest, DoesNotApplyNotionalToMarketOrders) {
    AccountLimits limits;
    limits.max_order_notional = 10;
    risk.configure(AccountId{kAccount}, limits);

    // A market order's cost depends on liquidity that has not been consumed
    // yet, so there is no honest notional to check it against; the quantity and
    // position limits bound it instead.
    NewOrder command = order(Side::Buy, kNoPrice, 1);
    command.type = OrderType::Market;
    EXPECT_EQ(check(command), RejectReason::None);
}

TEST_F(RiskTest, EnforcesMaxOpenOrders) {
    AccountLimits limits;
    limits.max_open_orders = 2;
    risk.configure(AccountId{kAccount}, limits);

    EXPECT_EQ(check(order(Side::Buy, 100, 1)), RejectReason::None);
    risk.on_order_rested(resting(Side::Buy, 100, 1, 1));
    risk.on_order_rested(resting(Side::Buy, 100, 1, 2));
    EXPECT_EQ(risk.open_orders(AccountId{kAccount}), 2u);
    EXPECT_EQ(check(order(Side::Buy, 100, 1, 3)), RejectReason::RiskLimit);

    risk.on_order_closed(AccountId{kAccount});
    EXPECT_EQ(check(order(Side::Buy, 100, 1, 3)), RejectReason::None);
}

TEST_F(RiskTest, CountsRestingOrdersTowardThePositionLimit) {
    AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kAccount}, limits);

    risk.on_order_rested(resting(Side::Buy, 100, 60, 1));

    // The account is flat, but 60 lots of bids are one fill away from being a
    // position. Treating it as flat would let it build an unlimited position
    // one resting order at a time.
    EXPECT_EQ(risk.exposure(AccountId{kAccount}, InstrumentId{1}).net_position, 0);
    EXPECT_EQ(check(order(Side::Buy, 100, 40, 2)), RejectReason::None);
    EXPECT_EQ(check(order(Side::Buy, 100, 41, 2)), RejectReason::RiskLimit);
}

TEST_F(RiskTest, TreatsLongAndShortLimitsIndependently) {
    AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kAccount}, limits);

    risk.on_order_rested(resting(Side::Buy, 100, 90, 1));

    // A resting bid does not restrict how much the account may offer -- the two
    // sides can be worked at once without either breaching the cap.
    EXPECT_EQ(check(order(Side::Buy, 100, 20, 2)), RejectReason::RiskLimit);
    EXPECT_EQ(check(order(Side::Sell, 100, 90, 3)), RejectReason::None);
}

TEST_F(RiskTest, MovesQuantityFromCommitmentToPositionOnAFill) {
    AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kAccount}, limits);
    risk.configure(AccountId{99}, limits);

    risk.on_order_rested(resting(Side::Buy, 100, 50, 1));
    ASSERT_EQ(risk.exposure(AccountId{kAccount}, InstrumentId{1}).working_buy, 50u);

    Fill fill;
    fill.instrument = InstrumentId{1};
    fill.price = 100;
    fill.quantity = 30;
    fill.aggressor_account = AccountId{99};
    fill.resting_account = AccountId{kAccount};
    fill.aggressor_side = Side::Sell;
    risk.on_fill(fill);

    const Exposure resting_side = risk.exposure(AccountId{kAccount}, InstrumentId{1});
    EXPECT_EQ(resting_side.net_position, 30) << "the resting buyer is now long";
    EXPECT_EQ(resting_side.working_buy, 20u) << "and has 20 still working";

    // Both sides move at the same instant. Updating only the aggressor would
    // leave the other holding a position the venue does not know about.
    const Exposure aggressor_side = risk.exposure(AccountId{99}, InstrumentId{1});
    EXPECT_EQ(aggressor_side.net_position, -30);
}

TEST_F(RiskTest, ReleasesCommitmentWhenAnOrderIsCancelled) {
    AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kAccount}, limits);

    risk.on_order_rested(resting(Side::Buy, 100, 100, 1));
    ASSERT_EQ(check(order(Side::Buy, 100, 1, 2)), RejectReason::RiskLimit);

    risk.on_working_released(AccountId{kAccount}, InstrumentId{1}, Side::Buy, 100);
    EXPECT_EQ(risk.exposure(AccountId{kAccount}, InstrumentId{1}).working_buy, 0u);
    EXPECT_EQ(check(order(Side::Buy, 100, 100, 2)), RejectReason::None);
}

TEST_F(RiskTest, ClampsRatherThanUnderflowsWhenCommitmentIsReleasedTwice) {
    AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kAccount}, limits);
    risk.on_order_rested(resting(Side::Buy, 100, 40, 1));

    risk.on_working_released(AccountId{kAccount}, InstrumentId{1}, Side::Buy, 40);
    risk.on_working_released(AccountId{kAccount}, InstrumentId{1}, Side::Buy, 40);

    // An unsigned underflow here would look like an enormous commitment and
    // lock the account out of the venue permanently. Ending up flat is the safe
    // failure.
    EXPECT_EQ(risk.exposure(AccountId{kAccount}, InstrumentId{1}).working_buy, 0u);
    EXPECT_EQ(check(order(Side::Buy, 100, 100, 2)), RejectReason::None);
}

TEST_F(RiskTest, KeepsExposureSeparatePerInstrument) {
    AccountLimits limits;
    limits.max_position = 50;
    risk.configure(AccountId{kAccount}, limits);

    Order other = resting(Side::Buy, 100, 50, 1);
    other.instrument = InstrumentId{2};
    risk.on_order_rested(other);

    // A position in one instrument must not consume another's limit.
    EXPECT_EQ(check(order(Side::Buy, 100, 50, 2)), RejectReason::None);
    EXPECT_EQ(risk.exposure(AccountId{kAccount}, InstrumentId{1}).working_buy, 0u);
    EXPECT_EQ(risk.exposure(AccountId{kAccount}, InstrumentId{2}).working_buy, 50u);
}

TEST_F(RiskTest, ShortPositionsAreCappedByTheSameLimit) {
    AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kAccount}, limits);
    risk.configure(AccountId{99}, limits);

    risk.on_order_rested(resting(Side::Sell, 100, 100, 1));
    Fill fill;
    fill.instrument = InstrumentId{1};
    fill.quantity = 100;
    fill.aggressor_account = AccountId{99};
    fill.resting_account = AccountId{kAccount};
    fill.aggressor_side = Side::Buy;
    risk.on_fill(fill);

    ASSERT_EQ(risk.exposure(AccountId{kAccount}, InstrumentId{1}).net_position, -100);
    EXPECT_EQ(check(order(Side::Sell, 100, 1, 2)), RejectReason::RiskLimit);
    EXPECT_EQ(check(order(Side::Buy, 100, 100, 3)), RejectReason::None)
        << "buying back reduces the short and must stay permitted";
}

}  // namespace
}  // namespace xc::risk
