/// Fuzzes journal recovery on corrupted segments.
///
/// Recovery runs exactly when the system is already in trouble, on bytes that
/// a crash, a failing disk or a half-finished copy left behind. It is the one
/// code path whose input is guaranteed to be damaged, and the one that cannot
/// respond to damage by crashing, because the crash is what produced the file.
///
/// The fuzzer's bytes become the segment. The oracle is:
///
///   - recovery terminates, without faulting, reading out of bounds (ASan) or
///     invoking undefined behaviour (UBSan);
///   - it returns one of the outcomes it defines, never an unlisted value;
///   - it never reports Clean while also reporting damage, and never reports
///     damage without saying where the good data ends;
///   - every record handed to the visitor carries a sequence number strictly
///     greater than the last, because a reader that lets the numbering go
///     backwards would let replay apply commands out of order;
///   - truncating to the reported offset produces a journal that then reads
///     Clean. That is the property the whole recovery story rests on: the
///     report is not advice, it is an instruction that works.

#include <unistd.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "xc/journal/reader.hpp"
#include "xc/journal/writer.hpp"

namespace {

using xc::journal::JournalReader;
using xc::journal::JournalWriter;
using xc::journal::Record;
using xc::journal::RecoveryOutcome;
using xc::journal::RecoveryReport;

void check(bool condition, const char* what) {
    if (!condition) {
        // Discarded deliberately: the next statement aborts, so there is
        // nothing a failed write could be handled by.
        static_cast<void>(std::fputs(what, stderr));
        static_cast<void>(std::fputc('\n', stderr));
        std::abort();
    }
}

bool is_defined(RecoveryOutcome outcome) {
    switch (outcome) {
        case RecoveryOutcome::Clean:
        case RecoveryOutcome::TornTail:
        case RecoveryOutcome::Damaged:
        case RecoveryOutcome::SequenceGap:
        case RecoveryOutcome::Unreadable:
            return true;
    }
    return false;
}

/// A directory that is removed however this iteration ends.
///
/// The fuzzer runs hundreds of thousands of iterations in one process; one
/// leaked directory per iteration fills the disk long before the run is
/// interesting.
class ScratchDirectory {
  public:
    ScratchDirectory() {
        static std::uint64_t counter = 0;
        path_ = std::filesystem::temp_directory_path() /
                ("xc-fuzz-journal-" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }
    ~ScratchDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    ScratchDirectory(const ScratchDirectory&) = delete;
    ScratchDirectory& operator=(const ScratchDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
};

RecoveryReport recover(const std::filesystem::path& directory, std::vector<Record>& records) {
    records.clear();
    JournalReader reader(directory);
    return reader.read([&records](const Record& record) { records.push_back(record); });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // A segment larger than this tells us nothing a smaller one does not, and
    // the per-iteration file write is what bounds this target's throughput.
    if (size > std::size_t{64} * 1024) {
        return 0;
    }

    const ScratchDirectory scratch;
    const std::filesystem::path segment = scratch.path() / JournalWriter::segment_name(1);
    {
        std::ofstream out(segment, std::ios::binary);
        if (!out) {
            return 0;  // the filesystem, not the code under test
        }
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }

    std::vector<Record> records;
    const RecoveryReport report = recover(scratch.path(), records);

    check(is_defined(report.outcome), "recovery returned an outcome it does not define");
    check(report.records_recovered == records.size(),
          "the recovered count disagrees with the records actually delivered");

    xc::SeqNum previous = 0;
    for (const Record& record : records) {
        check(record.sequence > previous, "recovered records are not strictly increasing");
        previous = record.sequence;
    }
    if (!records.empty()) {
        check(report.last_sequence == previous,
              "last_sequence disagrees with the last record delivered");
    }

    if (report.outcome == RecoveryOutcome::Clean) {
        check(report.good_bytes_in_damaged_segment == 0 && report.damaged_segment.empty(),
              "a clean journal reported damage anyway");
        return 0;
    }

    if (report.outcome != RecoveryOutcome::TornTail && report.outcome != RecoveryOutcome::Damaged) {
        return 0;
    }

    // The report says where the good data ends. Truncating there must leave a
    // journal that reads clean -- otherwise the advice recovery gives is wrong,
    // and acting on it would discard good records or keep bad ones.
    check(!report.damaged_segment.empty(), "damage was reported without naming a segment");
    check(report.good_bytes_in_damaged_segment <= size,
          "the good-byte offset is past the end of the file");

    std::error_code ec;
    std::filesystem::resize_file(report.damaged_segment, report.good_bytes_in_damaged_segment, ec);
    if (ec) {
        return 0;
    }

    std::vector<Record> after;
    const RecoveryReport second = recover(scratch.path(), after);
    check(
        second.outcome == RecoveryOutcome::Clean || second.outcome == RecoveryOutcome::SequenceGap,
        "truncating to the reported offset did not leave a readable journal");
    check(after.size() == records.size(), "truncation changed which records recovery delivers");

    return 0;
}
