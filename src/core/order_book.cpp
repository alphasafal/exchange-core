#include "xc/core/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <utility>

namespace xc {

OrderBook::OrderBook(Instrument instrument, std::size_t initial_capacity)
    : instrument_(std::move(instrument)), pool_(initial_capacity) {
    index_.reserve(initial_capacity);
}

RejectReason OrderBook::validate(const Order& order) const {
    if (order.instrument != instrument_.id) {
        return RejectReason::UnknownInstrument;
    }
    if (!instrument_.is_valid_quantity(order.quantity)) {
        return RejectReason::InvalidQuantity;
    }
    if (order.type == OrderType::Limit && !instrument_.is_valid_price(order.price)) {
        return RejectReason::InvalidPrice;
    }
    // A duplicate id would overwrite the index entry for a live order, leaving
    // the original resting on a level with nothing pointing at it -- it could
    // never be cancelled and would only leave the book by being filled.
    if (index_.contains(order.id)) {
        return RejectReason::DuplicateOrderId;
    }
    return RejectReason::None;
}

template<typename Levels, typename Crosses>
Quantity OrderBook::fillable(const Levels& levels, Crosses crosses, Quantity needed) const {
    Quantity available = 0;
    for (const auto& [price, level] : levels) {
        if (!crosses(price)) {
            break;
        }
        available += level.total_quantity();
        if (available >= needed) {
            // Counting past what was asked for wastes a walk over levels that
            // cannot change the answer.
            return available;
        }
    }
    return available;
}

template<typename Levels, typename Crosses>
void OrderBook::match(Levels& levels, Order& incoming, Crosses crosses, std::vector<Fill>& fills) {
    while (incoming.remaining > 0 && !levels.empty()) {
        auto level_it = levels.begin();
        const Price level_price = level_it->first;
        if (!crosses(level_price)) {
            break;  // Nothing deeper in the book can cross either.
        }

        PriceLevel& level = level_it->second;
        while (incoming.remaining > 0 && !level.empty()) {
            const OrderHandle resting_handle = level.front();
            Order& resting = pool_[resting_handle];

            const Quantity quantity = std::min(incoming.remaining, resting.remaining);
            incoming.remaining -= quantity;
            resting.remaining -= quantity;
            level.reduce(quantity);

            const bool resting_filled = resting.remaining == 0;
            fills.push_back(Fill{
                .id = TradeId{next_trade_id_++},
                .instrument = instrument_.id,
                .price = level_price,
                .quantity = quantity,
                .aggressor_order = incoming.id,
                .resting_order = resting.id,
                .aggressor_account = incoming.account,
                .resting_account = resting.account,
                .aggressor_side = incoming.side,
                .resting_filled = resting_filled,
                .timestamp = incoming.accepted_at,
            });

            if (resting_filled) {
                // Read the id before releasing: the reference is dead the
                // moment the node goes back on the free list.
                const OrderId resting_id = resting.id;
                level.unlink(pool_, resting_handle);
                index_.erase(resting_id);
                pool_.release(resting_handle);
            }
        }

        if (level.empty()) {
            levels.erase(level_it);
        }
    }
}

void OrderBook::rest(Order& order) {
    const OrderHandle handle = pool_.acquire(order);
    index_.emplace(order.id, handle);
    if (order.is_buy()) {
        bids_[order.price].push_back(pool_, handle);
    } else {
        asks_[order.price].push_back(pool_, handle);
    }
}

SubmitResult OrderBook::submit(Order order, std::vector<Fill>& fills) {
    SubmitResult result;
    result.reject = validate(order);
    if (!result.accepted()) {
        return result;
    }

    order.remaining = order.quantity;

    // A market order crosses at any price; a limit order crosses only up to
    // its own. Expressing that as a predicate keeps one matching loop for both
    // sides and both order types rather than four near-identical copies.
    const bool is_market = order.type == OrderType::Market;
    const Price limit = order.price;
    const bool buying = order.is_buy();

    const auto crosses = [is_market, limit, buying](Price level_price) {
        if (is_market) {
            return true;
        }
        return buying ? limit >= level_price : limit <= level_price;
    };

    // Fill-or-kill is decided before anything is mutated.
    //
    // The tempting implementation matches greedily and then abandons the trades
    // if the order turns out not to fill completely. That does not undo the
    // matching: resting orders have already been decremented and fully filled
    // ones already removed. The book is left inconsistent while the caller is
    // told the order did nothing. Measuring available liquidity first costs one
    // extra walk over the crossing levels and makes the failure path a genuine
    // no-op.
    if (order.tif == TimeInForce::FillOrKill) {
        const Quantity available = buying ? fillable(asks_, crosses, order.quantity)
                                          : fillable(bids_, crosses, order.quantity);
        if (available < order.quantity) {
            result.reject = RejectReason::FillOrKillUnfillable;
            return result;
        }
    }

    if (buying) {
        match(asks_, order, crosses, fills);
    } else {
        match(bids_, order, crosses, fills);
    }
    result.filled = order.quantity - order.remaining;

    if (order.remaining == 0) {
        return result;
    }

    assert(order.tif != TimeInForce::FillOrKill &&
           "fill-or-kill was cleared as fillable but did not fill");

    // A market order never rests: it has no price to rest at, and holding one
    // on the book would mean an order that trades through any future quote.
    // An immediate-or-cancel order is cancelled rather than rested by
    // definition. In both cases the remainder simply expires.
    if (is_market || order.tif == TimeInForce::ImmediateOrCancel) {
        return result;
    }

    rest(order);
    result.rested = true;
    return result;
}

CancelResult OrderBook::cancel(OrderId id) {
    CancelResult result;

    const auto it = index_.find(id);
    if (it == index_.end()) {
        result.reject = RejectReason::UnknownOrder;
        return result;
    }

    const OrderHandle handle = it->second;
    result.order = pool_[handle];

    // Unlinking needs the level the order rests on, which is fully determined
    // by its own side and price -- there is no search.
    if (result.order.is_buy()) {
        const auto level_it = bids_.find(result.order.price);
        assert(level_it != bids_.end() && "indexed order has no price level");
        level_it->second.unlink(pool_, handle);
        if (level_it->second.empty()) {
            bids_.erase(level_it);
        }
    } else {
        const auto level_it = asks_.find(result.order.price);
        assert(level_it != asks_.end() && "indexed order has no price level");
        level_it->second.unlink(pool_, handle);
        if (level_it->second.empty()) {
            asks_.erase(level_it);
        }
    }

    index_.erase(it);
    pool_.release(handle);
    return result;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    if (side == Side::Buy) {
        const auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_quantity();
    }
    const auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_quantity();
}

const Order* OrderBook::find(OrderId id) const {
    const auto it = index_.find(id);
    return it == index_.end() ? nullptr : &pool_[it->second];
}

Quantity OrderBook::total_quantity(Side side) const {
    Quantity total = 0;
    if (side == Side::Buy) {
        for (const auto& [price, level] : bids_) {
            total += level.total_quantity();
        }
    } else {
        for (const auto& [price, level] : asks_) {
            total += level.total_quantity();
        }
    }
    return total;
}

}  // namespace xc
