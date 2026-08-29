#include <gtest/gtest.h>

#include "xc/risk/kill_switch.hpp"

namespace xc::risk {
namespace {

constexpr AccountId kAccount{5};
constexpr AccountId kOther{9};

TEST(KillSwitch, StartsPermissive) {
    KillSwitch kill;
    EXPECT_FALSE(kill.venue_halted());
    EXPECT_FALSE(kill.account_halted(kAccount));
    EXPECT_FALSE(kill.blocks_new_orders(kAccount));
}

TEST(KillSwitch, HaltingTheVenueBlocksEveryAccount) {
    KillSwitch kill;
    kill.halt_venue(HaltReason::Manual, 1234);

    EXPECT_TRUE(kill.blocks_new_orders(kAccount));
    EXPECT_TRUE(kill.blocks_new_orders(kOther));
    EXPECT_EQ(kill.venue_state().reason, HaltReason::Manual);
    EXPECT_EQ(kill.venue_state().since, 1234);
}

TEST(KillSwitch, HaltingOneAccountLeavesTheRestTrading) {
    KillSwitch kill;
    kill.halt_account(kAccount, HaltReason::RiskBreach, 99);

    EXPECT_TRUE(kill.blocks_new_orders(kAccount));
    EXPECT_FALSE(kill.blocks_new_orders(kOther));
    EXPECT_EQ(kill.account_state(kAccount).reason, HaltReason::RiskBreach);
    EXPECT_EQ(kill.account_state(kOther).reason, HaltReason::None);
}

TEST(KillSwitch, AnAccountStaysBlockedWhileTheVenueIsHaltedEvenIfItResumes) {
    KillSwitch kill;
    kill.halt_venue(HaltReason::Manual, 0);
    kill.halt_account(kAccount, HaltReason::Manual, 0);

    kill.resume_account(kAccount);
    EXPECT_FALSE(kill.account_halted(kAccount));
    EXPECT_TRUE(kill.blocks_new_orders(kAccount)) << "the venue halt still stands";

    kill.resume_venue();
    EXPECT_FALSE(kill.blocks_new_orders(kAccount));
}

TEST(KillSwitch, DoesNotResumeOnItsOwn) {
    KillSwitch kill;
    kill.set_auto_trip_threshold(2);
    kill.record_reject(kAccount, 1);
    kill.record_reject(kAccount, 2);
    ASSERT_TRUE(kill.account_halted(kAccount));

    // Whatever tripped this has not been diagnosed. A control that un-trips
    // itself would flap through the same failure repeatedly.
    for (Nanos t = 3; t < 100; ++t) {
        kill.record_accept(kAccount);
        ASSERT_TRUE(kill.account_halted(kAccount));
    }
    kill.resume_account(kAccount);
    EXPECT_FALSE(kill.account_halted(kAccount));
}

// --- Automatic tripping ----------------------------------------------------

TEST(KillSwitch, DoesNotTripWhenTheThresholdIsDisabled) {
    KillSwitch kill;
    for (Nanos t = 0; t < 1000; ++t) {
        kill.record_reject(kAccount, t);
    }
    EXPECT_FALSE(kill.account_halted(kAccount));
}

TEST(KillSwitch, TripsAfterARunOfRejects) {
    KillSwitch kill;
    kill.set_auto_trip_threshold(3);

    kill.record_reject(kAccount, 1);
    kill.record_reject(kAccount, 2);
    EXPECT_FALSE(kill.account_halted(kAccount));

    kill.record_reject(kAccount, 3);
    EXPECT_TRUE(kill.account_halted(kAccount));
    EXPECT_EQ(kill.account_state(kAccount).reason, HaltReason::RepeatedRejects);
    EXPECT_EQ(kill.account_state(kAccount).since, 3);
}

TEST(KillSwitch, CountsConsecutiveRejectsNotLifetimeRejects) {
    KillSwitch kill;
    kill.set_auto_trip_threshold(3);

    // An account trading normally for hours accumulates the occasional reject:
    // a mistyped price, a cancel that raced a fill. Tripping on a lifetime
    // total would eventually halt every account on the venue for no reason.
    for (int i = 0; i < 100; ++i) {
        kill.record_reject(kAccount, i);
        kill.record_reject(kAccount, i);
        kill.record_accept(kAccount);
    }
    EXPECT_FALSE(kill.account_halted(kAccount));
    EXPECT_EQ(kill.consecutive_rejects(kAccount), 0u);
}

TEST(KillSwitch, TripsIndependentlyPerAccount) {
    KillSwitch kill;
    kill.set_auto_trip_threshold(2);

    kill.record_reject(kAccount, 1);
    kill.record_reject(kOther, 1);
    kill.record_reject(kAccount, 2);

    EXPECT_TRUE(kill.account_halted(kAccount));
    EXPECT_FALSE(kill.account_halted(kOther)) << "one account's failure is not another's";
}

TEST(KillSwitch, ResumingClearsTheRejectRun) {
    KillSwitch kill;
    kill.set_auto_trip_threshold(3);
    kill.record_reject(kAccount, 1);
    kill.record_reject(kAccount, 2);
    kill.record_reject(kAccount, 3);
    ASSERT_TRUE(kill.account_halted(kAccount));

    kill.resume_account(kAccount);
    // Resuming must not leave the account one reject away from tripping
    // straight back into the halt it was just released from.
    EXPECT_EQ(kill.consecutive_rejects(kAccount), 0u);
    kill.record_reject(kAccount, 4);
    EXPECT_FALSE(kill.account_halted(kAccount));
}

TEST(KillSwitch, DoesNotKeepCountingOnceHalted) {
    KillSwitch kill;
    kill.set_auto_trip_threshold(2);
    kill.record_reject(kAccount, 1);
    kill.record_reject(kAccount, 2);
    ASSERT_TRUE(kill.account_halted(kAccount));
    const Nanos tripped_at = kill.account_state(kAccount).since;

    for (Nanos t = 3; t < 50; ++t) {
        kill.record_reject(kAccount, t);
    }
    // The halt records when it tripped, not when it was last poked.
    EXPECT_EQ(kill.account_state(kAccount).since, tripped_at);
}

}  // namespace
}  // namespace xc::risk
