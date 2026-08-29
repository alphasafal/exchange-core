#pragma once

#include <string>

#include "xc/core/types.hpp"

namespace xc {

/// Static configuration for one tradable instrument.
///
/// Resolved once at startup. The matching path holds an InstrumentId and never
/// looks at the symbol string, so no hot-path operation touches heap-allocated
/// text or performs a string comparison.
struct Instrument {
    InstrumentId id;
    std::string symbol;

    /// Smallest permitted price increment, in the same integer tick units as
    /// Price. A tick_size above one lets an instrument quote in coarser
    /// increments than its internal representation, which is how real venues
    /// widen the grid on illiquid names.
    Price tick_size = 1;

    /// Smallest permitted quantity increment.
    Quantity lot_size = 1;

    /// Smallest order the venue accepts.
    Quantity min_quantity = 1;

    /// Number of decimal places implied by one tick, used only for display.
    /// A price of 15025 with display_exponent 2 renders as 150.25.
    std::uint8_t display_exponent = 2;

    SelfTradePolicy self_trade_policy = SelfTradePolicy::CancelIncoming;

    /// How many resting orders this instrument's book is sized for.
    ///
    /// Not a limit: the book grows past it rather than refusing orders, because
    /// rejecting a client over an internal sizing choice would be a worse
    /// failure than one reallocation. But growth is the only thing that
    /// allocates on the matching path, so a venue sets this from the high-water
    /// mark it has actually measured -- OrderPool reports both that and the
    /// number of times it had to grow.
    std::size_t expected_resting_orders = 4096;

    /// True when `price` sits on this instrument's tick grid and is positive.
    /// Prices off the grid are rejected rather than rounded: silently moving a
    /// customer's price is a worse failure than refusing it.
    constexpr bool is_valid_price(Price price) const noexcept {
        return price > 0 && price % tick_size == 0;
    }

    /// True when `quantity` is a positive whole number of lots and at least the
    /// venue minimum.
    constexpr bool is_valid_quantity(Quantity quantity) const noexcept {
        return quantity >= min_quantity && quantity % lot_size == 0;
    }
};

}  // namespace xc
