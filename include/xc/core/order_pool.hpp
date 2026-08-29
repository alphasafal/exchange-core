#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "xc/core/order.hpp"

namespace xc {

/// Index of an order inside an OrderPool.
///
/// The book links orders by index rather than by pointer. Indices survive the
/// slab growing, where raw pointers into it would dangle, and they are half the
/// width of a pointer, so a price level's links cost less cache footprint per
/// resting order.
using OrderHandle = std::uint32_t;

/// The handle that refers to no order. Chosen as the maximum representable
/// value so that a valid handle is any index below the slab's size.
inline constexpr OrderHandle kNullHandle = static_cast<OrderHandle>(-1);

/// An order plus its links within one price level's FIFO queue.
struct OrderNode {
    Order order;
    OrderHandle prev = kNullHandle;
    OrderHandle next = kNullHandle;
};

/// Contiguous slab storage for live orders, with a free list for reuse.
///
/// A matching engine allocates and frees an order on nearly every message it
/// handles. Routing that through the general-purpose allocator puts a lock and
/// a size-class lookup on the hot path and scatters orders across the heap,
/// which is exactly the wrong layout for walking a price level. Instead the
/// slab is reserved once at startup and released nodes are pushed onto a free
/// list, so in steady state acquiring an order is a pop and a copy with no
/// allocation at all.
///
/// The slab does grow if the reservation is exhausted, because refusing orders
/// because of an internal capacity choice would be a worse failure than a
/// single reallocation. Growth is counted in `growth_events()` so a benchmark
/// can prove it reserved enough and stayed at zero.
///
/// **Handles stay valid across growth; references do not.** `operator[]`
/// returns a reference into the slab's current storage, and growth moves that
/// storage. Never hold a reference across a call to `acquire()`.
class OrderPool {
  public:
    explicit OrderPool(std::size_t initial_capacity = 1024);

    /// Copies `order` into the slab and returns its handle.
    OrderHandle acquire(const Order& order);

    /// Returns a node to the free list. The handle must not be used again.
    void release(OrderHandle handle);

    Order& operator[](OrderHandle handle) noexcept { return nodes_[handle].order; }
    const Order& operator[](OrderHandle handle) const noexcept { return nodes_[handle].order; }

    OrderNode& node(OrderHandle handle) noexcept { return nodes_[handle]; }
    const OrderNode& node(OrderHandle handle) const noexcept { return nodes_[handle]; }

    /// Number of orders currently live.
    std::size_t live() const noexcept { return live_; }

    /// Nodes the slab can hold without growing.
    std::size_t capacity() const noexcept { return nodes_.capacity(); }

    /// Largest number of simultaneously live orders seen. Used to size the
    /// initial reservation from a real workload rather than from a guess.
    std::size_t high_water_mark() const noexcept { return high_water_mark_; }

    /// How many times the slab had to grow. Expected to be zero in a benchmark
    /// that reserved correctly, and asserted as such by the allocation tests.
    std::size_t growth_events() const noexcept { return growth_events_; }

  private:
    std::vector<OrderNode> nodes_;
    OrderHandle free_head_ = kNullHandle;
    std::size_t live_ = 0;
    std::size_t high_water_mark_ = 0;
    std::size_t growth_events_ = 0;
};

}  // namespace xc
