#pragma once

#include "xc/core/types.hpp"

namespace xc {

/// A single order as the book holds it.
///
/// Deliberately free of std::string and of any owning pointer: the book stores
/// these by value inside a contiguous slab, so keeping the type trivially
/// copyable means moving one is a memcpy and a level walk stays inside a small
/// number of cache lines.
struct Order {
    OrderId id;
    AccountId account;
    InstrumentId instrument;

    /// Limit price in ticks. Ignored for market orders, which carry kNoPrice.
    Price price = kNoPrice;

    /// Quantity as submitted. Never modified after acceptance, so that a fill
    /// report can always state what fraction of the original order traded.
    Quantity quantity = 0;

    /// Quantity still available to trade. Only this field moves during matching.
    Quantity remaining = 0;

    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::Day;

    /// When the engine accepted the order, from the engine's Clock.
    Nanos accepted_at = 0;

    /// Position in the engine's total ordering of accepted commands.
    ///
    /// This, not accepted_at, is what defines time priority. Two orders can
    /// share a timestamp -- clock resolution is finite and a coarse clock makes
    /// that common -- but no two can share a sequence number, so priority stays
    /// a total order and replay reproduces it exactly.
    SeqNum sequence = 0;

    /// Refuse to take liquidity: if this order would cross on arrival it is
    /// rejected instead. Used by market makers who must not pay the spread.
    bool post_only = false;

    constexpr Quantity filled() const noexcept { return quantity - remaining; }
    constexpr bool is_filled() const noexcept { return remaining == 0; }
    constexpr bool is_buy() const noexcept { return side == Side::Buy; }
};

}  // namespace xc
