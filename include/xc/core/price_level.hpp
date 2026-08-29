#pragma once

#include <cstdint>

#include "xc/core/order_pool.hpp"

namespace xc {

/// The FIFO queue of resting orders at one price.
///
/// Time priority within a price level is first-in-first-out, so the structure
/// needs three operations: append at the back when an order rests, read and pop
/// the front while matching, and remove an arbitrary order when it is
/// cancelled. Cancels dominate real order flow -- most orders are cancelled,
/// not filled -- which makes the third operation the one worth optimising.
///
/// A vector or deque of orders makes the cancel a linear scan of the level.
/// This is an intrusive doubly linked list threaded through the order slab, so
/// a cancel that already knows the order's handle unlinks it in constant time
/// regardless of how deep the level is.
///
/// The level holds no storage of its own; nodes live in the OrderPool that is
/// passed to each operation. That keeps the level small enough that the price
/// map storing thousands of them stays cache-friendly.
class PriceLevel {
  public:
    /// Appends an order at the back of the queue, behind everything resting.
    void push_back(OrderPool& pool, OrderHandle handle);

    /// Unlinks an order from anywhere in the queue in constant time. The node
    /// itself is not released; the caller owns that decision.
    void unlink(OrderPool& pool, OrderHandle handle);

    /// Reduces the level's resting total after a partial fill or an amend.
    void reduce(Quantity quantity) noexcept;

    OrderHandle front() const noexcept { return head_; }
    bool empty() const noexcept { return head_ == kNullHandle; }

    /// Sum of `remaining` across every order resting here. Maintained
    /// incrementally so that publishing depth never walks the queue.
    Quantity total_quantity() const noexcept { return total_quantity_; }

    std::uint32_t order_count() const noexcept { return order_count_; }

  private:
    OrderHandle head_ = kNullHandle;
    OrderHandle tail_ = kNullHandle;
    Quantity total_quantity_ = 0;
    std::uint32_t order_count_ = 0;
};

}  // namespace xc
