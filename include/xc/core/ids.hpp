#pragma once

#include <cstdint>
#include <functional>

namespace xc {

/// A 64-bit identifier that cannot be confused with an identifier of another
/// kind.
///
/// Order, account, trade and instrument identifiers are all unsigned 64-bit
/// integers, and in a matching engine they are passed to each other's
/// functions constantly: cancel takes an order id, risk lookups take an
/// account id, fills carry both. As bare integers every one of those call sites
/// compiles happily with the arguments transposed, and the resulting bug is a
/// silent lookup miss rather than a crash. Tagging the type moves that entire
/// class of mistake to compile time for the cost of an explicit constructor;
/// the wrapper is trivially copyable and compiles to a bare integer.
///
/// Zero is reserved as the invalid sentinel for every id type, so a
/// default-constructed identifier is never mistaken for a real one.
template<typename Tag>
class Id {
  public:
    using value_type = std::uint64_t;

    constexpr Id() noexcept = default;
    constexpr explicit Id(value_type value) noexcept : value_(value) {}

    constexpr value_type value() const noexcept { return value_; }
    constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(Id, Id) noexcept = default;
    friend constexpr auto operator<=>(Id, Id) noexcept = default;

  private:
    value_type value_ = 0;
};

/// Identifies an order within the venue. Assigned by the submitting client and
/// required to be unique per account.
using OrderId = Id<struct OrderIdTag>;

/// Identifies the trading account an order belongs to. Risk limits, position
/// tracking and self-trade prevention are all keyed on this.
using AccountId = Id<struct AccountIdTag>;

/// Assigned by the engine to each fill, monotonically increasing per instrument.
using TradeId = Id<struct TradeIdTag>;

/// Identifies a tradable instrument. Resolved from a symbol string once at
/// configuration time so the matching path never touches a string.
using InstrumentId = Id<struct InstrumentIdTag>;

}  // namespace xc

namespace std {

/// Enables use as an unordered_map key. Hashing the underlying integer is
/// sufficient: identifiers are dense and assigned sequentially, which is the
/// case libstdc++ and libc++ bucket well.
template<typename Tag>
struct hash<xc::Id<Tag>> {
    size_t operator()(xc::Id<Tag> id) const noexcept {
        return hash<typename xc::Id<Tag>::value_type>{}(id.value());
    }
};

}  // namespace std
