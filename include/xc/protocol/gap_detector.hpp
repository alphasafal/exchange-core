#pragma once

#include <cstdint>

#include "xc/core/types.hpp"

namespace xc::protocol {

/// What a subscriber should do about the message it just received.
enum class SequenceCheck : std::uint8_t {
    /// The next message in the stream. Process it.
    InOrder,

    /// Messages are missing.
    ///
    /// The subscriber's book is now wrong and cannot be repaired by anything in
    /// this message: an incremental update describes a change to a state the
    /// subscriber no longer has. It has to be resynchronised from a snapshot.
    /// The message that revealed the gap is still valid and is applied after
    /// the snapshot catches up to it.
    Gap,

    /// Already seen. Networks duplicate packets, and a subscriber that applies
    /// an increment twice ends up with a book that is wrong in a way nothing
    /// downstream can detect. Duplicates are dropped silently.
    Duplicate,
};

/// Tracks sequence continuity on a market data feed.
///
/// Market data goes out over UDP, which may drop, duplicate or reorder. None of
/// those can be prevented, so the protocol makes them *detectable*: every
/// message carries a sequence number in its header, where a subscriber can read
/// it without parsing a body it may not even implement.
///
/// Silent loss is the failure that matters. A subscriber trading on a book that
/// quietly diverged from the venue's is worse off than one that knows it is
/// blind, because it keeps acting with confidence on a view that is wrong.
class GapDetector {
  public:
    explicit GapDetector(SeqNum expected_first = 1) : expected_(expected_first) {}

    SequenceCheck observe(SeqNum sequence) {
        if (sequence == expected_) {
            ++expected_;
            ++in_order_;
            return SequenceCheck::InOrder;
        }

        if (sequence < expected_) {
            ++duplicates_;
            return SequenceCheck::Duplicate;
        }

        // Ahead of what was expected: everything between is missing. The
        // expectation jumps past the gap rather than waiting for messages that
        // will never arrive -- a subscriber stuck expecting a lost sequence
        // would treat the entire rest of the session as out of order.
        missing_ += sequence - expected_;
        ++gaps_;
        expected_ = sequence + 1;
        return SequenceCheck::Gap;
    }

    /// Restarts tracking from a snapshot valid as of `sequence`. Every
    /// increment up to and including it is already reflected in the snapshot.
    void resynchronise(SeqNum sequence) { expected_ = sequence + 1; }

    SeqNum expected() const noexcept { return expected_; }
    std::uint64_t in_order() const noexcept { return in_order_; }
    std::uint64_t gaps() const noexcept { return gaps_; }
    std::uint64_t missing() const noexcept { return missing_; }
    std::uint64_t duplicates() const noexcept { return duplicates_; }

  private:
    SeqNum expected_ = 1;
    std::uint64_t in_order_ = 0;
    std::uint64_t gaps_ = 0;
    std::uint64_t missing_ = 0;
    std::uint64_t duplicates_ = 0;
};

}  // namespace xc::protocol
