#include "xc/core/matching_engine.hpp"

#include <algorithm>

#include <utility>

namespace xc {
namespace {

constexpr std::size_t kFillsReserve = 256;

}  // namespace

MatchingEngine::MatchingEngine(Clock& clock) : clock_(clock) {
    fills_.reserve(kFillsReserve);
    withdrawals_.reserve(kFillsReserve);
}

void MatchingEngine::settle_risk(InstrumentId instrument) {
    if (risk_ == nullptr) {
        return;
    }

    for (const Fill& fill : fills_) {
        risk_->on_fill(fill);
        if (fill.resting_filled) {
            // The resting order left the book, so it is no longer one of its
            // account's open orders.
            risk_->on_order_closed(fill.resting_account);
        }
    }

    for (const Withdrawal& withdrawal : withdrawals_) {
        risk_->on_working_released(withdrawal.account, withdrawal.instrument, withdrawal.side,
                                   withdrawal.quantity);
        if (withdrawal.fully_removed) {
            risk_->on_order_closed(withdrawal.account);
        }
    }

    // The collar tracks the last price at which two parties actually dealt.
    if (!fills_.empty()) {
        risk_->set_reference_price(instrument, fills_.back().price);
    }
}

bool MatchingEngine::add_instrument(const Instrument& instrument) {
    if (!instrument.id.valid() || instrument.symbol.empty()) {
        return false;
    }
    if (books_.contains(instrument.id) || symbols_.contains(instrument.symbol)) {
        return false;
    }
    // An instrument becoming tradable is an event in the venue's total order,
    // so it takes a sequence number like any other -- and unconditionally,
    // whether or not a journal is attached. If numbering depended on the
    // journal being enabled, the same command stream would produce different
    // sequence numbers with logging on and off, and a journaled run could never
    // be compared against an unjournaled one.
    const SeqNum sequence = next_sequence();

    // Journaled before the instrument becomes usable, so that a replay rebuilds
    // the venue's configuration from the log rather than trusting a config file
    // that may have been edited since the run it is replaying.
    if (journal_ != nullptr) {
        journal::Record record;
        record.type = journal::RecordType::InstrumentDefined;
        record.sequence = sequence;
        record.timestamp = clock_.now();
        record.instrument = instrument;
        if (!journal_->append(record, record.timestamp)) {
            return false;
        }
    }

    symbols_.emplace(instrument.symbol, instrument.id);
    books_.emplace(instrument.id, std::make_unique<OrderBook>(instrument));

    instrument_order_.push_back(instrument.id);
    std::sort(instrument_order_.begin(), instrument_order_.end());
    return true;
}

bool MatchingEngine::journal_command(const journal::Record& record, Nanos now) {
    if (journal_ == nullptr) {
        return true;
    }
    if (journal_->append(record, now)) {
        return true;
    }
    // A command that could not be recorded must not be executed. Halting the
    // venue is severe, and it is the correct response: every trade from here on
    // would be one no replay could reproduce.
    if (kill_ != nullptr) {
        kill_->halt_venue(risk::HaltReason::RiskBreach, now);
    }
    return false;
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
    fills_.clear();
    withdrawals_.clear();

    OrderBook* target = book_for(command.instrument);
    if (target == nullptr || !command.account.valid()) {
        outcome.reject =
            target == nullptr ? RejectReason::UnknownInstrument : RejectReason::UnknownOrder;
        for (EngineListener* listener : listeners_) {
            listener->on_order_rejected(outcome.sequence, command, outcome.reject);
        }
        return outcome;
    }

    const Nanos now = clock_.now();

    // A halt stops new orders and nothing else. Cancellation stays open, which
    // is the whole point: an account is halted exactly when it most needs to
    // withdraw what it already has resting.
    if (kill_ != nullptr && kill_->blocks_new_orders(command.account)) {
        outcome.reject = RejectReason::Halted;
        for (EngineListener* listener : listeners_) {
            listener->on_order_rejected(outcome.sequence, command, outcome.reject);
        }
        return outcome;
    }

    if (risk_ != nullptr) {
        outcome.reject = risk_->check(command, target->instrument(), now);
        if (!outcome.accepted()) {
            if (kill_ != nullptr) {
                kill_->record_reject(command.account, now);
            }
            for (EngineListener* listener : listeners_) {
                listener->on_order_rejected(outcome.sequence, command, outcome.reject);
            }
            return outcome;
        }
    }

    // The sequence number is assigned here: after every gate that could turn
    // the command away without touching the book, and immediately before the
    // command is journaled. Sequencing earlier would leave a gap in the
    // journal's numbering for every rejected command, and a gap in a journal
    // has to mean data loss -- if it can also mean "that one was rejected",
    // recovery has no way to tell the two apart.
    outcome.sequence = next_sequence();

    scratch_record_.type = journal::RecordType::NewOrder;
    scratch_record_.sequence = outcome.sequence;
    scratch_record_.timestamp = now;
    scratch_record_.new_order = command;
    if (!journal_command(scratch_record_, now)) {
        outcome.reject = RejectReason::Halted;
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
    order.accepted_at = now;

    static_cast<SubmitResult&>(outcome) = target->submit(order, fills_, &withdrawals_);

    if (!outcome.accepted()) {
        if (kill_ != nullptr) {
            kill_->record_reject(command.account, now);
        }
        for (EngineListener* listener : listeners_) {
            listener->on_order_rejected(outcome.sequence, command, outcome.reject);
        }
        return outcome;
    }

    if (kill_ != nullptr) {
        kill_->record_accept(command.account);
    }

    settle_risk(command.instrument);
    if (risk_ != nullptr && outcome.rested) {
        // Recorded from the order as it actually rests, not as it was
        // submitted: part of it may already have traded on the way in.
        if (const Order* rested = target->find(command.id); rested != nullptr) {
            risk_->on_order_rested(*rested);
        }
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
    fills_.clear();
    withdrawals_.clear();

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

    const Nanos now = clock_.now();
    outcome.sequence = next_sequence();
    scratch_record_.type = journal::RecordType::CancelOrder;
    scratch_record_.sequence = outcome.sequence;
    scratch_record_.timestamp = now;
    scratch_record_.cancel_order = command;
    if (!journal_command(scratch_record_, now)) {
        outcome.reject = RejectReason::Halted;
        return outcome;
    }

    static_cast<CancelResult&>(outcome) = target->cancel(command.id);
    if (outcome.accepted()) {
        if (risk_ != nullptr) {
            risk_->on_working_released(outcome.order.account, command.instrument,
                                       outcome.order.side, outcome.order.remaining);
            risk_->on_order_closed(outcome.order.account);
        }
        for (EngineListener* listener : listeners_) {
            listener->on_order_cancelled(outcome.sequence, outcome.order);
        }
    }
    return outcome;
}

ReplaceOutcome MatchingEngine::replace(const ReplaceOrder& command) {
    ReplaceOutcome outcome;
    fills_.clear();
    withdrawals_.clear();

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

    const Nanos now = clock_.now();
    outcome.sequence = next_sequence();
    scratch_record_.type = journal::RecordType::ReplaceOrder;
    scratch_record_.sequence = outcome.sequence;
    scratch_record_.timestamp = now;
    scratch_record_.replace_order = command;
    if (!journal_command(scratch_record_, now)) {
        outcome.reject = RejectReason::Halted;
        return outcome;
    }

    // Released before the amendment runs and re-recorded afterwards, so that
    // both paths -- an in-place reduction and a full re-queue -- settle through
    // the same code. Trying to compute a delta for the in-place case would mean
    // two ways to get exposure wrong instead of one.
    const Order previous_state = *resting;
    if (risk_ != nullptr) {
        risk_->on_working_released(previous_state.account, command.instrument, previous_state.side,
                                   previous_state.remaining);
        risk_->on_order_closed(previous_state.account);
    }

    static_cast<ReplaceResult&>(outcome) =
        target->replace(command.id, command.new_price, command.new_quantity, outcome.sequence, now,
                        fills_, &withdrawals_);

    if (!outcome.accepted()) {
        // The amendment was refused and the order is untouched, so the exposure
        // released above has to go straight back on.
        if (risk_ != nullptr) {
            risk_->on_order_rested(previous_state);
        }
        return outcome;
    }

    settle_risk(command.instrument);

    const Order* amended = target->find(command.id);
    if (risk_ != nullptr && amended != nullptr) {
        risk_->on_order_rested(*amended);
    }
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
