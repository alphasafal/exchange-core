#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "xc/journal/reader.hpp"
#include "xc/journal/writer.hpp"

namespace xc::journal {
namespace {

/// A journal directory that cleans up after itself, so a failing test cannot
/// leave state that makes the next run behave differently.
class TempJournal {
  public:
    TempJournal() {
        path_ =
            std::filesystem::temp_directory_path() /
            ("xc-journal-test-" + std::to_string(::getpid()) + "-" + std::to_string(++counter_));
        std::filesystem::remove_all(path_);
    }
    ~TempJournal() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

Record order_record(SeqNum sequence, Quantity quantity = 10) {
    Record record;
    record.type = RecordType::NewOrder;
    record.sequence = sequence;
    record.timestamp = static_cast<Nanos>(sequence) * 1000;
    record.new_order.id = OrderId{sequence};
    record.new_order.account = AccountId{1};
    record.new_order.instrument = InstrumentId{1};
    record.new_order.price = 100;
    record.new_order.quantity = quantity;
    return record;
}

std::vector<Record> read_all(const std::filesystem::path& directory, RecoveryReport& report) {
    std::vector<Record> records;
    JournalReader reader(directory);
    report = reader.read([&](const Record& record) { records.push_back(record); });
    return records;
}

TEST(JournalIo, RoundTripsRecordsInWriteOrder) {
    TempJournal temp;
    {
        JournalWriter writer(WriterConfig{.directory = temp.path()});
        ASSERT_TRUE(writer.open()) << writer.last_error();
        for (SeqNum i = 1; i <= 500; ++i) {
            ASSERT_TRUE(writer.append(order_record(i), 0)) << writer.last_error();
        }
        EXPECT_EQ(writer.records_written(), 500u);
    }

    RecoveryReport report;
    const std::vector<Record> records = read_all(temp.path(), report);

    EXPECT_EQ(report.outcome, RecoveryOutcome::Clean);
    EXPECT_TRUE(report.usable());
    ASSERT_EQ(records.size(), 500u);
    EXPECT_EQ(report.last_sequence, 500u);
    for (std::size_t i = 0; i < records.size(); ++i) {
        EXPECT_EQ(records[i].sequence, i + 1);
        EXPECT_EQ(records[i].new_order.id, OrderId{i + 1});
    }
}

TEST(JournalIo, RollsSegmentsAndReadsThemInOrder) {
    TempJournal temp;
    {
        // Small enough that a few hundred records span many segments.
        JournalWriter writer(
            WriterConfig{.directory = temp.path(), .segment_bytes = 512, .buffer_bytes = 64});
        ASSERT_TRUE(writer.open());
        for (SeqNum i = 1; i <= 300; ++i) {
            ASSERT_TRUE(writer.append(order_record(i), 0));
        }
        EXPECT_GT(writer.segments_opened(), 5u);
    }

    JournalReader reader(temp.path());
    EXPECT_GT(reader.segments().size(), 5u);

    RecoveryReport report;
    const std::vector<Record> records = read_all(temp.path(), report);
    ASSERT_EQ(records.size(), 300u);
    for (std::size_t i = 0; i < records.size(); ++i) {
        // Segments are ordered by the index in the name, so a journal split
        // across many files still reads back as one stream in write order.
        EXPECT_EQ(records[i].sequence, i + 1);
    }
}

TEST(JournalIo, ReopeningStartsANewSegmentAndKeepsTheOldOne) {
    TempJournal temp;
    {
        JournalWriter writer(WriterConfig{.directory = temp.path()});
        ASSERT_TRUE(writer.open());
        writer.append(order_record(1), 0);
    }
    {
        JournalWriter writer(WriterConfig{.directory = temp.path()});
        ASSERT_TRUE(writer.open());
        writer.append(order_record(2), 0);
    }

    // A segment left by an earlier run must never be reused or appended to: a
    // crash may have left it half-written, and recovery needs to see it exactly
    // as it was.
    JournalReader reader(temp.path());
    EXPECT_EQ(reader.segments().size(), 2u);

    RecoveryReport report;
    const std::vector<Record> records = read_all(temp.path(), report);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].sequence, 1u);
    EXPECT_EQ(records[1].sequence, 2u);
}

TEST(JournalIo, RecoversEveryCompleteRecordBeforeATornTail) {
    TempJournal temp;
    {
        JournalWriter writer(WriterConfig{.directory = temp.path()});
        ASSERT_TRUE(writer.open());
        for (SeqNum i = 1; i <= 50; ++i) {
            writer.append(order_record(i), 0);
        }
    }

    // Simulate losing power part-way through a write. A record is appended with
    // one write() that the machine can interrupt anywhere, so a torn tail is
    // the expected shape of a crash rather than a corruption.
    const std::filesystem::path segment = temp.path() / JournalWriter::segment_name(1);
    const auto full_size = std::filesystem::file_size(segment);
    std::string error;
    ASSERT_TRUE(JournalReader::truncate_to(segment, full_size - 7, error)) << error;

    RecoveryReport report;
    const std::vector<Record> records = read_all(temp.path(), report);

    EXPECT_EQ(report.outcome, RecoveryOutcome::TornTail);
    EXPECT_TRUE(report.usable()) << "the complete records before the tear are perfectly good";
    EXPECT_EQ(records.size(), 49u);
    EXPECT_EQ(report.last_sequence, 49u);
    EXPECT_EQ(report.damaged_segment, segment);
    EXPECT_GT(report.good_bytes_in_damaged_segment, 0u);
}

TEST(JournalIo, TruncatingToTheReportedOffsetLeavesACleanJournal) {
    TempJournal temp;
    {
        JournalWriter writer(WriterConfig{.directory = temp.path()});
        ASSERT_TRUE(writer.open());
        for (SeqNum i = 1; i <= 20; ++i) {
            writer.append(order_record(i), 0);
        }
    }

    const std::filesystem::path segment = temp.path() / JournalWriter::segment_name(1);
    std::string error;
    ASSERT_TRUE(
        JournalReader::truncate_to(segment, std::filesystem::file_size(segment) - 3, error));

    RecoveryReport first;
    read_all(temp.path(), first);
    ASSERT_EQ(first.outcome, RecoveryOutcome::TornTail);

    ASSERT_TRUE(JournalReader::truncate_to(first.damaged_segment,
                                           first.good_bytes_in_damaged_segment, error))
        << error;

    RecoveryReport second;
    const std::vector<Record> records = read_all(temp.path(), second);
    EXPECT_EQ(second.outcome, RecoveryOutcome::Clean);
    EXPECT_EQ(records.size(), first.records_recovered);
}

TEST(JournalIo, StopsAtACorruptRecordRatherThanReadingPastIt) {
    TempJournal temp;
    {
        JournalWriter writer(WriterConfig{.directory = temp.path()});
        ASSERT_TRUE(writer.open());
        for (SeqNum i = 1; i <= 30; ++i) {
            writer.append(order_record(i), 0);
        }
    }

    // Flip a bit inside the tenth record.
    const std::filesystem::path segment = temp.path() / JournalWriter::segment_name(1);
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(segment, std::ios::binary);
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::size_t record_size = bytes.size() / 30;
    bytes[record_size * 9 + 12] ^= 0x40U;
    {
        std::ofstream out(segment, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    RecoveryReport report;
    const std::vector<Record> records = read_all(temp.path(), report);

    EXPECT_EQ(report.outcome, RecoveryOutcome::Damaged);
    EXPECT_FALSE(report.usable()) << "damage is not a torn tail and must not be treated as one";
    EXPECT_EQ(records.size(), 9u) << "everything before the damage is still recovered";

    // There is no framing marker to resynchronise on, so anything found after
    // the damage would be bytes that merely happen to parse. A short honest
    // recovery beats a plausible wrong one.
    EXPECT_EQ(report.records_recovered, 9u);
}

TEST(JournalIo, ReportsAnEmptyDirectoryAsClean) {
    TempJournal temp;
    std::filesystem::create_directories(temp.path());

    RecoveryReport report;
    const std::vector<Record> records = read_all(temp.path(), report);
    EXPECT_EQ(report.outcome, RecoveryOutcome::Clean);
    EXPECT_TRUE(records.empty());
    EXPECT_EQ(report.last_sequence, 0u);
}

TEST(JournalIo, CountsSyncsUnderEachDurabilityPolicy) {
    const auto syncs_for = [](Durability durability, Nanos interval) {
        TempJournal temp;
        JournalWriter writer(WriterConfig{
            .directory = temp.path(), .durability = durability, .sync_interval = interval});
        EXPECT_TRUE(writer.open());
        for (SeqNum i = 1; i <= 100; ++i) {
            // One microsecond of simulated time between records.
            writer.append(order_record(i), static_cast<Nanos>(i) * 1000);
        }
        writer.close();
        return writer.syncs();
    };

    // Durability is a real cost, and its magnitude has to be attributable
    // rather than guessed at -- so the count is exposed and the benchmark
    // reports throughput alongside it.
    const std::uint64_t never = syncs_for(Durability::None, 0);
    const std::uint64_t interval = syncs_for(Durability::Interval, 10'000);
    const std::uint64_t always = syncs_for(Durability::Always, 0);

    EXPECT_GE(always, 100u) << "one persist per record";
    EXPECT_GT(always, interval);
    EXPECT_GT(interval, never);
}

TEST(JournalIo, ReportsFailureRatherThanLosingRecordsSilently) {
    JournalWriter writer(WriterConfig{.directory = "/dev/null/cannot-create-here"});
    EXPECT_FALSE(writer.open());
    EXPECT_FALSE(writer.healthy());
    EXPECT_FALSE(writer.last_error().empty());

    // A journal that cannot record a command must not accept one. Continuing
    // would execute trades no replay could reproduce -- exactly the state the
    // journal exists to prevent.
    EXPECT_FALSE(writer.append(order_record(1), 0));
}

}  // namespace
}  // namespace xc::journal
