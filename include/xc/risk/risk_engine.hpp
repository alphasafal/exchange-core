#pragma once

#include <cstdint>
#include <unordered_map>

#include "xc/core/commands.hpp"
#include "xc/core/fill.hpp"
#include "xc/core/instrument.hpp"
#include "xc/core/order.hpp"
#include "xc/risk/exposure.hpp"
#include "xc/risk/limits.hpp"

namespace xc::risk {

/// Enforces pre-trade limits and tracks what each account has at stake.
///
/// Sits between the gateway and the book: every new order passes through
/// `check()` before the engine will match it, and the engine reports back what
/// actually happened so exposure stays accurate.
///
/// Accuracy here depends entirely on the engine telling it everything. An
/// account's working quantity is incremented when an order rests and decremented
/// when it fills, cancels or expires, and a missed callback leaks exposure that
/// is never released -- the account slowly loses the ability to trade with no
/// visible cause. The engine integration is written so that every path which
/// removes quantity from the book reports it.
///
/// Not thread-safe, and lives on the matching thread with the book.
class RiskEngine {
  public:
    /// Applies limits to an account. An account with no configured limits is
    /// unconstrained, which is deliberate: a venue must be able to run this
    /// component before it has finished onboarding every account, and silently
    /// rejecting an unconfigured account's orders would be a worse failure than
    /// not checking them.
    void configure(AccountId account, const AccountLimits& limits);

    const AccountLimits* limits_for(AccountId account) const;

    /// Applies venue-level controls to an instrument.
    void configure_instrument(InstrumentId instrument, const InstrumentControls& controls);

    /// Updates the price the collar is measured against. The engine calls this
    /// with the last traded price, which is the most defensible reference a
    /// venue has: it is a price at which two willing parties actually dealt,
    /// rather than one either of them merely hoped for.
    void set_reference_price(InstrumentId instrument, Price price);

    Price reference_price(InstrumentId instrument) const;

    /// Decides whether an order may reach the book.
    ///
    /// **Must be called exactly once per inbound message**, because it consumes
    /// the account's rate-limit budget. That budget is spent whether or not the
    /// order is admitted: a message that arrives and is rejected still cost the
    /// matching thread the work of rejecting it, and an account that could send
    /// unlimited invalid orders would defeat the limit entirely.
    ///
    /// Checks run cheapest-first, and the rate limit runs before everything
    /// else so that a flood is turned away before any of the more expensive
    /// work happens.
    RejectReason check(const NewOrder& command, const Instrument& instrument, Nanos now);

    /// The order rested. Its unfilled quantity is now a commitment.
    void on_order_rested(const Order& order);

    /// Quantity left the book without trading -- a cancellation, an expiry, or
    /// quantity withdrawn by self-trade prevention.
    void on_working_released(AccountId account, InstrumentId instrument, Side side,
                             Quantity quantity);

    /// A trade happened. Moves quantity from commitment to position on both
    /// sides at once, since a fill always has two.
    void on_fill(const Fill& fill);

    /// An order was accepted and did not rest -- it filled outright, or was an
    /// immediate-or-cancel whose remainder expired. Counted so that open order
    /// totals stay right.
    void on_order_closed(AccountId account);

    Exposure exposure(AccountId account, InstrumentId instrument) const;
    std::uint32_t open_orders(AccountId account) const;

  private:
    struct AccountState {
        AccountLimits limits;
        std::uint32_t open_orders = 0;
        std::unordered_map<InstrumentId, Exposure> exposure;

        /// Theoretical arrival time for the rate limiter. See the note on
        /// consume_rate_budget().
        Nanos theoretical_arrival = 0;
    };

    struct InstrumentState {
        InstrumentControls controls;
        Price reference_price = kNoPrice;
    };

    /// True when the account may send one more message now.
    bool consume_rate_budget(AccountState& state, Nanos now) const;

    RejectReason check_collar(const NewOrder& command) const;

    const AccountState* find(AccountId account) const;
    AccountState& state_for(AccountId account);

    std::unordered_map<AccountId, AccountState> accounts_;
    std::unordered_map<InstrumentId, InstrumentState> instruments_;
};

}  // namespace xc::risk
