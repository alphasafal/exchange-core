#include "xc/journal/replayer.hpp"

#include "xc/core/state_digest.hpp"

namespace xc::journal {

Replayer::Replayer(MatchingEngine& engine, ManualClock& clock) : engine_(engine), clock_(clock) {}

void Replayer::apply(const Record& record) {
    clock_.set(record.timestamp);

    switch (record.type) {
        case RecordType::InstrumentDefined:
            // Instrument definitions take a sequence number too, so replay has
            // to restore theirs as well or every command after the last
            // instrument would be renumbered.
            engine_.restore_sequence(record.sequence - 1);
            engine_.add_instrument(record.instrument);
            ++report_.instruments_defined;
            return;

        case RecordType::NewOrder:
            // Restored so the replayed command receives the sequence number the
            // original run gave it. Priority depends on it.
            engine_.restore_sequence(record.sequence - 1);
            engine_.submit(record.new_order);
            ++report_.orders_submitted;
            return;

        case RecordType::CancelOrder:
            engine_.restore_sequence(record.sequence - 1);
            engine_.cancel(record.cancel_order);
            ++report_.cancels_applied;
            return;

        case RecordType::ReplaceOrder:
            engine_.restore_sequence(record.sequence - 1);
            engine_.replace(record.replace_order);
            ++report_.replaces_applied;
            return;
    }
}

ReplayReport Replayer::replay(const std::filesystem::path& directory) {
    JournalReader reader(directory);
    report_.recovery = reader.read([this](const Record& record) { apply(record); });
    report_.state_digest = digest(engine_);
    return report_;
}

}  // namespace xc::journal
