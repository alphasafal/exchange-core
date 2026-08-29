#include "xc/core/price_level.hpp"

#include <cassert>

namespace xc {

void PriceLevel::push_back(OrderPool& pool, OrderHandle handle) {
    OrderNode& node = pool.node(handle);
    node.next = kNullHandle;
    node.prev = tail_;

    if (tail_ == kNullHandle) {
        head_ = handle;
    } else {
        pool.node(tail_).next = handle;
    }
    tail_ = handle;

    total_quantity_ += node.order.remaining;
    ++order_count_;
}

void PriceLevel::unlink(OrderPool& pool, OrderHandle handle) {
    assert(order_count_ > 0 && "unlinked an order from an empty level");

    OrderNode& node = pool.node(handle);
    if (node.prev != kNullHandle) {
        pool.node(node.prev).next = node.next;
    } else {
        assert(head_ == handle && "order claims to be first but the level disagrees");
        head_ = node.next;
    }

    if (node.next != kNullHandle) {
        pool.node(node.next).prev = node.prev;
    } else {
        assert(tail_ == handle && "order claims to be last but the level disagrees");
        tail_ = node.prev;
    }

    node.prev = kNullHandle;
    node.next = kNullHandle;

    assert(total_quantity_ >= node.order.remaining && "level total fell below a resting order");
    total_quantity_ -= node.order.remaining;
    --order_count_;
}

void PriceLevel::reduce(Quantity quantity) noexcept {
    assert(total_quantity_ >= quantity && "reduced a level below zero");
    total_quantity_ -= quantity;
}

}  // namespace xc
