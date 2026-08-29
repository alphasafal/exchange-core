#include <gtest/gtest.h>

#include <vector>

#include "order_book_fixture.hpp"
#include "xc/core/matching_engine.hpp"

namespace xc {
namespace {

using testing::test_instrument;

constexpr std::uint64_t kFirm = 5;
constexpr std::uint64_t kOther = 9;

Instrument instrument_with(SelfTradePolicy policy) {
    Instrument instrument = test_instrument();
    instrument.id = InstrumentId{1};
    instrument.symbol = "AAA";
    instrument.self_trade_policy = policy;
    return instrument;
}

class EngineRiskTest : public ::testing::Test {
  protected:
    ManualClock clock{1'000'000};
    MatchingEngine engine{clock};
    risk::RiskEngine risk;
    risk::KillSwitch kill;

    void SetUp() override { install(SelfTradePolicy::Allow); }

    void install(SelfTradePolicy policy) {
        ASSERT_TRUE(engine.add_instrument(instrument_with(policy)));
        engine.set_risk_engine(&risk);
        engine.set_kill_switch(&kill);
    }

    NewOrder order(std::uint64_t id, Side side, Price price, Quantity quantity,
                   std::uint64_t account = kFirm) {
        NewOrder command;
        command.id = OrderId{id};
        command.account = AccountId{account};
        command.instrument = InstrumentId{1};
        command.side = side;
        command.price = price;
        command.quantity = quantity;
        return command;
    }

    SubmitOutcome submit(const NewOrder& command) {
        clock.advance(1'000'000);
        return engine.submit(command);
    }

    risk::Exposure exposure(std::uint64_t account = kFirm) const {
        return risk.exposure(AccountId{account}, InstrumentId{1});
    }
};

TEST_F(EngineRiskTest, RejectsAnOrderThatBreachesALimitBeforeItReachesTheBook) {
    risk::AccountLimits limits;
    limits.max_order_quantity = 10;
    risk.configure(AccountId{kFirm}, limits);

    const SubmitOutcome outcome = submit(order(1, Side::Buy, 100, 11));
    EXPECT_EQ(outcome.reject, RejectReason::RiskLimit);
    EXPECT_EQ(engine.book(InstrumentId{1})->resting_order_count(), 0u)
        << "a pre-trade check that lets the order reach the book is not a pre-trade check";
}

TEST_F(EngineRiskTest, TracksExposureThroughRestingAndFilling) {
    risk::AccountLimits limits;
    limits.max_position = 1000;
    risk.configure(AccountId{kFirm}, limits);
    risk.configure(AccountId{kOther}, limits);

    submit(order(1, Side::Buy, 100, 50, kFirm));
    EXPECT_EQ(exposure().working_buy, 50u);
    EXPECT_EQ(exposure().net_position, 0);
    EXPECT_EQ(risk.open_orders(AccountId{kFirm}), 1u);

    submit(order(2, Side::Sell, 100, 30, kOther));
    EXPECT_EQ(exposure().working_buy, 20u);
    EXPECT_EQ(exposure().net_position, 30);
    EXPECT_EQ(exposure(kOther).net_position, -30);
    EXPECT_EQ(risk.open_orders(AccountId{kFirm}), 1u) << "still resting, partly filled";
}

TEST_F(EngineRiskTest, ReleasesExposureWhenAnOrderIsFullyFilled) {
    risk::AccountLimits limits;
    limits.max_position = 1000;
    risk.configure(AccountId{kFirm}, limits);
    risk.configure(AccountId{kOther}, limits);

    submit(order(1, Side::Buy, 100, 50, kFirm));
    submit(order(2, Side::Sell, 100, 50, kOther));

    EXPECT_EQ(exposure().working_buy, 0u);
    EXPECT_EQ(exposure().net_position, 50);
    EXPECT_EQ(risk.open_orders(AccountId{kFirm}), 0u);
}

TEST_F(EngineRiskTest, ReleasesExposureOnCancellation) {
    risk::AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kFirm}, limits);

    submit(order(1, Side::Buy, 100, 100, kFirm));
    ASSERT_EQ(submit(order(2, Side::Buy, 100, 1, kFirm)).reject, RejectReason::RiskLimit);

    CancelOrder cancel{OrderId{1}, AccountId{kFirm}, InstrumentId{1}};
    ASSERT_TRUE(engine.cancel(cancel).accepted());
    EXPECT_EQ(exposure().working_buy, 0u);
    EXPECT_EQ(risk.open_orders(AccountId{kFirm}), 0u);
    EXPECT_TRUE(submit(order(3, Side::Buy, 100, 100, kFirm)).accepted());
}

TEST_F(EngineRiskTest, ReleasesExposureWhenPreventionWithdrawsARestingOrder) {
    // The case with no fill and no cancel request to attribute it to. Without
    // an explicit withdrawal record the firm's exposure would never come back,
    // and it would slowly lose the ability to trade for no visible reason.
    MatchingEngine venue{clock};
    risk::RiskEngine venue_risk;
    ASSERT_TRUE(venue.add_instrument(instrument_with(SelfTradePolicy::CancelResting)));
    venue.set_risk_engine(&venue_risk);

    risk::AccountLimits limits;
    limits.max_position = 100;
    limits.max_open_orders = 10;
    venue_risk.configure(AccountId{kFirm}, limits);

    venue.submit(order(1, Side::Sell, 100, 40, kFirm));
    ASSERT_EQ(venue_risk.exposure(AccountId{kFirm}, InstrumentId{1}).working_sell, 40u);
    ASSERT_EQ(venue_risk.open_orders(AccountId{kFirm}), 1u);

    // The firm's own buy pulls its own resting offer aside, finds nothing
    // behind it, and rests instead. The offer's exposure must be released and
    // the bid's recorded -- the account ends up with one open order on the
    // other side, not two, and not zero.
    venue.submit(order(2, Side::Buy, 100, 40, kFirm));

    const risk::Exposure after = venue_risk.exposure(AccountId{kFirm}, InstrumentId{1});
    EXPECT_EQ(after.working_sell, 0u) << "the withdrawn offer's exposure came back";
    EXPECT_EQ(after.working_buy, 40u) << "and the new bid's was recorded";
    EXPECT_EQ(after.net_position, 0) << "nothing traded";
    EXPECT_EQ(venue_risk.open_orders(AccountId{kFirm}), 1u);
}

TEST_F(EngineRiskTest, ReleasesExposureWhenPreventionShrinksARestingOrder) {
    MatchingEngine venue{clock};
    risk::RiskEngine venue_risk;
    ASSERT_TRUE(venue.add_instrument(instrument_with(SelfTradePolicy::DecrementBoth)));
    venue.set_risk_engine(&venue_risk);

    risk::AccountLimits limits;
    limits.max_position = 100;
    venue_risk.configure(AccountId{kFirm}, limits);

    venue.submit(order(1, Side::Sell, 100, 40, kFirm));
    venue.submit(order(2, Side::Buy, 100, 15, kFirm));

    // Only part of the resting order was withdrawn; it is still open.
    EXPECT_EQ(venue_risk.exposure(AccountId{kFirm}, InstrumentId{1}).working_sell, 25u);
    EXPECT_EQ(venue_risk.open_orders(AccountId{kFirm}), 1u);
}

TEST_F(EngineRiskTest, KeepsExposureCorrectAcrossAnInPlaceAmendment) {
    risk::AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kFirm}, limits);

    submit(order(1, Side::Buy, 100, 80, kFirm));
    ASSERT_EQ(exposure().working_buy, 80u);

    ReplaceOrder replace{OrderId{1}, AccountId{kFirm}, InstrumentId{1}, 100, 30};
    ASSERT_TRUE(engine.replace(replace).accepted());
    EXPECT_EQ(exposure().working_buy, 30u);
    EXPECT_EQ(risk.open_orders(AccountId{kFirm}), 1u);
}

TEST_F(EngineRiskTest, KeepsExposureCorrectAcrossARequeuedAmendment) {
    risk::AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kFirm}, limits);

    submit(order(1, Side::Buy, 100, 80, kFirm));
    ReplaceOrder replace{OrderId{1}, AccountId{kFirm}, InstrumentId{1}, 98, 80};
    ASSERT_TRUE(engine.replace(replace).accepted());

    EXPECT_EQ(exposure().working_buy, 80u) << "moved, not duplicated";
    EXPECT_EQ(risk.open_orders(AccountId{kFirm}), 1u);
}

TEST_F(EngineRiskTest, RestoresExposureWhenAnAmendmentIsRefused) {
    risk::AccountLimits limits;
    limits.max_position = 100;
    risk.configure(AccountId{kFirm}, limits);

    submit(order(1, Side::Buy, 100, 80, kFirm));
    ReplaceOrder replace{OrderId{1}, AccountId{kFirm}, InstrumentId{1}, 0, 80};
    ASSERT_EQ(engine.replace(replace).reject, RejectReason::InvalidPrice);

    // The order is untouched, so its exposure must be too.
    EXPECT_EQ(exposure().working_buy, 80u);
    EXPECT_EQ(risk.open_orders(AccountId{kFirm}), 1u);
}

// --- Kill switch on the command path ---------------------------------------

TEST_F(EngineRiskTest, AHaltedAccountCannotOpenNewOrders) {
    kill.halt_account(AccountId{kFirm}, risk::HaltReason::Manual, 0);
    EXPECT_EQ(submit(order(1, Side::Buy, 100, 10, kFirm)).reject, RejectReason::Halted);
    EXPECT_TRUE(submit(order(2, Side::Buy, 100, 10, kOther)).accepted());
}

TEST_F(EngineRiskTest, AHaltedAccountCanStillCancel) {
    submit(order(1, Side::Buy, 100, 10, kFirm));
    kill.halt_account(AccountId{kFirm}, risk::HaltReason::RiskBreach, 0);

    // The property the whole control depends on. An account is halted exactly
    // when it most needs to withdraw what it already has resting; refusing the
    // cancel would strand those orders in the market with no way to pull them.
    CancelOrder cancel{OrderId{1}, AccountId{kFirm}, InstrumentId{1}};
    EXPECT_TRUE(engine.cancel(cancel).accepted());
    EXPECT_EQ(engine.book(InstrumentId{1})->resting_order_count(), 0u);
}

TEST_F(EngineRiskTest, AHaltedAccountCanStillAmendDownToReduceRisk) {
    submit(order(1, Side::Buy, 100, 100, kFirm));
    kill.halt_account(AccountId{kFirm}, risk::HaltReason::Manual, 0);

    ReplaceOrder replace{OrderId{1}, AccountId{kFirm}, InstrumentId{1}, 100, 10};
    EXPECT_TRUE(engine.replace(replace).accepted());
    EXPECT_EQ(exposure().working_buy, 10u);
}

TEST_F(EngineRiskTest, AVenueHaltStopsEveryAccount) {
    kill.halt_venue(risk::HaltReason::Manual, 0);
    EXPECT_EQ(submit(order(1, Side::Buy, 100, 10, kFirm)).reject, RejectReason::Halted);
    EXPECT_EQ(submit(order(2, Side::Buy, 100, 10, kOther)).reject, RejectReason::Halted);

    kill.resume_venue();
    EXPECT_TRUE(submit(order(3, Side::Buy, 100, 10, kFirm)).accepted());
}

TEST_F(EngineRiskTest, TripsAnAccountAutomaticallyAfterARunOfRejects) {
    kill.set_auto_trip_threshold(3);
    risk::AccountLimits limits;
    limits.max_order_quantity = 10;
    risk.configure(AccountId{kFirm}, limits);

    for (std::uint64_t i = 1; i <= 3; ++i) {
        ASSERT_EQ(submit(order(i, Side::Buy, 100, 999, kFirm)).reject, RejectReason::RiskLimit);
    }
    ASSERT_TRUE(kill.account_halted(AccountId{kFirm}));

    // Now even a perfectly good order is refused, and the reason changes to
    // the halt rather than the limit that caused it.
    EXPECT_EQ(submit(order(4, Side::Buy, 100, 5, kFirm)).reject, RejectReason::Halted);
}

TEST_F(EngineRiskTest, AnAcceptedOrderClearsTheRejectRun) {
    kill.set_auto_trip_threshold(3);
    risk::AccountLimits limits;
    limits.max_order_quantity = 10;
    risk.configure(AccountId{kFirm}, limits);

    submit(order(1, Side::Buy, 100, 999, kFirm));
    submit(order(2, Side::Buy, 100, 999, kFirm));
    ASSERT_TRUE(submit(order(3, Side::Buy, 100, 5, kFirm)).accepted());
    submit(order(4, Side::Buy, 100, 999, kFirm));
    submit(order(5, Side::Buy, 100, 999, kFirm));

    EXPECT_FALSE(kill.account_halted(AccountId{kFirm}));
}

TEST_F(EngineRiskTest, WorksWithNoRiskComponentInstalled) {
    MatchingEngine bare{clock};
    ASSERT_TRUE(bare.add_instrument(instrument_with(SelfTradePolicy::Allow)));
    // The differential and benchmark harnesses run without risk; the engine
    // must not require it.
    EXPECT_TRUE(bare.submit(order(1, Side::Buy, 100, 1'000'000, kFirm)).accepted());
}

TEST_F(EngineRiskTest, UpdatesTheCollarReferenceFromTheLastTrade) {
    risk::InstrumentControls controls;
    controls.collar_bps = 500;
    risk.configure_instrument(InstrumentId{1}, controls);

    submit(order(1, Side::Sell, 100, 10, kOther));
    submit(order(2, Side::Buy, 100, 10, kFirm));
    EXPECT_EQ(risk.reference_price(InstrumentId{1}), 100);

    EXPECT_EQ(submit(order(3, Side::Buy, 200, 10, kFirm)).reject, RejectReason::PriceCollar);
    EXPECT_TRUE(submit(order(4, Side::Buy, 104, 10, kFirm)).accepted());
}

}  // namespace
}  // namespace xc
