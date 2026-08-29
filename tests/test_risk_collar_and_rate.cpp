#include <gtest/gtest.h>

#include "order_book_fixture.hpp"
#include "xc/risk/risk_engine.hpp"

namespace xc::risk {
namespace {

using xc::testing::test_instrument;

constexpr std::uint64_t kAccount = 5;

NewOrder limit_order(Price price, Quantity quantity = 1, std::uint64_t account = kAccount) {
    NewOrder command;
    command.id = OrderId{1};
    command.account = AccountId{account};
    command.instrument = InstrumentId{1};
    command.side = Side::Buy;
    command.type = OrderType::Limit;
    command.price = price;
    command.quantity = quantity;
    return command;
}

// --- Price collar ----------------------------------------------------------

class CollarTest : public ::testing::Test {
  protected:
    RiskEngine risk;
    Instrument instrument = test_instrument();
    Nanos now = 0;

    void SetUp() override {
        InstrumentControls controls;
        controls.collar_bps = 500;  // 5%
        risk.configure_instrument(InstrumentId{1}, controls);
    }

    RejectReason check(const NewOrder& command) {
        now += 1'000'000;
        return risk.check(command, instrument, now);
    }
};

TEST_F(CollarTest, AdmitsEverythingBeforeAReferencePriceExists) {
    // A venue that has not traded yet cannot tell a fat finger from price
    // discovery. Guessing would reject the first legitimate order of the
    // session.
    EXPECT_EQ(risk.reference_price(InstrumentId{1}), kNoPrice);
    EXPECT_EQ(check(limit_order(1)), RejectReason::None);
    EXPECT_EQ(check(limit_order(1'000'000)), RejectReason::None);
}

TEST_F(CollarTest, AdmitsPricesInsideTheBand) {
    risk.set_reference_price(InstrumentId{1}, 10'000);
    EXPECT_EQ(check(limit_order(10'000)), RejectReason::None);
    EXPECT_EQ(check(limit_order(10'500)), RejectReason::None) << "exactly 5% above";
    EXPECT_EQ(check(limit_order(9'500)), RejectReason::None) << "exactly 5% below";
}

TEST_F(CollarTest, RejectsPricesOutsideTheBandInBothDirections) {
    risk.set_reference_price(InstrumentId{1}, 10'000);
    EXPECT_EQ(check(limit_order(10'501)), RejectReason::PriceCollar);
    EXPECT_EQ(check(limit_order(9'499)), RejectReason::PriceCollar);

    // The case the collar exists for: a misplaced decimal point that would
    // otherwise sweep every resting order on the book.
    EXPECT_EQ(check(limit_order(100'000)), RejectReason::PriceCollar);
}

TEST_F(CollarTest, DoesNotOverflowOnAnExtremePrice) {
    risk.set_reference_price(InstrumentId{1}, 10'000);
    // Scaling this deviation by 10000 overflows a signed 64-bit value. Wrapping
    // would turn the wildest possible price into a small deviation and admit it.
    EXPECT_EQ(check(limit_order((1LL << 62) + 1)), RejectReason::PriceCollar);
}

TEST_F(CollarTest, DoesNotApplyToMarketOrders) {
    risk.set_reference_price(InstrumentId{1}, 10'000);
    NewOrder command = limit_order(kNoPrice);
    command.type = OrderType::Market;
    EXPECT_EQ(check(command), RejectReason::None);
}

TEST_F(CollarTest, IsDisabledWhenNoCollarIsConfigured) {
    RiskEngine unconstrained;
    unconstrained.set_reference_price(InstrumentId{1}, 10'000);
    EXPECT_EQ(unconstrained.check(limit_order(1'000'000), instrument, 1), RejectReason::None);
}

TEST_F(CollarTest, FollowsTheReferencePriceAsItMoves) {
    risk.set_reference_price(InstrumentId{1}, 10'000);
    ASSERT_EQ(check(limit_order(12'000)), RejectReason::PriceCollar);

    // The market moved there legitimately, one trade at a time. The collar must
    // follow, or it turns into a halt on any instrument that trends.
    risk.set_reference_price(InstrumentId{1}, 11'500);
    EXPECT_EQ(check(limit_order(12'000)), RejectReason::None);
}

TEST_F(CollarTest, IgnoresANonPositiveReferencePrice) {
    risk.set_reference_price(InstrumentId{1}, 10'000);
    risk.set_reference_price(InstrumentId{1}, 0);
    EXPECT_EQ(risk.reference_price(InstrumentId{1}), 10'000)
        << "a meaningless reference must not replace a good one";
}

// --- Message rate limiting -------------------------------------------------

class RateLimitTest : public ::testing::Test {
  protected:
    RiskEngine risk;
    Instrument instrument = test_instrument();

    void configure(std::uint32_t per_second, std::uint32_t burst) {
        AccountLimits limits;
        limits.max_messages_per_second = per_second;
        limits.message_burst = burst;
        risk.configure(AccountId{kAccount}, limits);
    }

    RejectReason at(Nanos now) { return risk.check(limit_order(100), instrument, now); }
};

TEST_F(RateLimitTest, IsDisabledByDefault) {
    AccountLimits limits;
    risk.configure(AccountId{kAccount}, limits);
    for (int i = 0; i < 1000; ++i) {
        ASSERT_EQ(at(0), RejectReason::None) << "message " << i << " at the same instant";
    }
}

TEST_F(RateLimitTest, AdmitsUpToTheBurstBackToBack) {
    configure(/*per_second=*/1000, /*burst=*/5);

    // Real order flow is bursty: a market maker requoting a ladder sends a
    // dozen messages in a microsecond and then nothing for a second.
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(at(0), RejectReason::None) << "burst message " << i;
    }
    EXPECT_EQ(at(0), RejectReason::RateLimit) << "the burst is exhausted";
}

TEST_F(RateLimitTest, RefillsSteadilyWithTime) {
    configure(/*per_second=*/1000, /*burst=*/1);
    constexpr Nanos kInterval = 1'000'000;  // one millisecond per message

    ASSERT_EQ(at(0), RejectReason::None);
    EXPECT_EQ(at(kInterval / 2), RejectReason::RateLimit) << "too soon";
    EXPECT_EQ(at(kInterval), RejectReason::None);
    EXPECT_EQ(at(2 * kInterval), RejectReason::None);
}

TEST_F(RateLimitTest, HasNoWindowBoundaryToExploit) {
    configure(/*per_second=*/100, /*burst=*/100);
    constexpr Nanos kSecond = 1'000'000'000;

    // A fixed-window counter admits the whole allowance at the end of one
    // window and again at the start of the next -- twice the configured rate
    // across the boundary. Draining the burst just before a second elapses and
    // trying again just after must not be admitted.
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(at(kSecond - 1), RejectReason::None) << "priming message " << i;
    }
    EXPECT_EQ(at(kSecond + 1), RejectReason::RateLimit);
}

TEST_F(RateLimitTest, ChargesForMessagesItRejectsForOtherReasons) {
    AccountLimits limits;
    limits.max_messages_per_second = 1000;
    limits.message_burst = 2;
    limits.max_order_quantity = 1;
    risk.configure(AccountId{kAccount}, limits);

    // An oversized order still cost the matching thread the work of rejecting
    // it. An account that could send unlimited invalid orders for free would
    // defeat the rate limit entirely.
    EXPECT_EQ(risk.check(limit_order(100, 999), instrument, 0), RejectReason::RiskLimit);
    EXPECT_EQ(risk.check(limit_order(100, 999), instrument, 0), RejectReason::RiskLimit);
    EXPECT_EQ(risk.check(limit_order(100, 1), instrument, 0), RejectReason::RateLimit)
        << "the two rejected messages consumed the burst";
}

TEST_F(RateLimitTest, IsCheckedBeforeMoreExpensiveControls) {
    AccountLimits limits;
    limits.max_messages_per_second = 1000;
    limits.message_burst = 1;
    limits.max_order_quantity = 1;
    risk.configure(AccountId{kAccount}, limits);

    ASSERT_EQ(risk.check(limit_order(100, 1), instrument, 0), RejectReason::None);
    // A flood should be turned away before anything more expensive runs, so the
    // rate limit is the reason reported even though the order also breaches a
    // quantity limit.
    EXPECT_EQ(risk.check(limit_order(100, 999), instrument, 0), RejectReason::RateLimit);
}

TEST_F(RateLimitTest, LimitsEachAccountIndependently) {
    configure(/*per_second=*/1000, /*burst=*/1);
    AccountLimits other;
    other.max_messages_per_second = 1000;
    other.message_burst = 1;
    risk.configure(AccountId{99}, other);

    ASSERT_EQ(risk.check(limit_order(100, 1, kAccount), instrument, 0), RejectReason::None);
    EXPECT_EQ(risk.check(limit_order(100, 1, kAccount), instrument, 0), RejectReason::RateLimit);
    EXPECT_EQ(risk.check(limit_order(100, 1, 99), instrument, 0), RejectReason::None)
        << "one account's flood must not throttle another";
}

}  // namespace
}  // namespace xc::risk
