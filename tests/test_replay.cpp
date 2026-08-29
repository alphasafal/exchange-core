#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "flow_generator.hpp"
#include "order_book_fixture.hpp"
#include "xc/core/state_digest.hpp"
#include "xc/journal/replayer.hpp"
#include "xc/journal/writer.hpp"

namespace xc {
namespace {

using testing::Command;
using testing::generate_flow;
using testing::test_instrument;

class TempJournal {
  public:
    TempJournal() {
        path_ = std::filesystem::temp_directory_path() /
                ("xc-replay-test-" + std::to_string(::getpid()) + "-" + std::to_string(++counter_));
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

Instrument journal_instrument(std::uint64_t id, std::string symbol, SelfTradePolicy policy) {
    Instrument instrument = test_instrument();
    instrument.id = InstrumentId{id};
    instrument.symbol = std::move(symbol);
    instrument.self_trade_policy = policy;
    return instrument;
}

/// Drives a generated command stream through a journaling engine and returns
/// the digest of the state it ended in.
std::uint64_t run_and_journal(const std::filesystem::path& directory, std::uint64_t seed,
                              std::size_t command_count, SelfTradePolicy policy,
                              journal::Durability durability = journal::Durability::None,
                              std::uint64_t segment_bytes = 64ULL << 20) {
    journal::JournalWriter writer(journal::WriterConfig{
        .directory = directory, .segment_bytes = segment_bytes, .durability = durability});
    EXPECT_TRUE(writer.open()) << writer.last_error();

    ManualClock clock;
    MatchingEngine engine{clock};
    engine.set_journal(&writer);
    EXPECT_TRUE(engine.add_instrument(journal_instrument(1, "AAA", policy)));
    EXPECT_TRUE(engine.add_instrument(journal_instrument(2, "BBB", policy)));

    const std::vector<Command> stream = generate_flow(seed, command_count);
    for (const Command& command : stream) {
        clock.set(static_cast<Nanos>(command.sequence) * 1000);
        switch (command.kind) {
            case Command::Kind::Submit: {
                NewOrder order;
                order.id = command.order.id;
                order.account = command.order.account;
                order.instrument = command.order.instrument;
                order.side = command.order.side;
                order.type = command.order.type;
                order.tif = command.order.tif;
                order.price = command.order.price;
                order.quantity = command.order.quantity;
                order.post_only = command.order.post_only;
                engine.submit(order);
                break;
            }
            case Command::Kind::Cancel:
                engine.cancel(CancelOrder{command.target, AccountId{1}, InstrumentId{1}});
                break;
            case Command::Kind::Replace:
                engine.replace(ReplaceOrder{command.target, AccountId{1}, InstrumentId{1},
                                            command.new_price, command.new_quantity});
                break;
        }
    }

    writer.close();
    EXPECT_TRUE(writer.healthy()) << writer.last_error();
    return digest(engine);
}

std::uint64_t replay_digest(const std::filesystem::path& directory, journal::ReplayReport& report) {
    ManualClock clock;
    MatchingEngine engine{clock};
    journal::Replayer replayer(engine, clock);
    report = replayer.replay(directory);
    return report.state_digest;
}

// --- The digest itself -----------------------------------------------------

TEST(StateDigest, DistinguishesEveryFieldThatAffectsFutureBehaviour) {
    ManualClock clock;
    const auto build = [&clock](Price price, Quantity quantity, Side side) {
        auto engine = std::make_unique<MatchingEngine>(clock);
        engine->add_instrument(journal_instrument(1, "AAA", SelfTradePolicy::Allow));
        NewOrder order;
        order.id = OrderId{1};
        order.account = AccountId{1};
        order.instrument = InstrumentId{1};
        order.side = side;
        order.price = price;
        order.quantity = quantity;
        engine->submit(order);
        return engine;
    };

    const std::uint64_t base = digest(*build(100, 10, Side::Buy));
    EXPECT_NE(base, digest(*build(101, 10, Side::Buy))) << "price";
    EXPECT_NE(base, digest(*build(100, 11, Side::Buy))) << "quantity";
    EXPECT_NE(base, digest(*build(100, 10, Side::Sell))) << "side";
    EXPECT_EQ(base, digest(*build(100, 10, Side::Buy))) << "and is stable for identical state";
}

TEST(StateDigest, DistinguishesQueueOrderAtTheSamePrice) {
    ManualClock clock;
    const auto build = [&clock](bool reversed) {
        auto engine = std::make_unique<MatchingEngine>(clock);
        engine->add_instrument(journal_instrument(1, "AAA", SelfTradePolicy::Allow));
        const std::uint64_t ids[2] = {reversed ? 2u : 1u, reversed ? 1u : 2u};
        for (const std::uint64_t id : ids) {
            NewOrder order;
            order.id = OrderId{id};
            order.account = AccountId{1};
            order.instrument = InstrumentId{1};
            order.price = 100;
            order.quantity = 10;
            engine->submit(order);
        }
        return engine;
    };

    // Two books holding the same orders at the same price in a different queue
    // order will match differently on the next command, so the digest must
    // separate them.
    EXPECT_NE(digest(*build(false)), digest(*build(true)));
}

// --- Replay ----------------------------------------------------------------

TEST(Replay, ReproducesEngineStateExactly) {
    TempJournal temp;
    const std::uint64_t original = run_and_journal(temp.path(), 42, 3000, SelfTradePolicy::Allow);

    journal::ReplayReport report;
    const std::uint64_t replayed = replay_digest(temp.path(), report);

    EXPECT_EQ(report.recovery.outcome, journal::RecoveryOutcome::Clean);
    EXPECT_EQ(report.instruments_defined, 2u);
    EXPECT_GT(report.orders_submitted, 0u);
    EXPECT_EQ(replayed, original) << "replaying the journal must reconstruct the identical book";
}

TEST(Replay, ReproducesStateUnderEverySelfTradePolicy) {
    for (const SelfTradePolicy policy :
         {SelfTradePolicy::Allow, SelfTradePolicy::CancelIncoming, SelfTradePolicy::CancelResting,
          SelfTradePolicy::CancelBoth, SelfTradePolicy::DecrementBoth}) {
        TempJournal temp;
        const std::uint64_t original = run_and_journal(temp.path(), 7, 2000, policy);
        journal::ReplayReport report;
        EXPECT_EQ(replay_digest(temp.path(), report), original) << to_string(policy);
    }
}

TEST(Replay, ReproducesStateAcrossManySegments) {
    TempJournal temp;
    const std::uint64_t original =
        run_and_journal(temp.path(), 99, 2000, SelfTradePolicy::Allow, journal::Durability::None,
                        /*segment_bytes=*/2048);

    journal::JournalReader reader(temp.path());
    ASSERT_GT(reader.segments().size(), 5u) << "the run was expected to span many segments";

    journal::ReplayReport report;
    EXPECT_EQ(replay_digest(temp.path(), report), original);
}

TEST(Replay, IsItselfDeterministic) {
    TempJournal temp;
    run_and_journal(temp.path(), 3, 1500, SelfTradePolicy::CancelResting);

    journal::ReplayReport first;
    journal::ReplayReport second;
    const std::uint64_t a = replay_digest(temp.path(), first);
    const std::uint64_t b = replay_digest(temp.path(), second);

    // Two replays of one journal must agree with each other, not merely with
    // the original. If they can differ, the digest is reading something that
    // varies between runs -- hash iteration order being the usual culprit.
    EXPECT_EQ(a, b);
}

TEST(Replay, RebuildsInstrumentsFromTheJournalAlone) {
    TempJournal temp;
    run_and_journal(temp.path(), 5, 500, SelfTradePolicy::DecrementBoth);

    ManualClock clock;
    MatchingEngine engine{clock};
    journal::Replayer replayer(engine, clock);
    replayer.replay(temp.path());

    // Nothing configured this engine. Its instruments, tick sizes and
    // self-trade policy all came out of the log.
    ASSERT_NE(engine.find_instrument("AAA"), nullptr);
    ASSERT_NE(engine.find_instrument("BBB"), nullptr);
    EXPECT_EQ(engine.find_instrument("AAA")->self_trade_policy, SelfTradePolicy::DecrementBoth);
    EXPECT_EQ(engine.instruments().size(), 2u);
}

TEST(Replay, RecoversToTheLastCompleteCommandAfterATornTail) {
    TempJournal temp;
    run_and_journal(temp.path(), 11, 1000, SelfTradePolicy::Allow);

    const std::filesystem::path segment = temp.path() / journal::JournalWriter::segment_name(1);
    const auto size = std::filesystem::file_size(segment);
    std::string error;
    ASSERT_TRUE(journal::JournalReader::truncate_to(segment, size - 9, error)) << error;

    journal::ReplayReport report;
    replay_digest(temp.path(), report);

    // A crash mid-write loses the final command and nothing before it. The
    // reconstructed venue is a real state the venue actually passed through,
    // one command short of where it stopped.
    EXPECT_EQ(report.recovery.outcome, journal::RecoveryOutcome::TornTail);
    EXPECT_TRUE(report.recovery.usable());
    EXPECT_GT(report.orders_submitted, 0u);
}

TEST(Replay, ATornTailCostsExactlyTheLastCommand) {
    TempJournal temp;
    run_and_journal(temp.path(), 13, 400, SelfTradePolicy::Allow);

    journal::ReplayReport whole;
    replay_digest(temp.path(), whole);

    const std::filesystem::path segment = temp.path() / journal::JournalWriter::segment_name(1);
    std::string error;
    ASSERT_TRUE(
        journal::JournalReader::truncate_to(segment, whole.recovery.bytes_recovered - 1, error));

    journal::ReplayReport truncated;
    replay_digest(temp.path(), truncated);
    EXPECT_EQ(truncated.recovery.records_recovered, whole.recovery.records_recovered - 1);
}

TEST(Replay, ReportsAMissingSegmentAsASequenceGap) {
    TempJournal temp;
    run_and_journal(temp.path(), 31, 1200, SelfTradePolicy::Allow, journal::Durability::None,
                    /*segment_bytes=*/1024);

    journal::JournalReader reader(temp.path());
    const std::vector<std::filesystem::path> segments = reader.segments();
    ASSERT_GT(segments.size(), 4u);

    // Delete a segment from the middle: the shape of an archive that was never
    // copied, or a file lost between machines.
    std::filesystem::remove(segments[2]);

    journal::ReplayReport report;
    replay_digest(temp.path(), report);

    // Recovering the remainder would build a book that never existed -- the
    // surviving commands of a stream that had others in the middle.
    EXPECT_EQ(report.recovery.outcome, journal::RecoveryOutcome::SequenceGap);
    EXPECT_FALSE(report.recovery.usable());
    EXPECT_GT(report.recovery.expected_sequence, 0u);
    EXPECT_NE(report.recovery.message.find("missing"), std::string::npos);
}

TEST(Replay, ARestartedEngineContinuesTheNumberingWithoutAGap) {
    TempJournal temp;
    ManualClock clock;

    // First run.
    SeqNum last = 0;
    {
        journal::JournalWriter writer(journal::WriterConfig{.directory = temp.path()});
        ASSERT_TRUE(writer.open());
        MatchingEngine engine{clock};
        engine.set_journal(&writer);
        engine.add_instrument(journal_instrument(1, "AAA", SelfTradePolicy::Allow));
        for (std::uint64_t i = 1; i <= 20; ++i) {
            NewOrder o;
            o.id = OrderId{i};
            o.account = AccountId{1};
            o.instrument = InstrumentId{1};
            o.price = 100;
            o.quantity = 10;
            last = engine.submit(o).sequence;
        }
    }

    // Restart: recover, then resume numbering from where the journal ended.
    journal::ReplayReport recovered;
    {
        MatchingEngine engine{clock};
        journal::Replayer replayer(engine, clock);
        recovered = replayer.replay(temp.path());
        ASSERT_EQ(recovered.recovery.outcome, journal::RecoveryOutcome::Clean);
        ASSERT_EQ(recovered.recovery.last_sequence, last);
    }
    {
        journal::JournalWriter writer(journal::WriterConfig{.directory = temp.path()});
        ASSERT_TRUE(writer.open());
        MatchingEngine engine{clock};
        engine.set_journal(&writer);
        // The operational step that keeps the invariant across a restart.
        engine.restore_sequence(recovered.recovery.last_sequence);
        engine.add_instrument(journal_instrument(2, "BBB", SelfTradePolicy::Allow));
        NewOrder o;
        o.id = OrderId{999};
        o.account = AccountId{1};
        o.instrument = InstrumentId{2};
        o.price = 100;
        o.quantity = 10;
        engine.submit(o);
    }

    // The two runs' records form one gap-free stream, so the whole journal --
    // across the restart -- still reads clean.
    ManualClock replay_clock;
    MatchingEngine engine{replay_clock};
    journal::Replayer replayer(engine, replay_clock);
    const journal::ReplayReport report = replayer.replay(temp.path());
    EXPECT_EQ(report.recovery.outcome, journal::RecoveryOutcome::Clean);
    EXPECT_EQ(report.recovery.last_sequence, last + 2);
}

TEST(Replay, SurvivesDurableWritesUnchanged) {
    TempJournal temp;
    // The durability setting decides when bytes are persisted, never what they
    // are. A journal written with fsync on every record must replay to the same
    // state as one written without.
    const std::uint64_t original =
        run_and_journal(temp.path(), 21, 300, SelfTradePolicy::Allow, journal::Durability::Always);

    journal::ReplayReport report;
    EXPECT_EQ(replay_digest(temp.path(), report), original);
}

}  // namespace
}  // namespace xc
