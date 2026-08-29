#include "xc/core/order_pool.hpp"

#include <cassert>
#include <limits>

namespace xc {

OrderPool::OrderPool(std::size_t initial_capacity) {
    nodes_.reserve(initial_capacity);
}

OrderHandle OrderPool::acquire(const Order& order) {
    OrderHandle handle;
    if (free_head_ != kNullHandle) {
        // Reuse: the common path once the venue reaches steady state.
        handle = free_head_;
        free_head_ = nodes_[free_head_].next;
        nodes_[handle].order = order;
        nodes_[handle].prev = kNullHandle;
        nodes_[handle].next = kNullHandle;
    } else {
        const std::size_t index = nodes_.size();
        // Handles are 32-bit, so the slab cannot address more than 2^32-2
        // orders. Four billion simultaneously live orders is far outside what
        // this engine is built for, but silently truncating the index would
        // corrupt the book, so it is an assertion rather than a comment.
        assert(index < static_cast<std::size_t>(kNullHandle) &&
               "order slab exceeded the range of a 32-bit handle");

        const bool will_grow = nodes_.size() == nodes_.capacity();
        nodes_.push_back(OrderNode{order, kNullHandle, kNullHandle});
        if (will_grow) {
            ++growth_events_;
        }
        handle = static_cast<OrderHandle>(index);
    }

    ++live_;
    if (live_ > high_water_mark_) {
        high_water_mark_ = live_;
    }
    return handle;
}

void OrderPool::release(OrderHandle handle) {
    assert(handle < nodes_.size() && "released a handle the slab never issued");
    assert(live_ > 0 && "released more orders than were acquired");

    // The freed node's `next` doubles as the free-list link. `prev` is cleared
    // so that a stale handle used after release trips the book's own
    // consistency checks rather than quietly walking a live level.
    nodes_[handle].prev = kNullHandle;
    nodes_[handle].next = free_head_;
    free_head_ = handle;
    --live_;
}

}  // namespace xc
