#pragma once

#include <cstdint>
#include <filesystem>

#include "xc/core/clock.hpp"
#include "xc/core/matching_engine.hpp"
#include "xc/journal/reader.hpp"
#include "xc/journal/record.hpp"

namespace xc::journal {

struct ReplayReport {
    RecoveryReport recovery;

    std::uint64_t instruments_defined = 0;
    std::uint64_t orders_submitted = 0;
    std::uint64_t cancels_applied = 0;
    std::uint64_t replaces_applied = 0;

    /// Fingerprint of the engine once the journal has been replayed.
    std::uint64_t state_digest = 0;
};

/// Rebuilds engine state by replaying a journal.
///
/// The engine is handed a ManualClock rather than a real one, and each record
/// sets it to the timestamp the original run stamped. A replay that read the
/// wall clock would produce different timestamps from the run it is
/// reconstructing and could not be compared against it.
///
/// Each record also restores the sequence number the original command was
/// given. Queue priority is derived from that number, so a replay that
/// renumbered commands would match every trade correctly and still leave the
/// resting orders in a different order.
///
/// No risk component is installed. Commands refused by risk never reached the
/// book and were never journaled, so what remains is exactly the stream the
/// book saw -- which is why replay does not need a copy of the risk
/// configuration to be exact.
class Replayer {
  public:
    Replayer(MatchingEngine& engine, ManualClock& clock);

    /// Applies one journaled record.
    void apply(const Record& record);

    /// Reads the journal at `directory` and applies everything recoverable.
    ReplayReport replay(const std::filesystem::path& directory);

    const ReplayReport& report() const noexcept { return report_; }

  private:
    MatchingEngine& engine_;
    ManualClock& clock_;
    ReplayReport report_;
};

}  // namespace xc::journal
