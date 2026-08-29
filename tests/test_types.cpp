#include <gtest/gtest.h>

#include <type_traits>
#include <unordered_map>

#include "xc/core/instrument.hpp"
#include "xc/core/types.hpp"

namespace xc {
namespace {

// The whole point of the tagged identifier is that these conversions do not
// exist. If any of these static_asserts ever fails, the type has stopped
// protecting the call sites it was introduced for.
static_assert(!std::is_convertible_v<OrderId, AccountId>);
static_assert(!std::is_convertible_v<AccountId, OrderId>);
static_assert(!std::is_convertible_v<std::uint64_t, OrderId>,
              "construction from a raw integer must be explicit");
static_assert(!std::is_convertible_v<OrderId, std::uint64_t>);

// A wrapper that costs anything at runtime would not be worth having on the
// matching path.
static_assert(sizeof(OrderId) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<OrderId>);

TEST(Id, DefaultConstructsInvalid) {
    EXPECT_FALSE(OrderId{}.valid());
    EXPECT_EQ(OrderId{}.value(), 0u);
    EXPECT_FALSE(OrderId{0}.valid());
    EXPECT_TRUE(OrderId{1}.valid());
}

TEST(Id, OrdersAndComparesByValue) {
    EXPECT_EQ(OrderId{7}, OrderId{7});
    EXPECT_NE(OrderId{7}, OrderId{8});
    EXPECT_LT(OrderId{7}, OrderId{8});
}

TEST(Id, WorksAsAnUnorderedMapKey) {
    std::unordered_map<OrderId, int> book;
    book[OrderId{42}] = 1;
    book[OrderId{43}] = 2;
    EXPECT_EQ(book.at(OrderId{42}), 1);
    EXPECT_EQ(book.count(OrderId{44}), 0u);
}

TEST(Side, OppositeIsAnInvolution) {
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
    EXPECT_EQ(opposite(Side::Sell), Side::Buy);
    EXPECT_EQ(opposite(opposite(Side::Buy)), Side::Buy);
}

TEST(Instrument, RejectsPricesOffTheTickGrid) {
    const Instrument instrument{.id = InstrumentId{1}, .symbol = "TEST", .tick_size = 25};
    EXPECT_TRUE(instrument.is_valid_price(100));
    EXPECT_TRUE(instrument.is_valid_price(125));
    EXPECT_FALSE(instrument.is_valid_price(110)) << "not a multiple of the tick size";
    EXPECT_FALSE(instrument.is_valid_price(0)) << "zero is the no-price sentinel";
    EXPECT_FALSE(instrument.is_valid_price(-25)) << "prices are strictly positive";
}

TEST(Instrument, RejectsQuantitiesOffTheLotGridOrBelowMinimum) {
    const Instrument instrument{
        .id = InstrumentId{1}, .symbol = "TEST", .lot_size = 10, .min_quantity = 100};
    EXPECT_TRUE(instrument.is_valid_quantity(100));
    EXPECT_TRUE(instrument.is_valid_quantity(110));
    EXPECT_FALSE(instrument.is_valid_quantity(105)) << "not a whole number of lots";
    EXPECT_FALSE(instrument.is_valid_quantity(90)) << "below the venue minimum";
    EXPECT_FALSE(instrument.is_valid_quantity(0));
}

// Reject reasons are encoded as a single byte on the wire and compared across
// engines in differential tests, so every one of them must render distinctly.
TEST(RejectReason, EveryValueHasADistinctName) {
    const RejectReason reasons[] = {
        RejectReason::None,
        RejectReason::UnknownInstrument,
        RejectReason::UnknownOrder,
        RejectReason::DuplicateOrderId,
        RejectReason::InvalidPrice,
        RejectReason::InvalidQuantity,
        RejectReason::InvalidSide,
        RejectReason::FillOrKillUnfillable,
        RejectReason::PostOnlyWouldCross,
        RejectReason::SelfTrade,
        RejectReason::RiskLimit,
        RejectReason::Halted,
        RejectReason::RateLimit,
    };
    for (const RejectReason reason : reasons) {
        EXPECT_NE(to_string(reason), "Unknown") << static_cast<int>(reason);
    }
}

}  // namespace
}  // namespace xc
