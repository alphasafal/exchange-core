#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "xc/journal/record.hpp"

namespace xc::journal {

/// How a journal ended.
enum class RecoveryOutcome : std::uint8_t {
    /// Every byte of every segment decoded cleanly.
    Clean,

    /// The journal stops part-way through its final record.
    ///
    /// This is what a crash looks like, and it is not an error. A record is
    /// written with a single write() that the machine may interrupt at any
    /// point, so a torn tail is the expected outcome of losing power mid-append.
    /// The complete records before it are perfectly good.
    TornTail,

    /// A record failed its checksum, or was intact but unreadable by this
    /// build. Unlike a torn tail this indicates real damage or a version
    /// mismatch, and the records after it cannot be trusted to be findable.
    Damaged,

    /// A segment could not be read at all.
    Unreadable,
};

struct RecoveryReport {
    RecoveryOutcome outcome = RecoveryOutcome::Clean;

    std::uint64_t records_recovered = 0;
    std::uint64_t bytes_recovered = 0;
    std::uint32_t segments_read = 0;

    /// Sequence number of the last record recovered. Zero if none were.
    SeqNum last_sequence = 0;

    /// Where the good data ends in the last segment read. Truncating that file
    /// to this offset leaves a journal that is clean end to end.
    std::filesystem::path damaged_segment;
    std::uint64_t good_bytes_in_damaged_segment = 0;

    std::string message;

    bool usable() const noexcept {
        return outcome == RecoveryOutcome::Clean || outcome == RecoveryOutcome::TornTail;
    }
};

/// Reads a journal directory back in write order.
///
/// Segments are read one at a time and held whole in memory. Memory is
/// therefore bounded by the configured segment size rather than by the length
/// of the journal, so a run of any duration can be recovered on a machine that
/// can hold a single segment.
class JournalReader {
  public:
    explicit JournalReader(std::filesystem::path directory);

    /// Visits every recoverable record, in order, stopping at the first
    /// damaged or incomplete one.
    RecoveryReport read(const std::function<void(const Record&)>& visit);

    /// Truncates a damaged final segment to its last complete record, leaving a
    /// journal that reads cleanly.
    ///
    /// **Destructive, and deliberately not automatic.** Recovery reports what it
    /// found and lets the operator decide: discarding a torn tail after a power
    /// cut is routine, while discarding data after a checksum failure is
    /// throwing away evidence of a fault nobody has diagnosed yet.
    static bool truncate_to(const std::filesystem::path& segment, std::uint64_t bytes,
                            std::string& error);

    /// Segment files in the directory, in write order.
    std::vector<std::filesystem::path> segments() const;

  private:
    std::filesystem::path directory_;
};

}  // namespace xc::journal
