#pragma once

#include <cstdint>
#include <string_view>

#include "xc/core/ids.hpp"

namespace xc {

/// A price, expressed as a whole number of ticks.
///
/// Prices are never floating point anywhere in this engine. Binary floating
/// point cannot represent most decimal prices exactly, so a book keyed on
/// doubles can hold two "equal" prices that do not compare equal, split a
/// price level in half, and break time priority in a way that is close to
/// impossible to reproduce. Working in integer ticks makes price equality
/// exact and price arithmetic associative.
///
/// The mapping from ticks to a displayed decimal price belongs to the
/// instrument, not to this type -- see Instrument::tick_size. Signed rather
/// than unsigned because spreads, price deltas and collar offsets are all
/// naturally negative half the time, and unsigned arithmetic turns those into
/// enormous positive numbers.
using Price = std::int64_t;

/// A quantity, expressed as a whole number of lots.
using Quantity = std::uint64_t;

/// Nanoseconds. Interpreted against whichever Clock the engine was given, which
/// during replay is driven from the journal rather than from the wall clock.
using Nanos = std::int64_t;

/// Monotonic position in the engine's total ordering of accepted commands.
/// Every journal record and every outbound market data message carries one.
using SeqNum = std::uint64_t;

/// Sentinel for "no price". Distinguishable from any real tick because a real
/// price is always strictly positive.
inline constexpr Price kNoPrice = 0;

enum class Side : std::uint8_t {
    Buy = 0,
    Sell = 1,
};

constexpr Side opposite(Side side) noexcept {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

enum class OrderType : std::uint8_t {
    /// Rests on the book at its limit price; never trades through it.
    Limit = 0,
    /// Takes whatever liquidity exists at any price; never rests.
    Market = 1,
};

enum class TimeInForce : std::uint8_t {
    /// Rests until cancelled or until the session ends.
    Day = 0,
    /// Trades whatever it can immediately; the remainder is cancelled.
    ImmediateOrCancel = 1,
    /// Trades in full immediately or not at all.
    FillOrKill = 2,
    /// Rests until explicitly cancelled.
    GoodTilCancelled = 3,
};

/// Behaviour when an incoming order would trade against a resting order from
/// the same account.
///
/// Venues offer these as a configurable policy rather than a single rule
/// because the right answer depends on why the crossing happened: a firm with
/// several desks may want its resting quote preserved, while a firm running one
/// strategy usually wants the aggressor dropped.
enum class SelfTradePolicy : std::uint8_t {
    /// Permit self-trades. Present so the reference model and the optimised
    /// book can be compared with the feature switched off.
    Allow = 0,
    /// Cancel the remainder of the incoming order.
    CancelIncoming = 1,
    /// Cancel the resting order and continue matching the aggressor.
    CancelResting = 2,
    /// Cancel both sides of the potential match.
    CancelBoth = 3,
    /// Reduce both orders by the overlapping quantity without printing a trade.
    DecrementBoth = 4,
};

/// Why the engine refused a command.
///
/// Kept as a closed enumeration rather than a string so that rejects can be
/// counted, compared in differential tests, and encoded on the wire in one byte.
enum class RejectReason : std::uint8_t {
    None = 0,
    UnknownInstrument,
    UnknownOrder,
    DuplicateOrderId,
    InvalidPrice,
    InvalidQuantity,
    InvalidSide,
    /// A fill-or-kill order that the book could not fill completely.
    FillOrKillUnfillable,
    /// A post-only order that would have taken liquidity.
    PostOnlyWouldCross,
    /// Blocked by self-trade prevention.
    SelfTrade,
    /// Rejected by a pre-trade risk limit.
    RiskLimit,
    /// The account or the venue is halted by the kill switch.
    Halted,
    /// The account exceeded its permitted message rate.
    RateLimit,
};

constexpr std::string_view to_string(Side side) noexcept {
    return side == Side::Buy ? "Buy" : "Sell";
}

constexpr std::string_view to_string(OrderType type) noexcept {
    return type == OrderType::Limit ? "Limit" : "Market";
}

constexpr std::string_view to_string(TimeInForce tif) noexcept {
    switch (tif) {
        case TimeInForce::Day:
            return "Day";
        case TimeInForce::ImmediateOrCancel:
            return "IOC";
        case TimeInForce::FillOrKill:
            return "FOK";
        case TimeInForce::GoodTilCancelled:
            return "GTC";
    }
    return "Unknown";
}

constexpr std::string_view to_string(SelfTradePolicy policy) noexcept {
    switch (policy) {
        case SelfTradePolicy::Allow:
            return "Allow";
        case SelfTradePolicy::CancelIncoming:
            return "CancelIncoming";
        case SelfTradePolicy::CancelResting:
            return "CancelResting";
        case SelfTradePolicy::CancelBoth:
            return "CancelBoth";
        case SelfTradePolicy::DecrementBoth:
            return "DecrementBoth";
    }
    return "Unknown";
}

constexpr std::string_view to_string(RejectReason reason) noexcept {
    switch (reason) {
        case RejectReason::None:
            return "None";
        case RejectReason::UnknownInstrument:
            return "UnknownInstrument";
        case RejectReason::UnknownOrder:
            return "UnknownOrder";
        case RejectReason::DuplicateOrderId:
            return "DuplicateOrderId";
        case RejectReason::InvalidPrice:
            return "InvalidPrice";
        case RejectReason::InvalidQuantity:
            return "InvalidQuantity";
        case RejectReason::InvalidSide:
            return "InvalidSide";
        case RejectReason::FillOrKillUnfillable:
            return "FillOrKillUnfillable";
        case RejectReason::PostOnlyWouldCross:
            return "PostOnlyWouldCross";
        case RejectReason::SelfTrade:
            return "SelfTrade";
        case RejectReason::RiskLimit:
            return "RiskLimit";
        case RejectReason::Halted:
            return "Halted";
        case RejectReason::RateLimit:
            return "RateLimit";
    }
    return "Unknown";
}

}  // namespace xc
