#pragma once

#include <cstdint>
#include <limits>

#include "xc/core/types.hpp"

namespace xc::risk {

/// Pre-trade limits for one account.
///
/// Every limit defaults to "unlimited" so that adding a new one to this struct
/// cannot silently start rejecting an existing account's orders. An operator
/// opts into each constraint deliberately.
///
/// These are *pre-trade* checks: they run before an order reaches the book and
/// reject it outright. That ordering is the whole point. A limit enforced after
/// matching is not a limit, it is a report -- the trade has happened, the
/// position exists, and the only remaining question is how to unwind it.
struct AccountLimits {
    /// Largest single order, in lots. The first line of defence against a
    /// mistyped quantity.
    Quantity max_order_quantity = std::numeric_limits<Quantity>::max();

    /// Largest single order by notional value, in ticks times lots.
    ///
    /// Kept alongside the quantity limit rather than instead of it because the
    /// two catch different mistakes: a quantity limit alone lets an enormous
    /// order through on a high-priced instrument, and a notional limit alone
    /// lets a huge quantity through on a cheap one.
    std::uint64_t max_order_notional = std::numeric_limits<std::uint64_t>::max();

    /// Most orders the account may have resting at once, across all
    /// instruments. Bounds the damage a runaway quoting loop can do, and bounds
    /// the memory one account can occupy in the book.
    std::uint32_t max_open_orders = std::numeric_limits<std::uint32_t>::max();

    /// Largest absolute net position per instrument, in lots.
    ///
    /// Checked against the position the account would hold if everything it
    /// currently has working were to fill, not against the position it holds
    /// now. An account flat with a thousand lots of resting bids is one fill
    /// away from being long a thousand; treating it as flat would let it build
    /// an unlimited position one resting order at a time.
    Quantity max_position = std::numeric_limits<Quantity>::max();

    /// Messages the account may send per second, averaged. Zero disables the
    /// limit.
    ///
    /// Rate limiting is the one control that protects the venue rather than the
    /// account. A client stuck in a quote-cancel loop can saturate the matching
    /// thread and add latency to everyone else's orders, and it will do so
    /// without ever breaching a position or notional limit.
    std::uint32_t max_messages_per_second = 0;

    /// How many messages may arrive back to back before the average rate binds.
    ///
    /// Real order flow is bursty: a market maker requoting a whole ladder after
    /// a price move sends a dozen messages in a microsecond and then nothing
    /// for a second. A limiter with no burst tolerance rejects exactly the
    /// legitimate behaviour it should permit.
    std::uint32_t message_burst = 1;
};

/// Venue-level controls for one instrument.
///
/// These protect the market rather than any single account, which is why they
/// live here rather than in AccountLimits: a fat-fingered price hurts everyone
/// resting on the book, not only the account that sent it.
struct InstrumentControls {
    /// Furthest an order may be priced from the reference price, in basis
    /// points. Zero disables the collar.
    ///
    /// This is the check that stops a misplaced decimal point from sweeping the
    /// book. It is deliberately a rejection rather than a price adjustment:
    /// silently moving a client's price is a worse failure than refusing it.
    std::uint32_t collar_bps = 0;
};

}  // namespace xc::risk
