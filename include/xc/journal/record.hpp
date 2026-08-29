#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "xc/core/commands.hpp"
#include "xc/core/instrument.hpp"
#include "xc/core/types.hpp"

namespace xc::journal {

/// What a journal record describes.
///
/// The journal records **inputs, not outputs**. It stores the commands the
/// engine was given, never the fills or book states it produced.
///
/// That is the decision the whole recovery story rests on. Matching is a pure
/// function of the command sequence, so replaying the inputs reconstructs the
/// outputs exactly -- while a journal of outputs would be far larger, would go
/// stale the moment a bug in matching was fixed, and could never be used to
/// answer "what would this build have done with yesterday's flow?".
enum class RecordType : std::uint8_t {
    /// An instrument became tradable. Written before any command that
    /// references it, so replay can rebuild the venue's configuration from the
    /// journal alone rather than depending on a config file that may have been
    /// edited since.
    InstrumentDefined = 1,
    NewOrder = 2,
    CancelOrder = 3,
    ReplaceOrder = 4,
};

/// Format version, written into every record.
///
/// A reader that meets a version it does not understand stops rather than
/// guessing. Silently misparsing an old journal would produce a plausible but
/// wrong reconstruction, which is worse than refusing to produce one.
inline constexpr std::uint8_t kRecordVersion = 1;

/// Fixed-size prefix on every record.
///
///   offset  size  field
///        0     4  payload length
///        4     4  CRC-32C over the rest of the header and the payload
///        8     8  engine sequence number
///       16     1  record type
///       17     1  format version
///       18     2  reserved, must be zero
///
/// The checksum covers the sequence, type, version and payload but not the
/// length or itself. Length is excluded deliberately: a reader has to trust the
/// length field before it can find the end of the record to check anything, so
/// a length is validated by bounds rather than by checksum, and a corrupt one
/// is caught as a short or implausible record instead.
struct RecordHeader {
    static constexpr std::size_t kSize = 20;
    static constexpr std::uint32_t kMaxPayload = 1U << 20;

    std::uint32_t payload_length = 0;
    std::uint32_t checksum = 0;
    SeqNum sequence = 0;
    RecordType type = RecordType::NewOrder;
    std::uint8_t version = kRecordVersion;
};

/// One decoded journal entry.
///
/// A flat struct with one member per record type rather than a variant. The
/// journal is written once per command and read once per recovery, so the few
/// unused bytes cost nothing measurable, and a plain struct keeps both the
/// encoder and the tests obvious.
struct Record {
    RecordType type = RecordType::NewOrder;
    SeqNum sequence = 0;

    /// The timestamp the engine stamped when it first processed this command.
    ///
    /// Recorded because replay must not read a wall clock. A replayed run that
    /// called the real clock would stamp different times from the original and
    /// could not be compared against it -- which would defeat the point of
    /// replaying at all.
    Nanos timestamp = 0;

    Instrument instrument;
    NewOrder new_order;
    CancelOrder cancel_order;
    ReplaceOrder replace_order;
};

/// Appends the full encoded record -- header and payload -- to `out`.
void encode(const Record& record, std::vector<std::uint8_t>& out);

enum class DecodeStatus : std::uint8_t {
    Ok,
    /// Fewer bytes are available than the record needs. During recovery this
    /// means the journal ends mid-record, which is the expected shape of a
    /// crash rather than an error.
    Incomplete,
    /// The record is intact in length but its contents do not match their
    /// checksum.
    Corrupt,
    /// The header is not something this build can interpret: an unknown type,
    /// an unknown version, a reserved field that is not zero, or a length
    /// beyond the maximum.
    Unreadable,
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Incomplete;
    /// Bytes consumed. Meaningful only when status is Ok.
    std::size_t consumed = 0;
    Record record;
};

/// Decodes the record at the start of `data`.
DecodeResult decode(std::span<const std::uint8_t> data);

}  // namespace xc::journal
