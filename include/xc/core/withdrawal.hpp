#pragma once

#include "xc/core/types.hpp"

namespace xc::core {}

namespace xc {

/// Quantity taken off the book without trading.
///
/// Self-trade prevention can remove or shrink a *resting* order, and that
/// happens during someone else's command: there is no fill to report it and no
/// cancel request to attribute it to. Without an explicit record the resting
/// account's risk exposure would never be released -- it would slowly lose the
/// ability to trade, with nothing in any log to explain why.
struct Withdrawal {
    OrderId order;
    AccountId account;
    InstrumentId instrument;
    Side side = Side::Buy;

    /// Quantity removed from the book.
    Quantity quantity = 0;

    /// True when the order left the book entirely; false when it was only
    /// reduced and is still resting.
    bool fully_removed = false;

    /// Why. Currently always SelfTrade, but carried explicitly so that a future
    /// cause is not silently attributed to prevention.
    RejectReason reason = RejectReason::SelfTrade;
};

}  // namespace xc
