#pragma once

#include <cstddef>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

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

    /// Best price on each side, or nullopt when that side is empty.
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    /// Resting quantity at one price. Zero when nothing rests there.
    Quantity quantity_at(Side side, Price price) const;

    /// The live order, or nullptr. Returned as a pointer rather than a
    /// reference so absence is representable; invalidated by any mutation.
    const Order* find(OrderId id) const;

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
    void match(Levels& levels, Order& incoming, Crosses crosses, std::vector<Fill>& fills);

    /// Quantity available to an aggressor at prices it is willing to trade at,
    /// counting no further than `needed`. Used to decide a fill-or-kill before
    /// anything has been mutated.
    template<typename Levels, typename Crosses>
    Quantity fillable(const Levels& levels, Crosses crosses, Quantity needed) const;

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
