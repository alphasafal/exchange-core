#include "xc/risk/kill_switch.hpp"

namespace xc::risk {

void KillSwitch::halt_venue(HaltReason reason, Nanos now) {
    venue_ = HaltState{true, reason, now};
}

void KillSwitch::resume_venue() {
    venue_ = HaltState{};
}

void KillSwitch::halt_account(AccountId account, HaltReason reason, Nanos now) {
    accounts_[account].state = HaltState{true, reason, now};
}

void KillSwitch::resume_account(AccountId account) {
    AccountHalt& halt = accounts_[account];
    halt.state = HaltState{};
    // Clearing the run as well, so that resuming does not leave the account one
    // reject away from tripping straight back into the halt it was just
    // released from.
    halt.consecutive_rejects = 0;
}

bool KillSwitch::account_halted(AccountId account) const {
    const auto it = accounts_.find(account);
    return it != accounts_.end() && it->second.state.halted;
}

HaltState KillSwitch::account_state(AccountId account) const {
    const auto it = accounts_.find(account);
    return it == accounts_.end() ? HaltState{} : it->second.state;
}

bool KillSwitch::blocks_new_orders(AccountId account) const {
    return venue_.halted || account_halted(account);
}

void KillSwitch::record_reject(AccountId account, Nanos now) {
    if (auto_trip_threshold_ == 0) {
        return;
    }
    AccountHalt& halt = accounts_[account];
    if (halt.state.halted) {
        return;
    }
    ++halt.consecutive_rejects;
    if (halt.consecutive_rejects >= auto_trip_threshold_) {
        halt.state = HaltState{true, HaltReason::RepeatedRejects, now};
    }
}

void KillSwitch::record_accept(AccountId account) {
    const auto it = accounts_.find(account);
    if (it != accounts_.end()) {
        it->second.consecutive_rejects = 0;
    }
}

std::uint32_t KillSwitch::consecutive_rejects(AccountId account) const {
    const auto it = accounts_.find(account);
    return it == accounts_.end() ? 0 : it->second.consecutive_rejects;
}

}  // namespace xc::risk
