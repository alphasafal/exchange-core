#pragma once

#include <vector>

#include "xc/core/depth.hpp"
#include "xc/core/instrument.hpp"
#include "xc/core/order_book.hpp"

namespace xc::model {

/// A deliberately naive limit order book, used as an executable specification.
///
/// This is the oracle the optimised OrderBook is tested against. It is written
/// for one property only: that a reader can convince themselves it is correct
/// by reading it, without holding an invariant in their head. Every design
/// choice here trades speed for obviousness.
///
///   - Orders live in one flat vector in arrival order. There is no index, no
///     price map and no free list, so there is no bookkeeping that could drift
///     out of step with the orders themselves.
///   - Finding the order that should trade next is a linear scan over every
///     live order, picking the best price and then the lowest sequence number.
///     That is the definition of price-time priority stated directly, rather
///     than a data structure that maintains it as a side effect.
///   - Fill-or-kill copies the whole book and runs the real matching loop on
///     the copy. Whether the order can fill is answered by finding out, which
///     makes it independent of the optimised book's liquidity accounting rather
///     than a second implementation of the same idea.
///
/// The result is O(N) per fill and O(N**2) over a run. That is fine: this is
/// never on a hot path, and a fast oracle that shares a bug with the code it
/// checks is worth nothing.
class ReferenceBook {
  public:
    explicit ReferenceBook(Instrument instrument) : instrument_(std::move(instrument)) {}

    SubmitResult submit(Order order, std::vector<Fill>& fills);
    CancelResult cancel(OrderId id);
    ReplaceResult replace(OrderId id, Price new_price, Quantity new_quantity, SeqNum new_sequence,
                          Nanos now, std::vector<Fill>& fills);

    TopOfBook top_of_book() const;
    void depth(std::size_t max_levels, DepthSnapshot& out) const;

    /// Every live order, in the order they would be matched: best price first,
    /// then earliest sequence. Bids before asks, matching OrderBook's traversal
    /// so the two can be compared element by element.
    std::vector<Order> resting_orders() const;

  private:
    /// Index into `orders_` of the order an aggressor should trade with next,
    /// or -1 when nothing is eligible.
    std::ptrdiff_t best_counterparty(Side aggressor_side, Price limit, bool is_market) const;

    /// Runs the matching loop. Shared by real submission and by the throwaway
    /// copy that fill-or-kill uses to find out whether it can fill.
    SubmitResult match(Order& incoming, std::vector<Fill>& fills);

    void remove(std::size_t index);
    std::ptrdiff_t find(OrderId id) const;
    bool would_cross(const Order& order) const;

    Instrument instrument_;
    std::vector<Order> orders_;
    std::uint64_t next_trade_id_ = 1;
};

}  // namespace xc::model
