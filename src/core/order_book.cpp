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
Quantity OrderBook::fillable(const Levels& levels, Crosses crosses, Quantity needed,
                             AccountId account) const {
    const SelfTradePolicy policy = instrument_.self_trade_policy;
    const bool prevention_active = policy != SelfTradePolicy::Allow && account.valid();

    if (!prevention_active) {
        // Nothing can come between the aggressor and the book, so resting size
        // is fill size and one number per level answers the question.
        Quantity available = 0;
        for (const auto& [price, level] : levels) {
            if (!crosses(price)) {
                break;
            }
            available += level.total_quantity();
            if (available >= needed) {
                // Counting past what was asked for wastes a walk over levels
                // that cannot change the answer.
                return available;
            }
        }
        return available;
    }

    // With prevention active, resting size and fill size come apart, so this
    // walks individual orders and simulates what the matching loop would do.
    //
    // Each policy diverts quantity differently, and reading level totals gets
    // all three cases wrong:
    //
    //   - cancel-incoming and cancel-both stop the aggressor dead at its first
    //     own order, so nothing deeper is reachable however large it is;
    //   - cancel-resting pulls the own order aside at no cost, so it should be
    //     skipped but what is behind it still counts;
    //   - decrement-both consumes the aggressor's own quantity against its own
    //     order without printing a trade, so that quantity can never fill.
    //
    // The last case is the one that matters most here. Counting only tradeable
    // liquidity makes a fill-or-kill look satisfiable when part of the order is
    // about to be destroyed rather than filled, and it fills short -- which is
    // precisely the outcome fill-or-kill exists to rule out.
    //
    // The cost of this walk is paid only when prevention is enabled and only by
    // fill-or-kill orders, which are a small fraction of real flow.
    Quantity unallocated = needed;
    Quantity will_fill = 0;

    for (const auto& [price, level] : levels) {
        if (!crosses(price)) {
            break;
        }
        for (OrderHandle handle = level.front(); handle != kNullHandle;
             handle = pool_.node(handle).next) {
            const Order& resting = pool_[handle];

            if (resting.account == account) {
                switch (policy) {
                    case SelfTradePolicy::CancelIncoming:
                    case SelfTradePolicy::CancelBoth:
                        return will_fill;
                    case SelfTradePolicy::CancelResting:
                        continue;
                    case SelfTradePolicy::DecrementBoth:
                        unallocated -= std::min(unallocated, resting.remaining);
                        if (unallocated == 0) {
                            return will_fill;
                        }
                        continue;
                    case SelfTradePolicy::Allow:
                        break;  // Unreachable: prevention_active excludes it.
                }
            }

            const Quantity traded = std::min(unallocated, resting.remaining);
            will_fill += traded;
            unallocated -= traded;
            if (unallocated == 0) {
                return will_fill;
            }
        }
    }
    return will_fill;
}

void OrderBook::drop_resting(PriceLevel& level, OrderHandle handle) {
    // Read the id before releasing: the reference is dead the moment the node
    // goes back on the free list.
    const OrderId id = pool_[handle].id;
    level.unlink(pool_, handle);
    index_.erase(id);
    pool_.release(handle);
}

template<typename Levels, typename Crosses>
void OrderBook::match(Levels& levels, Order& incoming, Crosses crosses, std::vector<Fill>& fills,
                      Quantity& stp_cancelled) {
    const SelfTradePolicy policy = instrument_.self_trade_policy;
    const bool prevention_active = policy != SelfTradePolicy::Allow && incoming.account.valid();

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

            if (prevention_active && resting.account == incoming.account) {
                switch (policy) {
                    case SelfTradePolicy::CancelIncoming:
                        // The aggressor gives way and stops here. Everything
                        // deeper in the book stays untouched, which is the
                        // point: the firm keeps the queue position it paid for.
                        stp_cancelled += incoming.remaining;
                        incoming.remaining = 0;
                        break;

                    case SelfTradePolicy::CancelResting:
                        // The resting order gives way and the aggressor carries
                        // on into whatever was behind it.
                        drop_resting(level, resting_handle);
                        continue;

                    case SelfTradePolicy::CancelBoth:
                        stp_cancelled += incoming.remaining;
                        incoming.remaining = 0;
                        drop_resting(level, resting_handle);
                        break;

                    case SelfTradePolicy::DecrementBoth: {
                        // Both sides shrink by the overlap and no trade prints.
                        // Nothing changed hands, so this must never reach the
                        // tape or a position calculation.
                        const Quantity overlap = std::min(incoming.remaining, resting.remaining);
                        incoming.remaining -= overlap;
                        resting.remaining -= overlap;
                        level.reduce(overlap);
                        stp_cancelled += overlap;
                        if (resting.remaining == 0) {
                            drop_resting(level, resting_handle);
                        }
                        continue;
                    }

                    case SelfTradePolicy::Allow:
                        break;  // Unreachable: prevention_active excludes it.
                }
                break;
            }

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
                drop_resting(level, resting_handle);
            }
        }

        if (level.empty()) {
            levels.erase(level_it);
        }
    }
}

bool OrderBook::would_cross(const Order& order) const {
    // A market order takes liquidity by definition, so it always counts as
    // crossing -- combining it with post-only is a contradiction the caller is
    // told about rather than one that is silently resolved.
    if (order.type == OrderType::Market) {
        return true;
    }
    if (order.is_buy()) {
        const std::optional<Price> ask = best_ask();
        return ask.has_value() && order.price >= *ask;
    }
    const std::optional<Price> bid = best_bid();
    return bid.has_value() && order.price <= *bid;
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
    // A newly submitted order has traded nothing yet.
    order.remaining = order.quantity;
    return insert(order, fills);
}

SubmitResult OrderBook::insert(Order order, std::vector<Fill>& fills) {
    SubmitResult result;
    result.reject = validate(order);
    if (!result.accepted()) {
        return result;
    }

    // What this order is still asking to trade. For a fresh submission that is
    // its whole size; for a re-queued amendment it is the part that has not
    // already been filled. Everything below works from this rather than from
    // `quantity`, which stays as the client's cumulative total so that a later
    // amendment can still be interpreted against it.
    const Quantity requested = order.remaining;

    // Post-only is checked before matching rather than after. A market maker
    // sends these because paying the spread would invert its economics, so the
    // correct response to "this would cross" is to refuse the order, not to
    // trade it and report the fact afterwards.
    if (order.post_only && would_cross(order)) {
        result.reject = RejectReason::PostOnlyWouldCross;
        return result;
    }

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
        const Quantity available = buying ? fillable(asks_, crosses, requested, order.account)
                                          : fillable(bids_, crosses, requested, order.account);
        if (available < requested) {
            result.reject = RejectReason::FillOrKillUnfillable;
            return result;
        }
    }

    if (buying) {
        match(asks_, order, crosses, fills, result.stp_cancelled);
    } else {
        match(bids_, order, crosses, fills, result.stp_cancelled);
    }
    // Quantity removed by self-trade prevention never printed a trade, so it
    // has to come out of the filled figure or a client reconciling fills
    // against its own position would come up short.
    result.filled = requested - order.remaining - result.stp_cancelled;

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

ReplaceResult OrderBook::replace(OrderId id, Price new_price, Quantity new_quantity,
                                 SeqNum new_sequence, Nanos now, std::vector<Fill>& fills) {
    ReplaceResult result;

    const auto it = index_.find(id);
    if (it == index_.end()) {
        result.reject = RejectReason::UnknownOrder;
        return result;
    }

    const OrderHandle handle = it->second;
    const Order previous = pool_[handle];
    result.previous = previous;

    if (!instrument_.is_valid_price(new_price)) {
        result.reject = RejectReason::InvalidPrice;
        return result;
    }
    if (!instrument_.is_valid_quantity(new_quantity)) {
        result.reject = RejectReason::InvalidQuantity;
        return result;
    }

    // An amendment down to at or below what has already traded cannot be
    // honoured as a reduction -- the fills have happened. The order is
    // cancelled instead, which is what the client is really asking for when it
    // says "I no longer want more than this much" and it already has that much.
    if (new_quantity <= previous.filled()) {
        cancel(id);
        result.rested = false;
        result.priority_retained = false;
        return result;
    }

    const bool same_price = new_price == previous.price;
    const bool reducing = new_quantity < previous.quantity;

    if (same_price && reducing) {
        // Retained priority: the order stays exactly where it is in the queue
        // and only its size changes. Nothing is unlinked, so this is the
        // cheapest amendment and the common one.
        PriceLevel& level = previous.is_buy() ? bids_.find(previous.price)->second
                                              : asks_.find(previous.price)->second;
        Order& resting = pool_[handle];
        const Quantity new_remaining = new_quantity - previous.filled();
        assert(resting.remaining >= new_remaining && "reduction increased the resting quantity");
        level.reduce(resting.remaining - new_remaining);
        resting.quantity = new_quantity;
        resting.remaining = new_remaining;

        result.priority_retained = true;
        result.rested = true;
        return result;
    }

    // Everything else is a fresh arrival: the old order comes off the book and
    // the amendment goes back in at the tail of its level, where it can cross
    // and trade like any other incoming order.
    const CancelResult removed = cancel(id);
    assert(removed.accepted() && "indexed order could not be cancelled");
    (void)removed;

    Order amended = previous;
    amended.price = new_price;
    amended.quantity = new_quantity;
    amended.remaining = new_quantity - previous.filled();
    amended.sequence = new_sequence;
    amended.accepted_at = now;

    // insert() rather than submit(): the amendment carries its earlier fills
    // with it, so its remaining quantity must survive rather than being reset
    // to its cumulative total.
    const SubmitResult resubmitted = insert(amended, fills);
    result.reject = resubmitted.reject;
    result.filled = resubmitted.filled;
    result.stp_cancelled = resubmitted.stp_cancelled;
    result.rested = resubmitted.rested;
    result.priority_retained = false;
    return result;
}

TopOfBook OrderBook::top_of_book() const {
    TopOfBook top;
    top.instrument = instrument_.id;
    if (!bids_.empty()) {
        const auto& [price, level] = *bids_.begin();
        top.bid_price = price;
        top.bid_quantity = level.total_quantity();
    }
    if (!asks_.empty()) {
        const auto& [price, level] = *asks_.begin();
        top.ask_price = price;
        top.ask_quantity = level.total_quantity();
    }
    return top;
}

void OrderBook::depth(std::size_t max_levels, DepthSnapshot& out) const {
    out.instrument = instrument_.id;
    out.clear();
    out.bids.reserve(max_levels);
    out.asks.reserve(max_levels);

    // Both maps are already ordered best-price-first, so this is a truncated
    // in-order walk. Level totals are maintained incrementally as orders rest
    // and fill, so no level's queue is traversed here however deep it is.
    for (const auto& [price, level] : bids_) {
        if (out.bids.size() >= max_levels) {
            break;
        }
        out.bids.push_back(DepthLevel{price, level.total_quantity(), level.order_count()});
    }
    for (const auto& [price, level] : asks_) {
        if (out.asks.size() >= max_levels) {
            break;
        }
        out.asks.push_back(DepthLevel{price, level.total_quantity(), level.order_count()});
    }
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
