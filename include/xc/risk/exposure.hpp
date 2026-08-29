#pragma once

#include <cstdint>

#include "xc/core/types.hpp"

namespace xc::risk {

/// What one account currently has at stake in one instrument.
struct Exposure {
    /// Signed net position in lots: positive long, negative short.
    std::int64_t net_position = 0;

    /// Unfilled quantity resting on each side. Tracked separately from the net
    /// position because an order that has not filled yet is not a position, but
    /// it is a commitment -- and a limit that ignores it can be walked past one
    /// resting order at a time.
    Quantity working_buy = 0;
    Quantity working_sell = 0;

    /// Largest long position this account could reach if every resting buy
    /// filled and nothing else happened.
    std::int64_t projected_long() const noexcept {
        return net_position + static_cast<std::int64_t>(working_buy);
    }

    /// Largest short position, expressed as a positive magnitude.
    std::int64_t projected_short() const noexcept {
        return static_cast<std::int64_t>(working_sell) - net_position;
    }
};

}  // namespace xc::risk
