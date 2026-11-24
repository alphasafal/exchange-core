// Resend safety: a client that lost an acknowledgement can send the same order
// again without it executing twice, and can ask where the order stands.
// See docs/adr/0004-increasing-order-ids.md.
#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "order_book_fixture.hpp"
#include "xc/core/engine_listener.hpp"
#include "xc/core/matching_engine.hpp"
#include "xc/core/state_digest.hpp"
#include "xc/journal/replayer.hpp"
#include "xc/journal/writer.hpp"

namespace xc {
namespace {

using testing::test_instrument;

Instrument resend_safe(bool increasing = true) {
    Instrument instrument = test_instrument();
    instrument.symbol = "AAA";
    instrument.self_trade_policy = SelfTradePolicy::Allow;
    instrument.increasing_order_ids = increasing;
    return instrument;
}

NewOrder new_order(std::uint64_t id, Side side, Price price, Quantity quantity,
                   std::uint64_t account = 1) {
    NewOrder command;
    command.id = OrderId{id};
    command.account = AccountId{account};
    command.instrument = InstrumentId{1};
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    return command;
}

// --- Resends ----------------------------------------------------------------

struct Venue {
    explicit Venue(bool increasing = true) { engine.add_instrument(resend_safe(increasing)); }
    SubmitOutcome send(std::uint64_t id, Side side, Price price, Quantity quantity,
                       std::uint64_t account = 1) {
        return engine.submit(new_order(id, side, price, quantity, account));
    }
    const OrderBook& book() const { return *engine.book(InstrumentId{1}); }
    OrderStatus status(std::uint64_t account, std::uint64_t id) const {
        return book().status(AccountId{account}, OrderId{id});
    }

    ManualClock clock;
    MatchingEngine engine{clock};
};

TEST(Resend, AResentFilledOrderDoesNotExecuteTwice) {
    // The failure this feature exists for. Order 2 fills completely and leaves
    // the book, so an index of resting orders no longer knows it existed. A
    // client that never saw the acknowledgement sends it again.
    Venue unsafe(false);
    unsafe.send(1, Side::Sell, 100, 20, 2);
    ASSERT_TRUE(unsafe.send(2, Side::Buy, 100, 10).accepted());
    ASSERT_TRUE(unsafe.send(2, Side::Buy, 100, 10).accepted())
        << "without increasing ids the resend trades again: the bug this fixes";
    EXPECT_EQ(unsafe.book().quantity_at(Side::Sell, 100), 0u);

    Venue safe;
    safe.send(1, Side::Sell, 100, 20, 2);
    ASSERT_TRUE(safe.send(2, Side::Buy, 100, 10).accepted());
    EXPECT_EQ(safe.send(2, Side::Buy, 100, 10).reject, RejectReason::DuplicateOrderId);
    EXPECT_EQ(safe.book().quantity_at(Side::Sell, 100), 10u);
}

TEST(Resend, IsRefusedWithoutBeingNumberedOrJournaled) {
    // Refused before the book, like every other refusal that changes nothing
    // (ADR 0001). A client resending a backlog after reconnecting must not
    // grow the journal by one record per resend.
    Venue venue;
    ASSERT_TRUE(venue.send(1, Side::Buy, 100, 10).accepted());
    const SeqNum before = venue.engine.sequence();
    const SubmitOutcome resent = venue.send(1, Side::Buy, 100, 10);
    EXPECT_EQ(resent.reject, RejectReason::DuplicateOrderId);
    EXPECT_EQ(resent.sequence, 0u);
    EXPECT_EQ(venue.engine.sequence(), before);
    EXPECT_EQ(venue.book().resting_order_count(), 1u);
}

TEST(Resend, IdsMustIncreasePerAccountAndAccountsAreIndependent) {
    Venue venue;
    ASSERT_TRUE(venue.send(10, Side::Buy, 100, 1, 1).accepted());
    EXPECT_EQ(venue.send(9, Side::Buy, 100, 1, 1).reject, RejectReason::DuplicateOrderId);
    EXPECT_TRUE(venue.send(11, Side::Buy, 100, 1, 1).accepted());
    // Another account starts from its own mark, so a low id is fine there.
    EXPECT_TRUE(venue.send(3, Side::Buy, 99, 1, 2).accepted());
}

TEST(Resend, ARefusedOrderCanBeSentAgain) {
    // Refused orders never executed, so they do not raise the mark: a client
    // retrying a post-only order once the book has moved must still be able to.
    Venue venue;
    venue.send(1, Side::Sell, 100, 10, 2);

    NewOrder post_only = new_order(7, Side::Buy, 100, 5);
    post_only.post_only = true;
    ASSERT_EQ(venue.engine.submit(post_only).reject, RejectReason::PostOnlyWouldCross);
    EXPECT_EQ(venue.status(1, 7).state, OrderState::Unknown);

    ASSERT_TRUE(
        venue.engine.cancel(CancelOrder{OrderId{1}, AccountId{2}, InstrumentId{1}}).accepted());
    EXPECT_TRUE(venue.engine.submit(post_only).accepted());
}

TEST(Resend, AnAmendmentKeepsItsIdAndIsNotMistakenForAResend) {
    // A price change re-queues the order carrying an id at the account's
    // mark. It is an amendment, not a new order, and must go through.
    Venue venue;
    ASSERT_TRUE(venue.send(4, Side::Buy, 100, 10).accepted());
    const ReplaceOutcome moved =
        venue.engine.replace(ReplaceOrder{OrderId{4}, AccountId{1}, InstrumentId{1}, 101, 10});
    ASSERT_TRUE(moved.accepted());
    EXPECT_EQ(venue.book().quantity_at(Side::Buy, 101), 10u);
}

// --- Order status -----------------------------------------------------------

TEST(OrderStatusQuery, ReportsWorkingWithFilledAndRemaining) {
    Venue venue;
    venue.send(1, Side::Buy, 100, 10);
    venue.send(2, Side::Sell, 100, 4, 2);
    const OrderStatus status = venue.status(1, 1);
    EXPECT_EQ(status.state, OrderState::Working);
    EXPECT_EQ(status.filled, 4u);
    EXPECT_EQ(status.remaining, 6u);
}

TEST(OrderStatusQuery, ReportsNotWorkingOnceAnOrderHasLeftTheBook) {
    Venue venue;
    venue.send(1, Side::Buy, 100, 10);
    venue.send(2, Side::Sell, 100, 10, 2);
    EXPECT_EQ(venue.status(1, 1).state, OrderState::NotWorking);
    EXPECT_EQ(venue.status(2, 2).state, OrderState::NotWorking);
    // An id above the mark has never been accepted.
    EXPECT_EQ(venue.status(1, 2).state, OrderState::Unknown);
}

TEST(OrderStatusQuery, NeverDescribesAnotherAccountsOrder) {
    // Same rule as cancel: telling one account that another's order exists
    // leaks the shape of the book to anyone willing to guess ids.
    Venue venue;
    venue.send(1, Side::Buy, 100, 10, 1);
    EXPECT_EQ(venue.status(2, 1).state, OrderState::Unknown);
}

TEST(OrderStatusQuery, CannotSayNotWorkingWithoutIncreasingIds) {
    Venue venue(false);
    venue.send(1, Side::Buy, 100, 10);
    venue.send(2, Side::Sell, 100, 10, 2);
    EXPECT_EQ(venue.status(1, 1).state, OrderState::Unknown);
}

// --- Replay -----------------------------------------------------------------

class TempJournal {
  public:
    TempJournal() {
        path_ = std::filesystem::temp_directory_path() /
                ("xc-resend-test-" + std::to_string(::getpid()) + "-" + std::to_string(++counter_));
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

TEST(Resend, TheMarksSurviveReplay) {
    // A resend after a restart is the real case, so the mark has to be rebuilt
    // from the journal. If it were not, a client resending a filled order to a
    // recovered venue would trade twice -- the exact failure, one crash later.
    TempJournal journal;
    std::uint64_t original = 0;
    {
        journal::JournalWriter writer(journal::WriterConfig{.directory = journal.path()});
        ASSERT_TRUE(writer.open()) << writer.last_error();
        ManualClock clock;
        MatchingEngine engine{clock};
        engine.set_journal(&writer);
        ASSERT_TRUE(engine.add_instrument(resend_safe()));
        engine.submit(new_order(1, Side::Sell, 100, 10, 2));
        ASSERT_TRUE(engine.submit(new_order(2, Side::Buy, 100, 10, 1)).accepted());
        original = digest(engine);
        writer.close();
    }

    ManualClock clock;
    MatchingEngine replayed{clock};
    journal::Replayer replayer(replayed, clock);
    const journal::ReplayReport report = replayer.replay(journal.path());
    EXPECT_EQ(report.state_digest, original);

    // Order 2 filled and left the book, so only the rebuilt mark can refuse it.
    ASSERT_EQ(replayed.book(InstrumentId{1})->find(OrderId{2}), nullptr);
    EXPECT_EQ(replayed.submit(new_order(2, Side::Buy, 100, 10, 1)).reject,
              RejectReason::DuplicateOrderId);
}

TEST(StateDigest, CoversTheOrderIdMarks) {
    // Two engines with identical books but different marks will accept
    // different futures, so their digests must differ.
    ManualClock clock;
    MatchingEngine a{clock};
    MatchingEngine b{clock};
    a.add_instrument(resend_safe());
    b.add_instrument(resend_safe());
    a.submit(new_order(1, Side::Buy, 100, 10));
    b.submit(new_order(5, Side::Buy, 100, 10));
    a.cancel(CancelOrder{OrderId{1}, AccountId{1}, InstrumentId{1}});
    b.cancel(CancelOrder{OrderId{5}, AccountId{1}, InstrumentId{1}});
    ASSERT_EQ(a.book(InstrumentId{1})->resting_order_count(), 0u);
    ASSERT_EQ(b.book(InstrumentId{1})->resting_order_count(), 0u);
    EXPECT_NE(digest(a), digest(b));
}

// --- A crash between the journal and the acknowledgement --------------------

// Dies the instant the engine reports an order accepted -- after the journal
// write, before anything could have been sent to the client.
class CrashOnAccept final : public EngineListener {
  public:
    explicit CrashOnAccept(OrderId target) : target_(target) {}
    void on_order_accepted(SeqNum, const Order& order) override {
        if (order.id == target_) {
            std::_Exit(0);  // no destructors, no flush: as close to SIGKILL as a test can get
        }
    }

  private:
    OrderId target_;
};

TEST(ResendDeathTest, CrashAfterJournalBeforeAckLeavesAResendSafeVenue) {
    // The case ADR 0002 left open. The child journals order 7 with
    // durability Always, then dies before the acknowledgement could go out.
    // The parent recovers from the journal and plays the client's side:
    // it never saw an ack, so it asks for status and then resends.
    //
    // The default "fast" death-test style forks, so the child writes to the
    // same directory the parent then reads. The "threadsafe" style re-executes
    // the binary, which would give the child a different pid and directory.
    TempJournal journal;

    EXPECT_EXIT(
        {
            journal::JournalWriter writer(journal::WriterConfig{
                .directory = journal.path(), .durability = journal::Durability::Always});
            if (!writer.open()) {
                std::_Exit(2);
            }
            ManualClock clock;
            MatchingEngine engine{clock};
            engine.set_journal(&writer);
            CrashOnAccept crash(OrderId{7});
            engine.add_listener(&crash);
            engine.add_instrument(resend_safe());
            engine.submit(new_order(7, Side::Buy, 100, 10));
            std::_Exit(3);  // unreachable if the listener fired
        },
        ::testing::ExitedWithCode(0), "");

    ManualClock clock;
    MatchingEngine recovered{clock};
    journal::Replayer replayer(recovered, clock);
    replayer.replay(journal.path());

    const OrderBook* book = recovered.book(InstrumentId{1});
    ASSERT_NE(book, nullptr) << "the instrument record did not survive the crash";
    const OrderStatus status = book->status(AccountId{1}, OrderId{7});
    EXPECT_EQ(status.state, OrderState::Working)
        << "the journal write happened before the crash, so the order must exist";
    EXPECT_EQ(status.remaining, 10u);

    EXPECT_EQ(recovered.submit(new_order(7, Side::Buy, 100, 10)).reject,
              RejectReason::DuplicateOrderId);
    EXPECT_EQ(book->quantity_at(Side::Buy, 100), 10u) << "the resend must not add a second order";
}

}  // namespace
}  // namespace xc
