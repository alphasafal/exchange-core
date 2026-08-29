#include <gtest/gtest.h>

#include <random>
#include <unordered_map>
#include <vector>

#include "xc/core/ids.hpp"
#include "xc/util/flat_hash_map.hpp"

namespace xc {
namespace {

using Map = FlatHashMap<OrderId, std::uint32_t>;

TEST(FlatHashMap, StartsEmpty) {
    const Map map;
    EXPECT_EQ(map.size(), 0u);
    EXPECT_TRUE(map.empty());
    EXPECT_EQ(map.find(OrderId{1}), nullptr);
    EXPECT_FALSE(map.contains(OrderId{1}));
}

TEST(FlatHashMap, InsertsAndFinds) {
    Map map;
    EXPECT_TRUE(map.insert(OrderId{1}, 10));
    EXPECT_TRUE(map.insert(OrderId{2}, 20));

    ASSERT_NE(map.find(OrderId{1}), nullptr);
    EXPECT_EQ(*map.find(OrderId{1}), 10u);
    EXPECT_EQ(*map.find(OrderId{2}), 20u);
    EXPECT_EQ(map.find(OrderId{3}), nullptr);
    EXPECT_EQ(map.size(), 2u);
}

TEST(FlatHashMap, RefusesADuplicateKey) {
    Map map;
    EXPECT_TRUE(map.insert(OrderId{1}, 10));
    EXPECT_FALSE(map.insert(OrderId{1}, 99));
    EXPECT_EQ(*map.find(OrderId{1}), 10u) << "the original value must survive";
    EXPECT_EQ(map.size(), 1u);
}

TEST(FlatHashMap, ErasesAndReports) {
    Map map;
    map.insert(OrderId{1}, 10);
    EXPECT_TRUE(map.erase(OrderId{1}));
    EXPECT_FALSE(map.erase(OrderId{1})) << "erasing twice is not an error, but is not a success";
    EXPECT_EQ(map.find(OrderId{1}), nullptr);
    EXPECT_EQ(map.size(), 0u);
}

TEST(FlatHashMap, KeepsCollidedKeysReachableAfterAnErase) {
    // The failure mode open addressing has and chaining does not: emptying a
    // slot rather than tombstoning it truncates the probe sequence of every key
    // that collided past it, and those keys become invisible while still
    // occupying the table.
    Map map(8);
    std::vector<OrderId> inserted;
    for (std::uint64_t i = 1; i <= 5; ++i) {
        inserted.push_back(OrderId{i});
        ASSERT_TRUE(map.insert(OrderId{i}, static_cast<std::uint32_t>(i)));
    }

    for (const OrderId victim : inserted) {
        Map probe = map;
        ASSERT_TRUE(probe.erase(victim));
        for (const OrderId survivor : inserted) {
            if (survivor == victim) {
                EXPECT_EQ(probe.find(survivor), nullptr);
            } else {
                EXPECT_NE(probe.find(survivor), nullptr)
                    << "key " << survivor.value() << " vanished when " << victim.value()
                    << " was erased";
            }
        }
    }
}

TEST(FlatHashMap, GrowsAndKeepsEverything) {
    Map map(8);
    for (std::uint64_t i = 1; i <= 10'000; ++i) {
        ASSERT_TRUE(map.insert(OrderId{i}, static_cast<std::uint32_t>(i * 3)));
    }
    EXPECT_EQ(map.size(), 10'000u);
    EXPECT_GT(map.rehash_count(), 0u);

    for (std::uint64_t i = 1; i <= 10'000; ++i) {
        ASSERT_NE(map.find(OrderId{i}), nullptr) << "lost key " << i;
        EXPECT_EQ(*map.find(OrderId{i}), static_cast<std::uint32_t>(i * 3));
    }
}

TEST(FlatHashMap, DoesNotRehashWithinItsReservation) {
    Map map(4096);
    for (std::uint64_t i = 1; i <= 2000; ++i) {
        map.insert(OrderId{i}, 1);
    }
    // The property the order book depends on: a sized map performs no
    // allocation at all while it stays inside its reservation.
    EXPECT_EQ(map.rehash_count(), 0u);
}

TEST(FlatHashMap, SurvivesHeavyChurnWithoutDegrading) {
    Map map(1024);
    // Insert and erase far more keys than the table holds at once. Tombstones
    // accumulate on every erase, and without clearing them the table would
    // slowly fill with dead slots and every probe would walk them.
    for (std::uint64_t i = 1; i <= 200'000; ++i) {
        ASSERT_TRUE(map.insert(OrderId{i}, static_cast<std::uint32_t>(i)));
        ASSERT_TRUE(map.erase(OrderId{i}));
    }
    EXPECT_EQ(map.size(), 0u);
    EXPECT_LT(map.capacity(), 100'000u) << "churn must not grow the table without bound";

    map.insert(OrderId{999'999}, 7);
    ASSERT_NE(map.find(OrderId{999'999}), nullptr);
    EXPECT_EQ(*map.find(OrderId{999'999}), 7u);
}

TEST(FlatHashMap, AgreesWithTheStandardMapOnRandomOperations) {
    std::mt19937_64 rng(42);
    Map actual(64);
    std::unordered_map<std::uint64_t, std::uint32_t> expected;

    for (int step = 0; step < 100'000; ++step) {
        const std::uint64_t key = rng() % 5000 + 1;
        const auto value = static_cast<std::uint32_t>(rng());

        if (rng() % 3 == 0) {
            EXPECT_EQ(actual.erase(OrderId{key}), expected.erase(key) == 1) << "step " << step;
        } else {
            const bool inserted = expected.emplace(key, value).second;
            EXPECT_EQ(actual.insert(OrderId{key}, value), inserted) << "step " << step;
        }

        ASSERT_EQ(actual.size(), expected.size()) << "step " << step;
    }

    for (const auto& [key, value] : expected) {
        ASSERT_NE(actual.find(OrderId{key}), nullptr) << "missing key " << key;
        EXPECT_EQ(*actual.find(OrderId{key}), value);
    }
}

TEST(FlatHashMap, ClearsWithoutLosingCapacity) {
    Map map(256);
    for (std::uint64_t i = 1; i <= 100; ++i) {
        map.insert(OrderId{i}, 1);
    }
    const std::size_t capacity = map.capacity();
    map.clear();
    EXPECT_EQ(map.size(), 0u);
    EXPECT_EQ(map.capacity(), capacity);
    EXPECT_EQ(map.find(OrderId{1}), nullptr);
}

TEST(FlatHashMap, HandlesSequentialKeysWithoutClustering) {
    // Order ids are assigned sequentially and hash to themselves, so masking
    // them straight into a power-of-two table would place them in consecutive
    // slots. The bit mixer exists to stop that.
    Map map(8192);
    for (std::uint64_t i = 1; i <= 4000; ++i) {
        map.insert(OrderId{i}, static_cast<std::uint32_t>(i));
    }
    for (std::uint64_t i = 1; i <= 2000; ++i) {
        map.erase(OrderId{i});
    }
    for (std::uint64_t i = 2001; i <= 4000; ++i) {
        ASSERT_NE(map.find(OrderId{i}), nullptr) << "lost key " << i;
    }
}

}  // namespace
}  // namespace xc
