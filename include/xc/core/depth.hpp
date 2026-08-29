#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "xc/core/types.hpp"

namespace xc {

/// The best bid and offer.
///
/// This is the message a venue publishes most often by a wide margin, so it is
/// a flat aggregate with no indirection and no allocation.
struct TopOfBook {
    InstrumentId instrument;
    SeqNum sequence = 0;

    Price bid_price = kNoPrice;
    Price ask_price = kNoPrice;
    Quantity bid_quantity = 0;
    Quantity ask_quantity = 0;

    constexpr bool has_bid() const noexcept { return bid_price != kNoPrice; }
    constexpr bool has_ask() const noexcept { return ask_price != kNoPrice; }

    /// Distance between the two sides, in ticks. Absent while either side is
    /// empty, because a spread against nothing is not zero -- it is unknown,
    /// and returning zero would make an empty book look like the tightest
    /// market on the venue.
    constexpr std::optional<Price> spread() const noexcept {
        if (!has_bid() || !has_ask()) {
            return std::nullopt;
        }
        return ask_price - bid_price;
    }

    /// Twice the mid price, in ticks.
    ///
    /// Doubled on purpose. With an odd spread the true mid falls between two
    /// ticks, and integer division would silently round it -- half a tick of
    /// error injected into every downstream calculation that consumes it.
    /// Returning the doubled value keeps it exact and makes the caller decide
    /// how to round.
    constexpr std::optional<Price> mid_price_x2() const noexcept {
        if (!has_bid() || !has_ask()) {
            return std::nullopt;
        }
        return bid_price + ask_price;
    }
};

/// One aggregated price level.
struct DepthLevel {
    Price price = kNoPrice;
    Quantity quantity = 0;
    std::uint32_t order_count = 0;

    friend constexpr bool operator==(const DepthLevel&, const DepthLevel&) noexcept = default;
};

/// Aggregated depth for one instrument, best price first on each side.
///
/// Designed to be filled repeatedly into the same object. `clear()` empties the
/// level vectors without releasing their storage, so a publisher that snapshots
/// on every book change allocates during its first few messages and never
/// again.
struct DepthSnapshot {
    InstrumentId instrument;

    /// Set by the caller. The book does not own the engine's sequence, and a
    /// snapshot with no position in the command stream cannot be used to detect
    /// a gap on the wire.
    SeqNum sequence = 0;

    std::vector<DepthLevel> bids;
    std::vector<DepthLevel> asks;

    void clear() noexcept {
        bids.clear();
        asks.clear();
    }
};

}  // namespace xc
