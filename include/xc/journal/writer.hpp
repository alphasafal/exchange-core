#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "xc/journal/record.hpp"

namespace xc::journal {

/// How hard the journal works to survive a crash.
///
/// The three settings are genuinely different guarantees, not degrees of
/// caution, and the cost of each is measured rather than asserted -- see
/// docs/benchmarks.md.
enum class Durability : std::uint8_t {
    /// Hand bytes to the kernel and never ask it to persist them. Survives the
    /// process being killed; loses whatever the page cache was holding if the
    /// machine loses power.
    None = 0,

    /// Persist at most every `sync_interval`. Bounds how much can be lost by
    /// time rather than by record count, which is the setting most venues
    /// actually run: unbounded loss is unacceptable, and paying for durability
    /// on every single message is rarely worth it.
    Interval = 1,

    /// Persist before every append returns. The strongest guarantee available
    /// and by far the most expensive, since it puts a device round trip on the
    /// command path.
    Always = 2,
};

struct WriterConfig {
    /// Directory holding the segment files. Created if absent.
    std::filesystem::path directory;

    /// Roll to a new segment once the current one reaches this size.
    ///
    /// Segments exist so that recovery, retention and archival all operate on
    /// bounded units. They also bound recovery's memory: a reader loads one
    /// segment at a time, so a journal of any length is replayable on a machine
    /// that can hold a single segment.
    std::uint64_t segment_bytes = 64ULL << 20;

    Durability durability = Durability::Interval;

    /// Longest a record may sit unpersisted under Durability::Interval.
    Nanos sync_interval = 100'000'000;  // 100 ms

    /// Bytes buffered in user space before writing to the file.
    std::size_t buffer_bytes = 1U << 20;
};

/// Appends records to an on-disk journal.
///
/// Writes go through a user-space buffer into a plain file descriptor. Nothing
/// here is thread-safe: the journal is written by the matching thread, in the
/// same total order the engine assigned, and moving it off that thread would
/// mean the order on disk could differ from the order that was executed.
class JournalWriter {
  public:
    explicit JournalWriter(WriterConfig config);
    ~JournalWriter();

    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    /// Opens the journal, creating the directory and the first segment.
    /// Continues an existing journal by starting a new segment after the
    /// highest-numbered one present, so an earlier run's records are never
    /// overwritten or appended to -- a partially written segment stays exactly
    /// as the crash left it, for recovery to inspect.
    bool open();

    /// Encodes and appends a record. `now` drives the interval sync.
    bool append(const Record& record, Nanos now);

    /// Pushes the user-space buffer into the file. Does not persist it.
    bool flush();

    /// Persists everything written so far to durable storage.
    bool sync();

    /// Flushes, persists and closes. Called by the destructor.
    void close();

    /// False once any write has failed.
    ///
    /// A journal that cannot record a command must stop the venue rather than
    /// keep matching: continuing would execute trades that no replay could ever
    /// reproduce, which is precisely the state the journal exists to prevent.
    /// The engine integration halts on this.
    bool healthy() const noexcept { return healthy_; }

    const std::string& last_error() const noexcept { return last_error_; }

    std::uint64_t records_written() const noexcept { return records_written_; }
    std::uint64_t bytes_written() const noexcept { return bytes_written_; }
    std::uint32_t segments_opened() const noexcept { return segments_opened_; }

    /// How many times durable-storage persistence was actually requested. The
    /// benchmark reports throughput alongside this, so the cost of a durability
    /// setting can be attributed rather than guessed at.
    std::uint64_t syncs() const noexcept { return syncs_; }

    /// Path of the segment currently being written.
    const std::filesystem::path& current_segment() const noexcept { return current_path_; }

    /// Segment file name for an index, e.g. "segment-00000001.xcj".
    static std::string segment_name(std::uint32_t index);

  private:
    bool roll_segment();
    bool write_all(const std::uint8_t* data, std::size_t size);
    void fail(const std::string& message);

    WriterConfig config_;
    int fd_ = -1;
    std::filesystem::path current_path_;
    std::uint32_t segment_index_ = 0;
    std::uint64_t segment_size_ = 0;

    std::vector<std::uint8_t> buffer_;
    Nanos last_sync_ = 0;

    bool healthy_ = true;
    std::string last_error_;

    std::uint64_t records_written_ = 0;
    std::uint64_t bytes_written_ = 0;
    std::uint32_t segments_opened_ = 0;
    std::uint64_t syncs_ = 0;
};

}  // namespace xc::journal
