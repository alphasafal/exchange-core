#include "xc/risk/risk_engine.hpp"

#include <algorithm>
#include <cassert>
#include <limits>

namespace xc::risk {

void RiskEngine::configure(AccountId account, const AccountLimits& limits) {
    state_for(account).limits = limits;
}

RiskEngine::AccountState& RiskEngine::state_for(AccountId account) {
    return accounts_[account];
}

const RiskEngine::AccountState* RiskEngine::find(AccountId account) const {
    const auto it = accounts_.find(account);
    return it == accounts_.end() ? nullptr : &it->second;
}

const AccountLimits* RiskEngine::limits_for(AccountId account) const {
    const AccountState* state = find(account);
    return state == nullptr ? nullptr : &state->limits;
}

void RiskEngine::configure_instrument(InstrumentId instrument, const InstrumentControls& controls) {
    instruments_[instrument].controls = controls;
}

void RiskEngine::set_reference_price(InstrumentId instrument, Price price) {
    if (price > 0) {
        instruments_[instrument].reference_price = price;
    }
}

Price RiskEngine::reference_price(InstrumentId instrument) const {
    const auto it = instruments_.find(instrument);
    return it == instruments_.end() ? kNoPrice : it->second.reference_price;
}

bool RiskEngine::consume_rate_budget(AccountState& state, Nanos now) const {
    const std::uint32_t rate = state.limits.max_messages_per_second;
    if (rate == 0) {
        return true;
    }

    // A generic cell rate algorithm rather than a counter over a fixed window.
    //
    // Fixed windows let an account send its whole allowance at the end of one
    // window and again at the start of the next, admitting twice the configured
    // rate across the boundary. This tracks a single "theoretical arrival time"
    // instead: each message pushes it forward by one emission interval, and a
    // message is admitted only if it arrives no earlier than that time less the
    // burst tolerance. The result has no window boundary to exploit, needs one
    // integer of state per account, and never drifts, because nothing is ever
    // divided into a floating point rate.
    const Nanos interval = static_cast<Nanos>(1'000'000'000ULL / rate);
    const std::uint32_t burst = state.limits.message_burst == 0 ? 1 : state.limits.message_burst;
    const Nanos tolerance = static_cast<Nanos>(burst - 1) * interval;

    if (now < state.theoretical_arrival - tolerance) {
        return false;
    }
    state.theoretical_arrival = std::max(now, state.theoretical_arrival) + interval;
    return true;
}

RejectReason RiskEngine::check_collar(const NewOrder& command) const {
    const auto it = instruments_.find(command.instrument);
    if (it == instruments_.end() || it->second.controls.collar_bps == 0) {
        return RejectReason::None;
    }
    const Price reference = it->second.reference_price;
    // With no reference price there is nothing honest to measure against. A
    // venue that has not traded yet cannot tell a fat finger from price
    // discovery, and guessing would reject the first legitimate order of the
    // session.
    if (reference <= 0 || command.type != OrderType::Limit || command.price <= 0) {
        return RejectReason::None;
    }

    const Price deviation =
        command.price > reference ? command.price - reference : reference - command.price;

    // The comparison is deviation * 10000 > reference * collar_bps, performed
    // without ever forming a product that could overflow.
    //
    // An earlier version used __int128. Clang accepts it; GCC rejects it under
    // -Wpedantic as a non-standard extension, which the CI matrix caught. Both
    // operands are bounded instead, and where a product would overflow the
    // answer is already settled by how extreme the inputs are.
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    constexpr std::uint64_t kBasisPoints = 10'000;

    const auto scaled_deviation = static_cast<std::uint64_t>(deviation);
    const auto scaled_reference = static_cast<std::uint64_t>(reference);
    const std::uint64_t collar = it->second.controls.collar_bps;

    if (scaled_deviation > kMax / kBasisPoints) {
        // Further from the reference than any configurable band could reach.
        // Rejecting is both correct and the conservative direction.
        return RejectReason::PriceCollar;
    }
    if (scaled_reference > kMax / collar) {
        // The permitted band is wider than the representable price range, so
        // nothing can fall outside it.
        return RejectReason::None;
    }

    return scaled_deviation * kBasisPoints > scaled_reference * collar ? RejectReason::PriceCollar
                                                                       : RejectReason::None;
}

RejectReason RiskEngine::check(const NewOrder& command, const Instrument& instrument, Nanos now) {
    AccountState* state = nullptr;
    if (const auto it = accounts_.find(command.account); it != accounts_.end()) {
        state = &it->second;
    }

    // Cheapest first, and rate limiting before everything else so that a flood
    // is turned away before any more expensive work is done for it.
    if (state != nullptr && !consume_rate_budget(*state, now)) {
        return RejectReason::RateLimit;
    }

    if (const RejectReason collar = check_collar(command); collar != RejectReason::None) {
        return collar;
    }

    if (state == nullptr) {
        return RejectReason::None;  // Unconfigured accounts are unconstrained.
    }
    const AccountLimits& limits = state->limits;

    if (command.quantity > limits.max_order_quantity) {
        return RejectReason::RiskLimit;
    }

    // Notional is only meaningful for an order that carries a price. A market
    // order's cost depends on liquidity that has not been consumed yet, so it
    // is bounded by the quantity and position limits instead.
    if (command.type == OrderType::Limit && command.price > 0) {
        const auto notional = static_cast<std::uint64_t>(command.price) *
                              static_cast<std::uint64_t>(command.quantity);
        // Overflow would wrap to a small number and pass a limit it should
        // fail, so the division is done rather than the multiplication when the
        // quantity is large enough to matter.
        const bool overflowed =
            command.quantity != 0 && notional / static_cast<std::uint64_t>(command.quantity) !=
                                         static_cast<std::uint64_t>(command.price);
        if (overflowed || notional > limits.max_order_notional) {
            return RejectReason::RiskLimit;
        }
    }

    if (state->open_orders >= limits.max_open_orders) {
        return RejectReason::RiskLimit;
    }

    if (limits.max_position != std::numeric_limits<Quantity>::max()) {
        const auto it = state->exposure.find(command.instrument);
        Exposure projected = it == state->exposure.end() ? Exposure{} : it->second;

        // Checked against where this order could take the account if everything
        // working were to fill, not against where it stands now.
        if (command.side == Side::Buy) {
            projected.working_buy += command.quantity;
        } else {
            projected.working_sell += command.quantity;
        }

        const auto cap = static_cast<std::int64_t>(limits.max_position);
        if (projected.projected_long() > cap || projected.projected_short() > cap) {
            return RejectReason::RiskLimit;
        }
    }

    (void)instrument;
    return RejectReason::None;
}

void RiskEngine::on_order_rested(const Order& order) {
    AccountState& state = state_for(order.account);
    ++state.open_orders;
    Exposure& exposure = state.exposure[order.instrument];
    if (order.is_buy()) {
        exposure.working_buy += order.remaining;
    } else {
        exposure.working_sell += order.remaining;
    }
}

void RiskEngine::on_working_released(AccountId account, InstrumentId instrument, Side side,
                                     Quantity quantity) {
    const auto account_it = accounts_.find(account);
    if (account_it == accounts_.end()) {
        return;
    }
    const auto exposure_it = account_it->second.exposure.find(instrument);
    if (exposure_it == account_it->second.exposure.end()) {
        return;
    }

    Exposure& exposure = exposure_it->second;
    Quantity& working = side == Side::Buy ? exposure.working_buy : exposure.working_sell;
    // Clamped rather than asserted. Releasing more than was recorded means the
    // engine reported something twice, and the safe failure is to end up flat
    // -- an underflowed unsigned working total would look like an enormous
    // commitment and lock the account out of the venue entirely.
    working -= std::min(working, quantity);
}

void RiskEngine::on_fill(const Fill& fill) {
    const auto apply = [&](AccountId account, Side side) {
        const auto account_it = accounts_.find(account);
        if (account_it == accounts_.end()) {
            return;
        }
        Exposure& exposure = account_it->second.exposure[fill.instrument];
        const auto quantity = static_cast<std::int64_t>(fill.quantity);
        if (side == Side::Buy) {
            exposure.net_position += quantity;
            exposure.working_buy -= std::min(exposure.working_buy, fill.quantity);
        } else {
            exposure.net_position -= quantity;
            exposure.working_sell -= std::min(exposure.working_sell, fill.quantity);
        }
    };

    // A fill always has two sides, and both accounts' exposure moves at the
    // same instant. Updating only the aggressor would leave the resting side
    // holding a position the venue does not know about.
    apply(fill.aggressor_account, fill.aggressor_side);
    apply(fill.resting_account, opposite(fill.aggressor_side));
}

void RiskEngine::on_order_closed(AccountId account) {
    const auto it = accounts_.find(account);
    if (it == accounts_.end()) {
        return;
    }
    if (it->second.open_orders > 0) {
        --it->second.open_orders;
    }
}

Exposure RiskEngine::exposure(AccountId account, InstrumentId instrument) const {
    const AccountState* state = find(account);
    if (state == nullptr) {
        return Exposure{};
    }
    const auto it = state->exposure.find(instrument);
    return it == state->exposure.end() ? Exposure{} : it->second;
}

std::uint32_t RiskEngine::open_orders(AccountId account) const {
    const AccountState* state = find(account);
    return state == nullptr ? 0 : state->open_orders;
}

}  // namespace xc::risk
