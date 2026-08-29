#pragma once

#include "xc/core/types.hpp"

namespace xc {

/// One execution between an incoming order and a resting one.
///
/// Both sides are recorded in full. A fill report has to be attributable to two
/// accounts without a second lookup, and the differential tests compare fill
/// streams field by field, which only works if the fill is self-describing.
struct Fill {
    TradeId id;
    InstrumentId instrument;

    /// Always the resting order's price, never the aggressor's.
    ///
    /// The order that was on the book first set the terms, so an aggressor
    /// willing to pay more than the best offer trades at the offer and keeps
    /// the difference. Pricing at the aggressor's limit instead would
    /// systematically overcharge whoever crossed the spread.
    Price price = kNoPrice;
    Quantity quantity = 0;

    OrderId aggressor_order;
    OrderId resting_order;
    AccountId aggressor_account;
    AccountId resting_account;

    /// Side of the order that removed liquidity. This is what a public tape
    /// reports as the trade direction.
    Side aggressor_side = Side::Buy;

    /// True when the resting order was completely consumed by this fill.
    bool resting_filled = false;

    Nanos timestamp = 0;
};

}  // namespace xc
