#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace xc {

/// An open-addressed hash map that allocates only when it grows.
///
/// The order book needs to find an order by id on every cancel and every
/// amendment, which makes this the hottest lookup in the engine. std::
/// unordered_map is specified as a chain of buckets holding separately
/// allocated nodes, so it allocates once per inserted element -- reserve()
/// sizes the bucket array, not the nodes -- and that showed up directly as one
/// allocation per resting order in the allocation tests.
///
/// This stores keys and values in two flat arrays with linear probing. A
/// reserved map therefore performs no allocation at all while its size stays
/// within the reservation, and a lookup touches contiguous memory instead of
/// chasing a pointer per probe.
///
/// Deletion uses tombstones, which is the standard cost of open addressing: a
/// slot cannot simply be emptied, because doing so would truncate the probe
/// sequence of any key that had collided past it and make that key
/// unreachable. Tombstones are cleared by rehashing once they accumulate, so a
/// long-running venue that cancels continuously does not degrade into scanning
/// the whole table.
template<typename Key, typename Value>
class FlatHashMap {
  public:
    explicit FlatHashMap(std::size_t minimum_capacity = 64) {
        resize(round_up_power_of_two(minimum_capacity < 8 ? 8 : minimum_capacity));
    }

    /// Pointer to the mapped value, or nullptr. Invalidated by any insertion
    /// that grows the table.
    Value* find(const Key& key) {
        const std::size_t slot = locate(key);
        return state_[slot] == kOccupied ? &values_[slot] : nullptr;
    }

    const Value* find(const Key& key) const {
        const std::size_t slot = locate(key);
        return state_[slot] == kOccupied ? &values_[slot] : nullptr;
    }

    bool contains(const Key& key) const { return find(key) != nullptr; }

    /// Inserts, or returns false when the key is already present.
    bool insert(const Key& key, const Value& value) {
        if (occupied_ + tombstones_ + 1 > capacity_ * 7 / 10) {
            // Growth is driven by occupied *plus* tombstones, because a table
            // full of tombstones probes just as slowly as a full one even
            // though it holds nothing.
            grow();
        }

        std::size_t slot = index_for(key);
        std::size_t first_tombstone = kNoSlot;
        while (true) {
            if (state_[slot] == kEmpty) {
                // Reuses the earliest tombstone seen on the way, so repeated
                // insert-and-erase cycles do not push every key further from
                // its home slot.
                const std::size_t target = first_tombstone == kNoSlot ? slot : first_tombstone;
                if (first_tombstone != kNoSlot) {
                    --tombstones_;
                }
                keys_[target] = key;
                values_[target] = value;
                state_[target] = kOccupied;
                ++occupied_;
                return true;
            }
            if (state_[slot] == kTombstone) {
                if (first_tombstone == kNoSlot) {
                    first_tombstone = slot;
                }
            } else if (keys_[slot] == key) {
                return false;
            }
            slot = (slot + 1) & mask_;
        }
    }

    bool erase(const Key& key) {
        const std::size_t slot = locate(key);
        if (state_[slot] != kOccupied) {
            return false;
        }
        // Marked, not emptied: emptying would truncate the probe sequence of
        // any key that collided past this slot and make it unreachable.
        state_[slot] = kTombstone;
        --occupied_;
        ++tombstones_;
        return true;
    }

    void clear() {
        state_.assign(capacity_, kEmpty);
        occupied_ = 0;
        tombstones_ = 0;
    }

    std::size_t size() const noexcept { return occupied_; }
    bool empty() const noexcept { return occupied_ == 0; }
    std::size_t capacity() const noexcept { return capacity_; }

    /// How many times the table has been rebuilt. Zero across a workload that
    /// stays within its reservation, which the allocation tests assert.
    std::uint64_t rehash_count() const noexcept { return rehashes_; }

  private:
    static constexpr std::uint8_t kEmpty = 0;
    static constexpr std::uint8_t kOccupied = 1;
    static constexpr std::uint8_t kTombstone = 2;
    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);

    static std::size_t round_up_power_of_two(std::size_t value) {
        std::size_t result = 8;
        while (result < value) {
            result <<= 1;
        }
        return result;
    }

    std::size_t index_for(const Key& key) const {
        // The stored hash is passed through a bit mixer before use. Order ids
        // are assigned sequentially and their hash is the identity, so masking
        // them straight into a power-of-two table would place consecutive ids
        // in consecutive slots -- which is fine until a range is erased and the
        // resulting tombstone run makes every probe walk it.
        std::uint64_t hash = static_cast<std::uint64_t>(std::hash<Key>{}(key));
        hash ^= hash >> 30;
        hash *= 0xBF58476D1CE4E5B9ULL;
        hash ^= hash >> 27;
        hash *= 0x94D049BB133111EBULL;
        hash ^= hash >> 31;
        return static_cast<std::size_t>(hash) & mask_;
    }

    std::size_t locate(const Key& key) const {
        std::size_t slot = index_for(key);
        while (true) {
            if (state_[slot] == kEmpty) {
                return slot;  // Absent: the probe sequence ends here.
            }
            if (state_[slot] == kOccupied && keys_[slot] == key) {
                return slot;
            }
            slot = (slot + 1) & mask_;
        }
    }

    void resize(std::size_t capacity) {
        capacity_ = capacity;
        mask_ = capacity_ - 1;
        keys_.assign(capacity_, Key{});
        values_.assign(capacity_, Value{});
        state_.assign(capacity_, kEmpty);
        occupied_ = 0;
        tombstones_ = 0;
    }

    void grow() {
        std::vector<Key> old_keys = std::move(keys_);
        std::vector<Value> old_values = std::move(values_);
        std::vector<std::uint8_t> old_state = std::move(state_);

        // Doubling only when live entries actually warrant it: a table that is
        // mostly tombstones is rebuilt at the same size, which clears them
        // without growing memory that is not being used.
        const std::size_t live = occupied_ + 1;
        std::size_t capacity = capacity_;
        while (live > capacity * 7 / 10) {
            capacity <<= 1;
        }

        resize(capacity);
        ++rehashes_;

        for (std::size_t i = 0; i < old_state.size(); ++i) {
            if (old_state[i] == kOccupied) {
                insert(old_keys[i], old_values[i]);
            }
        }
    }

    std::vector<Key> keys_;
    std::vector<Value> values_;
    std::vector<std::uint8_t> state_;
    std::size_t capacity_ = 0;
    std::size_t mask_ = 0;
    std::size_t occupied_ = 0;
    std::size_t tombstones_ = 0;
    std::uint64_t rehashes_ = 0;
};

}  // namespace xc
