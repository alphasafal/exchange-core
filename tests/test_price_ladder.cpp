#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <random>
#include <set>
#include <vector>

#include "xc/core/order_pool.hpp"
#include "xc/core/price_ladder.hpp"

namespace xc {
namespace {

using Bids = PriceLadder<true>;   // highest price first
using Asks = PriceLadder<false>;  // lowest price first

template<typename Ladder>
std::vector<Price> walk(const Ladder& ladder) {
    std::vector<Price> prices;
    for (const auto& [price, level] : ladder) {
        static_cast<void>(level);
        prices.push_back(price);
    }
    return prices;
}

TEST(PriceLadder, StartsEmpty) {
    Bids bids;
    bids.configure(1, 1, 1024);
    EXPECT_TRUE(bids.empty());
    EXPECT_EQ(bids.size(), 0u);
    EXPECT_EQ(bids.begin(), bids.end());
    EXPECT_EQ(bids.find(500), bids.end());
}

TEST(PriceLadder, MapsPricesToTicks) {
    Asks asks;
    asks.configure(1000, 25, 100);  // 1000, 1025, ... 3475
    EXPECT_TRUE(asks.in_band(1000));
    EXPECT_TRUE(asks.in_band(1025));
    EXPECT_TRUE(asks.in_band(3475));
    EXPECT_FALSE(asks.in_band(3500));  // past the top
    EXPECT_FALSE(asks.in_band(999));   // below the base
    EXPECT_FALSE(asks.in_band(1010));  // off tick
    EXPECT_EQ(asks.price_at(0), 1000);
    EXPECT_EQ(asks.price_at(1), 1025);
}

TEST(PriceLadder, BidsWalkHighestFirstAndAsksLowestFirst) {
    Bids bids;
    Asks asks;
    bids.configure(1, 1, 4096);
    asks.configure(1, 1, 4096);
    for (const Price price : {100, 4000, 7, 2500, 63, 64, 65}) {
        static_cast<void>(bids[price]);
        static_cast<void>(asks[price]);
    }
    EXPECT_EQ(walk(bids), (std::vector<Price>{4000, 2500, 100, 65, 64, 63, 7}));
    EXPECT_EQ(walk(asks), (std::vector<Price>{7, 63, 64, 65, 100, 2500, 4000}));
}

TEST(PriceLadder, FindsAndErasesWithoutDisturbingNeighbours) {
    Asks asks;
    asks.configure(1, 1, 4096);
    for (const Price price : {10, 11, 12}) {
        static_cast<void>(asks[price]);
    }
    ASSERT_EQ(asks.size(), 3u);
    asks.erase(asks.find(11));
    EXPECT_EQ(asks.size(), 2u);
    EXPECT_EQ(asks.find(11), asks.end());
    EXPECT_EQ(walk(asks), (std::vector<Price>{10, 12}));
}

TEST(PriceLadder, ReusedLevelDoesNotRememberTheOldOne) {
    Asks asks;
    asks.configure(1, 1, 64);

    OrderPool pool(16);
    Order order;
    order.id = OrderId{1};
    order.price = 10;
    order.quantity = 5;
    order.remaining = 5;
    asks[10].push_back(pool, pool.acquire(order));
    ASSERT_EQ(asks[10].order_count(), 1u);

    asks.erase(asks.find(10));

    // The slot is reused the next time this price trades. A level that
    // remembered its old head would resurrect orders that no longer exist.
    EXPECT_TRUE(asks[10].empty());
    EXPECT_EQ(asks[10].order_count(), 0u);
    EXPECT_EQ(asks[10].total_quantity(), 0u);
}

/// The indices where a flat bitmap goes wrong: the ends of each 64-bit word and
/// of each summary word, in both directions.
TEST(PriceLadder, HandlesEveryWordBoundary) {
    const std::vector<Price> boundaries = {0,      1,      62,     63,     64,    65,   127,
                                           128,    4094,   4095,   4096,   4097,  8191, 8192,
                                           262142, 262143, 262144, 262145, 299999};
    Bids bids;
    Asks asks;
    bids.configure(0, 1, 300000);
    asks.configure(0, 1, 300000);
    for (const Price price : boundaries) {
        static_cast<void>(bids[price]);
        static_cast<void>(asks[price]);
    }

    std::vector<Price> ascending = boundaries;
    std::sort(ascending.begin(), ascending.end());
    std::vector<Price> descending = ascending;
    std::reverse(descending.begin(), descending.end());

    EXPECT_EQ(walk(asks), ascending);
    EXPECT_EQ(walk(bids), descending);
}

TEST(PriceLadder, ClearEmptiesEverything) {
    Bids bids;
    bids.configure(1, 1, 4096);
    for (Price price = 1; price < 4096; price += 7) {
        static_cast<void>(bids[price]);
    }
    ASSERT_FALSE(bids.empty());
    bids.clear();
    EXPECT_TRUE(bids.empty());
    EXPECT_EQ(bids.size(), 0u);
    EXPECT_EQ(bids.begin(), bids.end());
}

/// The strongest test here: the ladder against the container it replaces.
///
/// A std::map with the same comparator is the specification for ordering, and
/// the two must agree on contents and traversal order after any sequence of
/// inserts and erases. This is the same technique the order book uses against
/// the reference matcher, applied one level down.
template<bool Descending>
void differential_against_map(std::uint64_t seed) {
    constexpr std::size_t kTicks = 70000;  // spans several summary words
    PriceLadder<Descending> ladder;
    ladder.configure(0, 1, kTicks);

    using Cmp = std::conditional_t<Descending, std::greater<Price>, std::less<Price>>;
    std::map<Price, int, Cmp> model;

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<Price> price_of(0, kTicks - 1);

    for (int step = 0; step < 6000; ++step) {
        const Price price = price_of(rng);
        const bool insert = (rng() % 100) < 60 || model.empty();

        if (insert) {
            static_cast<void>(ladder[price]);
            ++model[price];
        } else {
            // Erase something that exists, chosen from the model.
            auto it = model.begin();
            std::advance(it, static_cast<long>(rng() % model.size()));
            const Price victim = it->first;
            ladder.erase(ladder.find(victim));
            model.erase(it);
            ASSERT_EQ(ladder.find(victim), ladder.end()) << "victim " << victim;
        }

        ASSERT_EQ(ladder.size(), model.size()) << "step " << step;
        if (!model.empty()) {
            ASSERT_NE(ladder.begin(), ladder.end()) << "step " << step;
            ASSERT_EQ((*ladder.begin()).first, model.begin()->first)
                << "best price differs at step " << step;
        }
    }

    std::vector<Price> expected;
    expected.reserve(model.size());
    for (const auto& [price, count] : model) {
        static_cast<void>(count);
        expected.push_back(price);
    }
    EXPECT_EQ(walk(ladder), expected);
}

TEST(PriceLadder, BidsAgreeWithAnOrderedMap) {
    for (std::uint64_t seed : {1u, 7u, 42u, 1234u}) {
        differential_against_map<true>(seed);
    }
}

TEST(PriceLadder, AsksAgreeWithAnOrderedMap) {
    for (std::uint64_t seed : {1u, 7u, 42u, 1234u}) {
        differential_against_map<false>(seed);
    }
}

}  // namespace
}  // namespace xc
