#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "xc/core/price_level.hpp"
#include "xc/core/types.hpp"

namespace xc {

/// Price levels in a flat array, indexed by tick, with a hierarchical bitmap
/// over the occupied ones.
///
/// This replaces the `std::map<Price, PriceLevel>` the book used to hold. That
/// map was correct and tidy, and it cost about 5 ns per doubling of the level
/// count -- the measured signature of a red-black tree, with every rest and
/// every cancel walking one more node and missing cache one more time.
/// docs/benchmarks.md has the curve that motivated this.
///
/// Here, finding a level is an array index, and finding the *best* level is
/// three `countr_zero` instructions over a summary bitmap rather than a descent
/// through a tree. Both are O(1) in the number of occupied levels.
///
/// The price paid is generality, and it is a real price. A ladder has to be
/// bounded, so an instrument declares the band it may quote in and prices
/// outside it are rejected rather than stored. That is a constraint the map did
/// not impose, it is recorded in docs/limitations.md, and it is the reason this
/// is a deliberate design change rather than a free optimisation.
///
/// Memory is `sizeof(PriceLevel)` per tick per side, allocated once when the
/// book is constructed and never again. A 65,536-tick band costs about 1.5 MB
/// a side. Widening the band is linear in memory and free in time; the bitmap
/// descent does not care how wide the band is, only how many levels are live.
///
/// `Descending` picks the traversal order: bids are walked highest price first,
/// asks lowest first, so "begin() is the best price" holds for both and the
/// matching loop stays one piece of code.
template<bool Descending>
class PriceLadder {
  public:
    static constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

    PriceLadder() = default;

    /// Sizes the ladder for a band. Allocates everything it will ever use.
    void configure(Price min_price, Price tick_size, std::size_t tick_count) {
        assert(tick_size > 0 && "tick size must be positive");
        assert(tick_count > 0 && "a ladder needs at least one tick");
        min_price_ = min_price;
        tick_ = tick_size;
        levels_.assign(tick_count, PriceLevel{});
        l0_.assign((tick_count + 63) / 64, 0);
        l1_.assign((l0_.size() + 63) / 64, 0);
        l2_.assign((l1_.size() + 63) / 64, 0);
        occupied_ = 0;
    }

    bool empty() const noexcept { return occupied_ == 0; }
    std::size_t size() const noexcept { return occupied_; }

    /// True when `price` lands on a tick inside the band.
    bool in_band(Price price) const noexcept {
        if (price < min_price_) {
            return false;
        }
        const Price offset = price - min_price_;
        if (offset % tick_ != 0) {
            return false;
        }
        return static_cast<std::size_t>(offset / tick_) < levels_.size();
    }

    // --- iteration, best price first ---------------------------------------

    template<bool Const>
    class Iter {
      public:
        using Ladder = std::conditional_t<Const, const PriceLadder, PriceLadder>;
        using Level = std::conditional_t<Const, const PriceLevel, PriceLevel>;

        /// What `*it` yields. A plain struct with `first` and `second` so that
        /// `const auto& [price, level] : ladder` reads exactly as it did when
        /// this was a std::map, and the call sites did not have to change.
        struct Entry {
            Price first;
            Level& second;
        };

        /// `it->second` needs something with a real `operator->`, and Entry is
        /// materialised on dereference rather than stored, so the arrow has to
        /// own a copy for the duration of the expression.
        class Arrow {
          public:
            explicit Arrow(Entry entry) : entry_(entry) {}
            Entry* operator->() { return &entry_; }

          private:
            Entry entry_;
        };

        Iter() = default;
        Iter(Ladder* ladder, std::size_t index) : ladder_(ladder), index_(index) {}

        Entry operator*() const {
            return Entry{ladder_->price_at(index_), ladder_->levels_[index_]};
        }
        Arrow operator->() const { return Arrow{**this}; }

        Iter& operator++() {
            index_ = ladder_->next_occupied(index_);
            return *this;
        }

        bool operator==(const Iter& other) const noexcept { return index_ == other.index_; }
        bool operator!=(const Iter& other) const noexcept { return index_ != other.index_; }

        std::size_t index() const noexcept { return index_; }

      private:
        Ladder* ladder_ = nullptr;
        std::size_t index_ = kNpos;
    };

    using iterator = Iter<false>;
    using const_iterator = Iter<true>;

    iterator begin() noexcept { return iterator{this, best_occupied()}; }
    iterator end() noexcept { return iterator{this, kNpos}; }
    const_iterator begin() const noexcept { return const_iterator{this, best_occupied()}; }
    const_iterator end() const noexcept { return const_iterator{this, kNpos}; }

    // --- lookup and mutation ------------------------------------------------

    /// The level at `price`, marking it occupied. The price must be in band;
    /// the book validates that before any order reaches here.
    PriceLevel& operator[](Price price) {
        const std::size_t index = index_of(price);
        assert(index != kNpos && "price outside the instrument's band reached the ladder");
        if (!test(index)) {
            set(index);
            ++occupied_;
        }
        return levels_[index];
    }

    iterator find(Price price) noexcept {
        const std::size_t index = index_of(price);
        if (index == kNpos || !test(index)) {
            return end();
        }
        return iterator{this, index};
    }

    const_iterator find(Price price) const noexcept {
        const std::size_t index = index_of(price);
        if (index == kNpos || !test(index)) {
            return end();
        }
        return const_iterator{this, index};
    }

    void erase(iterator it) noexcept {
        const std::size_t index = it.index();
        assert(index != kNpos && test(index) && "erasing a level that is not occupied");
        clear_bit(index);
        // Reset rather than leave stale: the slot is reused the next time this
        // price trades, and a level remembering an old head would resurrect
        // orders that no longer exist.
        levels_[index] = PriceLevel{};
        --occupied_;
    }

    void clear() noexcept {
        for (std::size_t i = best_occupied(); i != kNpos; i = next_occupied(i)) {
            levels_[i] = PriceLevel{};
        }
        l0_.assign(l0_.size(), 0);
        l1_.assign(l1_.size(), 0);
        l2_.assign(l2_.size(), 0);
        occupied_ = 0;
    }

    Price price_at(std::size_t index) const noexcept {
        return min_price_ + static_cast<Price>(index) * tick_;
    }

  private:
    static constexpr std::uint64_t kAll = ~std::uint64_t{0};

    /// Bits strictly above `bit` within a word.
    static constexpr std::uint64_t above(unsigned bit) noexcept {
        return bit >= 63 ? 0 : kAll << (bit + 1);
    }
    /// Bits strictly below `bit` within a word.
    static constexpr std::uint64_t below(unsigned bit) noexcept {
        return bit == 0 ? 0 : (std::uint64_t{1} << bit) - 1;
    }
    static unsigned lowest(std::uint64_t word) noexcept {
        return static_cast<unsigned>(std::countr_zero(word));
    }
    static unsigned highest(std::uint64_t word) noexcept {
        return 63U - static_cast<unsigned>(std::countl_zero(word));
    }

    std::size_t index_of(Price price) const noexcept {
        if (price < min_price_) {
            return kNpos;
        }
        const Price offset = price - min_price_;
        if (offset % tick_ != 0) {
            return kNpos;
        }
        const auto index = static_cast<std::size_t>(offset / tick_);
        return index < levels_.size() ? index : kNpos;
    }

    bool test(std::size_t index) const noexcept {
        return (l0_[index >> 6] & (std::uint64_t{1} << (index & 63))) != 0;
    }

    void set(std::size_t index) noexcept {
        l0_[index >> 6] |= std::uint64_t{1} << (index & 63);
        l1_[index >> 12] |= std::uint64_t{1} << ((index >> 6) & 63);
        l2_[index >> 18] |= std::uint64_t{1} << ((index >> 12) & 63);
    }

    /// Clears the bit, and the summary bits above it only once the word they
    /// summarise has actually emptied. Clearing a summary bit while its word
    /// still holds levels would make those levels invisible to every search.
    void clear_bit(std::size_t index) noexcept {
        const std::size_t w0 = index >> 6;
        l0_[w0] &= ~(std::uint64_t{1} << (index & 63));
        if (l0_[w0] != 0) {
            return;
        }
        const std::size_t w1 = index >> 12;
        l1_[w1] &= ~(std::uint64_t{1} << (w0 & 63));
        if (l1_[w1] != 0) {
            return;
        }
        l2_[index >> 18] &= ~(std::uint64_t{1} << (w1 & 63));
    }

    std::size_t from_l1(std::size_t w1) const noexcept {
        const unsigned b1 = Descending ? highest(l1_[w1]) : lowest(l1_[w1]);
        const std::size_t w0 = w1 * 64 + b1;
        const unsigned b0 = Descending ? highest(l0_[w0]) : lowest(l0_[w0]);
        return w0 * 64 + b0;
    }

    std::size_t from_l2(std::size_t w2) const noexcept {
        const unsigned b2 = Descending ? highest(l2_[w2]) : lowest(l2_[w2]);
        return from_l1(w2 * 64 + b2);
    }

    /// The best price's index: highest for bids, lowest for asks.
    std::size_t best_occupied() const noexcept {
        if (occupied_ == 0) {
            return kNpos;
        }
        if constexpr (Descending) {
            for (std::size_t w2 = l2_.size(); w2-- > 0;) {
                if (l2_[w2] != 0) {
                    return from_l2(w2);
                }
            }
        } else {
            for (std::size_t w2 = 0; w2 < l2_.size(); ++w2) {
                if (l2_[w2] != 0) {
                    return from_l2(w2);
                }
            }
        }
        return kNpos;
    }

    /// The next occupied level in traversal order, or kNpos.
    ///
    /// Walks outward one summary level at a time: the rest of the current word,
    /// then the rest of its summary word, then the rest of the top word, then
    /// the remaining top words. A level is never missed because a summary bit
    /// is set whenever anything below it is.
    std::size_t next_occupied(std::size_t index) const noexcept {
        if (index == kNpos) {
            return kNpos;
        }
        const std::size_t w0 = index >> 6;
        const auto b0 = static_cast<unsigned>(index & 63);

        const std::uint64_t rest0 = l0_[w0] & (Descending ? below(b0) : above(b0));
        if (rest0 != 0) {
            return w0 * 64 + (Descending ? highest(rest0) : lowest(rest0));
        }

        const std::size_t w1 = w0 >> 6;
        const auto b1 = static_cast<unsigned>(w0 & 63);
        const std::uint64_t rest1 = l1_[w1] & (Descending ? below(b1) : above(b1));
        if (rest1 != 0) {
            const unsigned pick = Descending ? highest(rest1) : lowest(rest1);
            const std::size_t next0 = w1 * 64 + pick;
            return next0 * 64 + (Descending ? highest(l0_[next0]) : lowest(l0_[next0]));
        }

        const std::size_t w2 = w1 >> 6;
        const auto b2 = static_cast<unsigned>(w1 & 63);
        const std::uint64_t rest2 = l2_[w2] & (Descending ? below(b2) : above(b2));
        if (rest2 != 0) {
            const unsigned pick = Descending ? highest(rest2) : lowest(rest2);
            return from_l1(w2 * 64 + pick);
        }

        if constexpr (Descending) {
            for (std::size_t w = w2; w-- > 0;) {
                if (l2_[w] != 0) {
                    return from_l2(w);
                }
            }
        } else {
            for (std::size_t w = w2 + 1; w < l2_.size(); ++w) {
                if (l2_[w] != 0) {
                    return from_l2(w);
                }
            }
        }
        return kNpos;
    }

    std::vector<PriceLevel> levels_;
    std::vector<std::uint64_t> l0_;  ///< one bit per tick
    std::vector<std::uint64_t> l1_;  ///< one bit per l0 word
    std::vector<std::uint64_t> l2_;  ///< one bit per l1 word
    std::size_t occupied_ = 0;
    Price min_price_ = 0;
    Price tick_ = 1;
};

}  // namespace xc
