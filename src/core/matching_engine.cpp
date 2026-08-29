#include "xc/core/matching_engine.hpp"

#include <utility>

namespace xc {
namespace {

constexpr std::size_t kFillsReserve = 256;

}  // namespace

MatchingEngine::MatchingEngine(Clock& clock) : clock_(clock) {
    fills_.reserve(kFillsReserve);
}

bool MatchingEngine::add_instrument(const Instrument& instrument) {
    if (!instrument.id.valid() || instrument.symbol.empty()) {
        return false;
    }
    if (books_.contains(instrument.id) || symbols_.contains(instrument.symbol)) {
        return false;
    }
    symbols_.emplace(instrument.symbol, instrument.id);
    books_.emplace(instrument.id, std::make_unique<OrderBook>(instrument));
    return true;
}

const Instrument* MatchingEngine::find_instrument(InstrumentId id) const {
    const auto it = books_.find(id);
    return it == books_.end() ? nullptr : &it->second->instrument();
}

const Instrument* MatchingEngine::find_instrument(std::string_view symbol) const {
    const auto it = symbols_.find(std::string(symbol));
    return it == symbols_.end() ? nullptr : find_instrument(it->second);
}

OrderBook* MatchingEngine::book_for(InstrumentId id) {
    const auto it = books_.find(id);
    return it == books_.end() ? nullptr : it->second.get();
}

const OrderBook* MatchingEngine::book(InstrumentId id) const {
    const auto it = books_.find(id);
    return it == books_.end() ? nullptr : it->second.get();
}

void MatchingEngine::add_listener(EngineListener* listener) {
    if (listener != nullptr) {
        listeners_.push_back(listener);
    }
}

SubmitOutcome MatchingEngine::submit(const NewOrder& command) {
    SubmitOutcome outcome;
    // Stamped before anything is decided, so the ordering does not depend on
    // the outcome. See the note on the class.
    outcome.sequence = next_sequence();
    fills_.clear();

    OrderBook* target = book_for(command.instrument);
    if (target == nullptr || !command.account.valid()) {
        outcome.reject =
            target == nullptr ? RejectReason::UnknownInstrument : RejectReason::UnknownOrder;
        for (EngineListener* listener : listeners_) {
            listener->on_order_rejected(outcome.sequence, command, outcome.reject);
        }
        return outcome;
    }

    Order order;
    order.id = command.id;
    order.account = command.account;
    order.instrument = command.instrument;
    order.side = command.side;
    order.type = command.type;
    order.tif = command.tif;
    order.price = command.price;
    order.quantity = command.quantity;
    order.remaining = command.quantity;
    order.post_only = command.post_only;
    // The client supplies neither of these. Letting it choose either would hand
    // it control over its own queue priority.
    order.sequence = outcome.sequence;
    order.accepted_at = clock_.now();

    static_cast<SubmitResult&>(outcome) = target->submit(order, fills_);

    if (!outcome.accepted()) {
        for (EngineListener* listener : listeners_) {
            listener->on_order_rejected(outcome.sequence, command, outcome.reject);
        }
        return outcome;
    }

    for (EngineListener* listener : listeners_) {
        listener->on_order_accepted(outcome.sequence, order);
        if (!fills_.empty()) {
            listener->on_fills(outcome.sequence, fills_);
        }
    }
    return outcome;
}

CancelOutcome MatchingEngine::cancel(const CancelOrder& command) {
    CancelOutcome outcome;
    outcome.sequence = next_sequence();
    fills_.clear();

    OrderBook* target = book_for(command.instrument);
    if (target == nullptr) {
        outcome.reject = RejectReason::UnknownInstrument;
        return outcome;
    }

    const Order* resting = target->find(command.id);
    // An account may only cancel its own orders. Reported as UnknownOrder
    // rather than as a permission failure: telling one account that another's
    // order exists leaks the shape of the book to anyone willing to guess ids.
    if (resting == nullptr || resting->account != command.account) {
        outcome.reject = RejectReason::UnknownOrder;
        return outcome;
    }

    static_cast<CancelResult&>(outcome) = target->cancel(command.id);
    if (outcome.accepted()) {
        for (EngineListener* listener : listeners_) {
            listener->on_order_cancelled(outcome.sequence, outcome.order);
        }
    }
    return outcome;
}

ReplaceOutcome MatchingEngine::replace(const ReplaceOrder& command) {
    ReplaceOutcome outcome;
    outcome.sequence = next_sequence();
    fills_.clear();

    OrderBook* target = book_for(command.instrument);
    if (target == nullptr) {
        outcome.reject = RejectReason::UnknownInstrument;
        return outcome;
    }

    const Order* resting = target->find(command.id);
    if (resting == nullptr || resting->account != command.account) {
        outcome.reject = RejectReason::UnknownOrder;
        return outcome;
    }

    static_cast<ReplaceResult&>(outcome) =
        target->replace(command.id, command.new_price, command.new_quantity, outcome.sequence,
                        clock_.now(), fills_);

    if (!outcome.accepted()) {
        return outcome;
    }

    const Order* amended = target->find(command.id);
    for (EngineListener* listener : listeners_) {
        if (amended != nullptr) {
            listener->on_order_replaced(outcome.sequence, outcome.previous, *amended);
        } else {
            // The amendment removed the order outright -- either it was reduced
            // to at or below what had already traded, or it crossed and filled.
            listener->on_order_cancelled(outcome.sequence, outcome.previous);
        }
        if (!fills_.empty()) {
            listener->on_fills(outcome.sequence, fills_);
        }
    }
    return outcome;
}

}  // namespace xc
