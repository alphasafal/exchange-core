#include "reference_book.hpp"

#include <algorithm>
#include <map>

namespace xc::model {

std::ptrdiff_t ReferenceBook::find(OrderId id) const {
    for (std::size_t i = 0; i < orders_.size(); ++i) {
        if (orders_[i].id == id) {
            return static_cast<std::ptrdiff_t>(i);
        }
    }
    return -1;
}

void ReferenceBook::remove(std::size_t index) {
    orders_.erase(orders_.begin() + static_cast<std::ptrdiff_t>(index));
}

std::ptrdiff_t ReferenceBook::best_counterparty(Side aggressor_side, Price limit,
                                                bool is_market) const {
    std::ptrdiff_t best = -1;
    for (std::size_t i = 0; i < orders_.size(); ++i) {
        const Order& candidate = orders_[i];
        if (candidate.side == aggressor_side || candidate.remaining == 0) {
            continue;
        }
        if (!is_market) {
            const bool crosses =
                aggressor_side == Side::Buy ? candidate.price <= limit : candidate.price >= limit;
            if (!crosses) {
                continue;
            }
        }

        if (best < 0) {
            best = static_cast<std::ptrdiff_t>(i);
            continue;
        }

        // Price first, then arrival. Stated directly rather than maintained as
        // a property of a data structure.
        const Order& incumbent = orders_[static_cast<std::size_t>(best)];
        const bool better_price = aggressor_side == Side::Buy ? candidate.price < incumbent.price
                                                              : candidate.price > incumbent.price;
        const bool same_price_but_earlier =
            candidate.price == incumbent.price && candidate.sequence < incumbent.sequence;
        if (better_price || same_price_but_earlier) {
            best = static_cast<std::ptrdiff_t>(i);
        }
    }
    return best;
}

SubmitResult ReferenceBook::match(Order& incoming, std::vector<Fill>& fills) {
    SubmitResult result;
    const Quantity requested = incoming.remaining;
    const bool is_market = incoming.type == OrderType::Market;
    const SelfTradePolicy policy = instrument_.self_trade_policy;
    const bool prevention_active = policy != SelfTradePolicy::Allow && incoming.account.valid();

    while (incoming.remaining > 0) {
        const std::ptrdiff_t index = best_counterparty(incoming.side, incoming.price, is_market);
        if (index < 0) {
            break;
        }
        const std::size_t slot = static_cast<std::size_t>(index);
        Order& resting = orders_[slot];

        if (prevention_active && resting.account == incoming.account) {
            if (policy == SelfTradePolicy::CancelIncoming) {
                result.stp_cancelled += incoming.remaining;
                incoming.remaining = 0;
            } else if (policy == SelfTradePolicy::CancelResting) {
                remove(slot);
            } else if (policy == SelfTradePolicy::CancelBoth) {
                result.stp_cancelled += incoming.remaining;
                incoming.remaining = 0;
                remove(slot);
            } else {  // DecrementBoth
                const Quantity overlap = std::min(incoming.remaining, resting.remaining);
                incoming.remaining -= overlap;
                resting.remaining -= overlap;
                result.stp_cancelled += overlap;
                if (resting.remaining == 0) {
                    remove(slot);
                }
            }
            continue;
        }

        const Quantity quantity = std::min(incoming.remaining, resting.remaining);
        incoming.remaining -= quantity;
        resting.remaining -= quantity;

        fills.push_back(Fill{
            .id = TradeId{next_trade_id_++},
            .instrument = instrument_.id,
            .price = resting.price,
            .quantity = quantity,
            .aggressor_order = incoming.id,
            .resting_order = resting.id,
            .aggressor_account = incoming.account,
            .resting_account = resting.account,
            .aggressor_side = incoming.side,
            .resting_filled = resting.remaining == 0,
            .timestamp = incoming.accepted_at,
        });

        if (resting.remaining == 0) {
            remove(slot);
        }
    }

    result.filled = requested - incoming.remaining - result.stp_cancelled;
    return result;
}

bool ReferenceBook::would_cross(const Order& order) const {
    if (order.type == OrderType::Market) {
        return true;
    }
    return best_counterparty(order.side, order.price, false) >= 0;
}

SubmitResult ReferenceBook::submit(Order order, std::vector<Fill>& fills) {
    SubmitResult result;

    if (order.instrument != instrument_.id) {
        result.reject = RejectReason::UnknownInstrument;
        return result;
    }
    if (!instrument_.is_valid_quantity(order.quantity)) {
        result.reject = RejectReason::InvalidQuantity;
        return result;
    }
    if (order.type == OrderType::Limit && !instrument_.is_valid_price(order.price)) {
        result.reject = RejectReason::InvalidPrice;
        return result;
    }
    if (find(order.id) >= 0) {
        result.reject = RejectReason::DuplicateOrderId;
        return result;
    }

    if (order.remaining == 0) {
        order.remaining = order.quantity;
    }

    if (order.post_only && would_cross(order)) {
        result.reject = RejectReason::PostOnlyWouldCross;
        return result;
    }

    // Fill-or-kill by simulation: copy the entire book, run the real matching
    // loop against the copy, and look at what happened. Nothing about the
    // optimised book's liquidity accounting is reused or assumed, so if the two
    // ever disagree the differential test has found something real.
    if (order.tif == TimeInForce::FillOrKill) {
        ReferenceBook trial = *this;
        Order probe = order;
        std::vector<Fill> discarded;
        const SubmitResult simulated = trial.match(probe, discarded);
        if (simulated.filled < order.remaining) {
            result.reject = RejectReason::FillOrKillUnfillable;
            return result;
        }
    }

    result = match(order, fills);

    if (order.remaining == 0) {
        return result;
    }
    if (order.type == OrderType::Market || order.tif == TimeInForce::ImmediateOrCancel) {
        return result;
    }

    orders_.push_back(order);
    result.rested = true;
    return result;
}

CancelResult ReferenceBook::cancel(OrderId id) {
    CancelResult result;
    const std::ptrdiff_t index = find(id);
    if (index < 0) {
        result.reject = RejectReason::UnknownOrder;
        return result;
    }
    result.order = orders_[static_cast<std::size_t>(index)];
    remove(static_cast<std::size_t>(index));
    return result;
}

ReplaceResult ReferenceBook::replace(OrderId id, Price new_price, Quantity new_quantity,
                                     SeqNum new_sequence, Nanos now, std::vector<Fill>& fills) {
    ReplaceResult result;
    const std::ptrdiff_t index = find(id);
    if (index < 0) {
        result.reject = RejectReason::UnknownOrder;
        return result;
    }

    const Order previous = orders_[static_cast<std::size_t>(index)];
    result.previous = previous;

    if (!instrument_.is_valid_price(new_price)) {
        result.reject = RejectReason::InvalidPrice;
        return result;
    }
    if (!instrument_.is_valid_quantity(new_quantity)) {
        result.reject = RejectReason::InvalidQuantity;
        return result;
    }

    if (new_quantity <= previous.filled()) {
        remove(static_cast<std::size_t>(index));
        return result;
    }

    if (new_price == previous.price && new_quantity < previous.quantity) {
        Order& resting = orders_[static_cast<std::size_t>(index)];
        resting.quantity = new_quantity;
        resting.remaining = new_quantity - previous.filled();
        result.priority_retained = true;
        result.rested = true;
        return result;
    }

    remove(static_cast<std::size_t>(index));

    Order amended = previous;
    amended.price = new_price;
    amended.quantity = new_quantity;
    amended.remaining = new_quantity - previous.filled();
    amended.sequence = new_sequence;
    amended.accepted_at = now;

    const SubmitResult resubmitted = submit(amended, fills);
    result.reject = resubmitted.reject;
    result.filled = resubmitted.filled;
    result.stp_cancelled = resubmitted.stp_cancelled;
    result.rested = resubmitted.rested;
    return result;
}

std::vector<Order> ReferenceBook::resting_orders() const {
    std::vector<Order> bids;
    std::vector<Order> asks;
    for (const Order& order : orders_) {
        (order.is_buy() ? bids : asks).push_back(order);
    }

    std::sort(bids.begin(), bids.end(), [](const Order& a, const Order& b) {
        return a.price != b.price ? a.price > b.price : a.sequence < b.sequence;
    });
    std::sort(asks.begin(), asks.end(), [](const Order& a, const Order& b) {
        return a.price != b.price ? a.price < b.price : a.sequence < b.sequence;
    });

    bids.insert(bids.end(), asks.begin(), asks.end());
    return bids;
}

TopOfBook ReferenceBook::top_of_book() const {
    TopOfBook top;
    top.instrument = instrument_.id;
    for (const Order& order : orders_) {
        if (order.is_buy()) {
            if (top.bid_price == kNoPrice || order.price > top.bid_price) {
                top.bid_price = order.price;
                top.bid_quantity = 0;
            }
        } else {
            if (top.ask_price == kNoPrice || order.price < top.ask_price) {
                top.ask_price = order.price;
                top.ask_quantity = 0;
            }
        }
    }
    for (const Order& order : orders_) {
        if (order.is_buy() && order.price == top.bid_price) {
            top.bid_quantity += order.remaining;
        } else if (!order.is_buy() && order.price == top.ask_price) {
            top.ask_quantity += order.remaining;
        }
    }
    return top;
}

void ReferenceBook::depth(std::size_t max_levels, DepthSnapshot& out) const {
    out.instrument = instrument_.id;
    out.clear();

    std::map<Price, DepthLevel, std::greater<Price>> bids;
    std::map<Price, DepthLevel, std::less<Price>> asks;
    for (const Order& order : orders_) {
        DepthLevel& level = order.is_buy() ? bids[order.price] : asks[order.price];
        level.price = order.price;
        level.quantity += order.remaining;
        ++level.order_count;
    }

    for (const auto& [price, level] : bids) {
        if (out.bids.size() >= max_levels) {
            break;
        }
        out.bids.push_back(level);
    }
    for (const auto& [price, level] : asks) {
        if (out.asks.size() >= max_levels) {
            break;
        }
        out.asks.push_back(level);
    }
}

}  // namespace xc::model
