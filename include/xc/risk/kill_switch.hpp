#pragma once

#include <cstdint>
#include <string_view>
#include <unordered_map>

#include "xc/core/types.hpp"

namespace xc::risk {

/// Why trading was stopped.
enum class HaltReason : std::uint8_t {
    None = 0,
    /// An operator pulled it.
    Manual,
    /// Tripped automatically after repeated rejected orders.
    RepeatedRejects,
    /// Tripped automatically after a risk limit was breached.
    RiskBreach,
};

constexpr std::string_view to_string(HaltReason reason) noexcept {
    switch (reason) {
        case HaltReason::None:
            return "None";
        case HaltReason::Manual:
            return "Manual";
        case HaltReason::RepeatedRejects:
            return "RepeatedRejects";
        case HaltReason::RiskBreach:
            return "RiskBreach";
    }
    return "Unknown";
}

struct HaltState {
    bool halted = false;
    HaltReason reason = HaltReason::None;
    /// When the halt was applied, from the engine's clock.
    Nanos since = 0;
};

/// Stops trading, for one account or for the whole venue.
///
/// **A halt blocks new orders. It never blocks cancellation.**
///
/// That asymmetry is the single most important property here, and getting it
/// backwards turns a safety control into the thing that causes the loss. An
/// account is halted precisely when something has gone wrong, which is exactly
/// when it most needs to withdraw the orders it already has resting. A switch
/// that froze the account's existing quotes would leave them exposed to the
/// market with no way to pull them -- the operator would have stopped the
/// account from reducing its risk while leaving every position it already had
/// live. Cancellation is always permitted, and halting with cancel-on-trip
/// actively pulls the account's resting orders rather than stranding them.
///
/// Halts are sticky. Nothing resumes on its own, including an automatic trip:
/// whatever tripped it has not been diagnosed yet, and a control that
/// un-trips itself would flap through the same failure repeatedly.
class KillSwitch {
  public:
    void halt_venue(HaltReason reason, Nanos now);
    void resume_venue();
    bool venue_halted() const noexcept { return venue_.halted; }
    const HaltState& venue_state() const noexcept { return venue_; }

    void halt_account(AccountId account, HaltReason reason, Nanos now);
    void resume_account(AccountId account);
    bool account_halted(AccountId account) const;
    HaltState account_state(AccountId account) const;

    /// True when this account may not open new orders, for any reason.
    bool blocks_new_orders(AccountId account) const;

    /// Consecutive rejected orders after which an account is halted
    /// automatically. Zero disables the trip.
    ///
    /// Counted consecutively rather than cumulatively on purpose. An account
    /// that has been trading normally for hours will have accumulated rejects
    /// along the way -- a mistyped price, a race between a cancel and a fill --
    /// and tripping on a lifetime total would eventually halt every account on
    /// the venue for no reason. A run of rejects with no successes between them
    /// is the signal that something is actually looping.
    void set_auto_trip_threshold(std::uint32_t consecutive_rejects) noexcept {
        auto_trip_threshold_ = consecutive_rejects;
    }

    std::uint32_t auto_trip_threshold() const noexcept { return auto_trip_threshold_; }

    /// Reports a rejected order. May trip the account's halt.
    void record_reject(AccountId account, Nanos now);

    /// Reports an accepted order, clearing the account's reject run.
    void record_accept(AccountId account);

    std::uint32_t consecutive_rejects(AccountId account) const;

  private:
    struct AccountHalt {
        HaltState state;
        std::uint32_t consecutive_rejects = 0;
    };

    HaltState venue_;
    std::unordered_map<AccountId, AccountHalt> accounts_;
    std::uint32_t auto_trip_threshold_ = 0;
};

}  // namespace xc::risk
