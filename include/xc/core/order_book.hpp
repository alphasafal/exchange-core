#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "xc/core/depth.hpp"
#include "xc/core/fill.hpp"
#include "xc/core/instrument.hpp"
#include "xc/core/order.hpp"
#include "xc/core/order_pool.hpp"
#include "xc/core/price_level.hpp"

namespace xc {

/// Outcome of submitting an order.
struct SubmitResult {
    RejectReason reject = RejectReason::None;

    /// Quantity that traded on arrival.
    Quantity filled = 0;

    /// Quantity of the incoming order removed by self-trade prevention without
    /// printing a trade. Reported separately because a client that sees only a
    /// short fill cannot otherwise distinguish "the book ran out of liquidity"
    /// from "my own resting order was in the way", and those call for
    /// completely different responses.
    Quantity stp_cancelled = 0;

    /// True when a remainder was left resting on the book.
    bool rested = false;

    constexpr bool accepted() const noexcept { return reject == RejectReason::None; }
};

/// Outcome of cancelling an order.
struct CancelResult {
    RejectReason reject = RejectReason::None;

    /// The order as it stood immediately before cancellation. Only meaningful
    /// when `reject` is None. Returned rather than discarded so the caller can
    /// report the unfilled remainder and release risk exposure without a
    /// second lookup.
    Order order;

    constexpr bool accepted() const noexcept { return reject == RejectReason::None; }
};

/// Outcome of amending a resting order.
struct ReplaceResult {
    RejectReason reject = RejectReason::None;

    /// True when the amendment kept the order's place in its queue. False when
    /// it was re-queued behind everything already resting at its price.
    bool priority_retained = false;

    /// The order as it stood before the amendment.
    Order previous;

    /// Set when a re-queued amendment crossed and traded on its way back in.
    Quantity filled = 0;
    Quantity stp_cancelled = 0;

    /// True when a remainder is resting after the amendment. False when the
    /// amendment consumed or cancelled the order outright.
    bool rested = false;

    constexpr bool accepted() const noexcept { return reject == RejectReason::None; }
};

/// A price-time priority limit order book for a single instrument.
///
/// Orders are ranked first by price and then, within a price, by the order in
/// which the engine accepted them. Both are exact: prices are integer ticks, so
/// two equal prices always compare equal, and arrival order is the engine's
/// sequence number rather than a timestamp, so no two orders can tie.
///
/// Complexity, and why it is shaped this way:
///
///   - best bid / best ask   O(1)        `begin()` of an ordered map
///   - insert resting order  O(log L)    L = distinct price levels
///   - cancel                O(1)        hash lookup plus an intrusive unlink
///   - match one fill        O(1)        front of a level's FIFO queue
///
/// The cancel path gets the strongest guarantee on purpose. In real order flow
/// most orders are cancelled rather than filled, so cancellation, not matching,
/// is the operation that runs most often.
///
/// This class is not thread-safe and is not meant to be. The engine drives it
/// from a single thread so that its behaviour is a pure function of the command
/// sequence, which is what makes deterministic replay possible.
class OrderBook {
  public:
    explicit OrderBook(Instrument instrument, std::size_t initial_capacity = 4096);

    /// Matches `order` against the book and rests any remainder that its time
    /// in force permits. Fills are appended to `fills`; existing contents are
    /// left alone so a caller can accumulate across several commands.
    SubmitResult submit(Order order, std::vector<Fill>& fills);

    /// Removes a resting order. Rejects with UnknownOrder if it is not live.
    CancelResult cancel(OrderId id);

    /// Amends a resting order's price or quantity.
    ///
    /// `new_quantity` is the order's new *total* size, counted from original
    /// submission, which is how venues express an amendment: a client that has
    /// been filled for 30 of 100 and amends to 50 is asking to trade 20 more,
    /// not 50 more.
    ///
    /// Whether the order keeps its place in the queue follows the rule real
    /// venues use, and the reasoning is about fairness rather than convenience.
    /// Reducing size at the same price only ever gives up queue position that
    /// was already earned, so priority is retained. Raising size or moving the
    /// price asks for a better position than the order paid for, and the
    /// amendment goes to the back of its level. Without that rule an order
    /// could hold the front of the queue indefinitely with a single lot and
    /// inflate itself the instant it was about to be filled.
    ///
    /// A re-queued amendment is a genuinely new arrival, so it can cross and
    /// trade immediately, and it needs the sequence number the engine would
    /// have given a fresh order -- priority is defined by that sequence, not by
    /// a timestamp.
    ReplaceResult replace(OrderId id, Price new_price, Quantity new_quantity, SeqNum new_sequence,
                          Nanos now, std::vector<Fill>& fills);

    /// The best bid and offer in one read. Cheaper than calling best_bid() and
    /// best_ask() separately and, more importantly, guaranteed to describe a
    /// single consistent instant.
    TopOfBook top_of_book() const;

    /// Fills `out` with up to `max_levels` aggregated levels per side, best
    /// price first.
    ///
    /// Takes its destination by reference rather than returning one so that a
    /// publisher can reuse the same buffers on every book change. `out` is
    /// cleared without releasing its capacity, so after a short warmup this
    /// path performs no allocation at all.
    void depth(std::size_t max_levels, DepthSnapshot& out) const;

    /// Best price on each side, or nullopt when that side is empty.
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    /// Resting quantity at one price. Zero when nothing rests there.
    Quantity quantity_at(Side side, Price price) const;

    /// The live order, or nullptr. Returned as a pointer rather than a
    /// reference so absence is representable; invalidated by any mutation.
    const Order* find(OrderId id) const;

    /// Visits every resting order in matching order: best price first, and
    /// within a price, the order that would fill first. Bids are visited before
    /// asks.
    ///
    /// Defined in the header because it is a template, and exposed because
    /// several things outside the book need its exact contents in exactly this
    /// order -- the differential tests, the state hash that proves replay is
    /// deterministic, and the recovery snapshot.
    template<typename Visitor>
    void for_each_resting_order(Visitor&& visit) const {
        for (const auto& [price, level] : bids_) {
            for (OrderHandle h = level.front(); h != kNullHandle; h = pool_.node(h).next) {
                visit(pool_[h]);
            }
        }
        for (const auto& [price, level] : asks_) {
            for (OrderHandle h = level.front(); h != kNullHandle; h = pool_.node(h).next) {
                visit(pool_[h]);
            }
        }
    }

    std::size_t resting_order_count() const noexcept { return index_.size(); }
    std::size_t bid_level_count() const noexcept { return bids_.size(); }
    std::size_t ask_level_count() const noexcept { return asks_.size(); }

    const Instrument& instrument() const noexcept { return instrument_; }
    const OrderPool& pool() const noexcept { return pool_; }

    /// Total quantity resting on one side. Walks every level, so it is a
    /// diagnostic and test helper rather than a hot-path query.
    Quantity total_quantity(Side side) const;

  private:
    /// Bids descend and asks ascend, so `begin()` is the best price on either
    /// side and the matching loop is identical for both.
    using BidLevels = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskLevels = std::map<Price, PriceLevel, std::less<Price>>;

    template<typename Levels, typename Crosses>
    void match(Levels& levels, Order& incoming, Crosses crosses, std::vector<Fill>& fills,
               Quantity& stp_cancelled);

    /// Quantity an aggressor from `account` could actually trade at prices it
    /// is willing to pay, counting no further than `needed`. Used to decide a
    /// fill-or-kill before anything has been mutated.
    template<typename Levels, typename Crosses>
    Quantity fillable(const Levels& levels, Crosses crosses, Quantity needed,
                      AccountId account) const;

    /// Unlinks a resting order from `level`, drops its index entry and returns
    /// its slab node. Shared by matching and self-trade prevention.
    void drop_resting(PriceLevel& level, OrderHandle handle);

    /// True when this order would take liquidity on arrival.
    bool would_cross(const Order& order) const;

    /// Matches and rests an order whose `remaining` is already set to what it
    /// is asking to trade. Shared by submission and by re-queued amendments,
    /// which arrive having already been partially filled.
    SubmitResult insert(Order order, std::vector<Fill>& fills);

    void rest(Order& order);
    RejectReason validate(const Order& order) const;

    Instrument instrument_;
    OrderPool pool_;
    BidLevels bids_;
    AskLevels asks_;

    /// Order id to slab handle. This is what turns a cancel into an O(1)
    /// unlink instead of a search through the book.
    std::unordered_map<OrderId, OrderHandle> index_;

    std::uint64_t next_trade_id_ = 1;
};

}  // namespace xc
