#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "order_book_fixture.hpp"
#include "xc/core/matching_engine.hpp"

namespace xc {
namespace {

using testing::test_instrument;

/// Records every callback in the order it arrived, so tests can assert on the
/// sequence of events rather than only on final state.
class RecordingListener final : public EngineListener {
  public:
    struct Event {
        enum class Kind { Accepted, Rejected, Cancelled, Replaced, Fills } kind;
        SeqNum sequence = 0;
        OrderId order;
        RejectReason reject = RejectReason::None;
        std::size_t fill_count = 0;
    };

    void on_order_accepted(SeqNum seq, const Order& order) override {
        events.push_back({Event::Kind::Accepted, seq, order.id, RejectReason::None, 0});
    }
    void on_order_rejected(SeqNum seq, const NewOrder& command, RejectReason reason) override {
        events.push_back({Event::Kind::Rejected, seq, command.id, reason, 0});
    }
    void on_order_cancelled(SeqNum seq, const Order& order) override {
        events.push_back({Event::Kind::Cancelled, seq, order.id, RejectReason::None, 0});
    }
    void on_order_replaced(SeqNum seq, const Order& previous, const Order& amended) override {
        events.push_back({Event::Kind::Replaced, seq, amended.id, RejectReason::None, 0});
        previous_prices.push_back(previous.price);
        amended_prices.push_back(amended.price);
    }
    void on_fills(SeqNum seq, std::span<const Fill> fills) override {
        events.push_back({Event::Kind::Fills, seq, OrderId{}, RejectReason::None, fills.size()});
        for (const Fill& fill : fills) {
            fill_quantities.push_back(fill.quantity);
        }
    }

    std::vector<Event> events;
    std::vector<Price> previous_prices;
    std::vector<Price> amended_prices;
    std::vector<Quantity> fill_quantities;
};

Instrument instrument_named(std::uint64_t id, std::string symbol) {
    Instrument instrument = test_instrument();
    instrument.id = InstrumentId{id};
    instrument.symbol = std::move(symbol);
    return instrument;
}

class EngineTest : public ::testing::Test {
  protected:
    ManualClock clock{1'000'000};
    MatchingEngine engine{clock};
    RecordingListener listener;

    void SetUp() override {
        ASSERT_TRUE(engine.add_instrument(instrument_named(1, "AAA")));
        ASSERT_TRUE(engine.add_instrument(instrument_named(2, "BBB")));
        engine.add_listener(&listener);
    }

    NewOrder order(std::uint64_t id, std::uint64_t instrument, Side side, Price price,
                   Quantity quantity, std::uint64_t account = 1) {
        NewOrder command;
        command.id = OrderId{id};
        command.account = AccountId{account};
        command.instrument = InstrumentId{instrument};
        command.side = side;
        command.price = price;
        command.quantity = quantity;
        return command;
    }
};

// --- Instrument registry ---------------------------------------------------

TEST_F(EngineTest, RejectsDuplicateInstrumentIdsAndSymbols) {
    EXPECT_FALSE(engine.add_instrument(instrument_named(1, "CCC"))) << "id already taken";
    EXPECT_FALSE(engine.add_instrument(instrument_named(3, "AAA"))) << "symbol already taken";
    EXPECT_TRUE(engine.add_instrument(instrument_named(3, "CCC")));
}

TEST_F(EngineTest, RejectsAnInstrumentWithNoIdOrNoSymbol) {
    EXPECT_FALSE(engine.add_instrument(instrument_named(0, "DDD")));
    EXPECT_FALSE(engine.add_instrument(instrument_named(4, "")));
}

TEST_F(EngineTest, ResolvesInstrumentsByIdAndBySymbol) {
    ASSERT_NE(engine.find_instrument(InstrumentId{1}), nullptr);
    EXPECT_EQ(engine.find_instrument(InstrumentId{1})->symbol, "AAA");
    ASSERT_NE(engine.find_instrument("BBB"), nullptr);
    EXPECT_EQ(engine.find_instrument("BBB")->id, InstrumentId{2});
    EXPECT_EQ(engine.find_instrument("ZZZ"), nullptr);
    EXPECT_EQ(engine.find_instrument(InstrumentId{99}), nullptr);
}

// --- Routing ---------------------------------------------------------------

TEST_F(EngineTest, KeepsInstrumentsCompletelyIsolated) {
    engine.submit(order(1, 1, Side::Buy, 100, 10));
    engine.submit(order(2, 2, Side::Buy, 100, 10));

    // A crossing sell in one instrument must not touch the other, however
    // identical the prices look.
    const SubmitOutcome crossing = engine.submit(order(3, 1, Side::Sell, 100, 10));
    EXPECT_EQ(crossing.filled, 10u);

    EXPECT_EQ(engine.book(InstrumentId{1})->resting_order_count(), 0u);
    EXPECT_EQ(engine.book(InstrumentId{2})->resting_order_count(), 1u);
    EXPECT_EQ(engine.book(InstrumentId{2})->best_bid(), 100);
}

TEST_F(EngineTest, RejectsCommandsForAnUnknownInstrument) {
    EXPECT_EQ(engine.submit(order(1, 99, Side::Buy, 100, 10)).reject,
              RejectReason::UnknownInstrument);

    CancelOrder cancel{OrderId{1}, AccountId{1}, InstrumentId{99}};
    EXPECT_EQ(engine.cancel(cancel).reject, RejectReason::UnknownInstrument);
}

// --- Sequencing ------------------------------------------------------------

TEST_F(EngineTest, AssignsStrictlyIncreasingSequenceNumbers) {
    // Two instruments were registered in SetUp, and registering an instrument
    // is an event in the venue's total order like any other.
    const SeqNum base = engine.sequence();
    ASSERT_EQ(base, 2u);

    const SeqNum first = engine.submit(order(1, 1, Side::Buy, 100, 10)).sequence;
    const SeqNum second = engine.submit(order(2, 1, Side::Buy, 101, 10)).sequence;
    CancelOrder cancel{OrderId{1}, AccountId{1}, InstrumentId{1}};
    const SeqNum third = engine.cancel(cancel).sequence;

    EXPECT_EQ(first, base + 1);
    EXPECT_EQ(second, base + 2);
    EXPECT_EQ(third, base + 3);
    EXPECT_EQ(engine.sequence(), base + 3);
}

TEST_F(EngineTest, DoesNotNumberCommandsThatNeverReachTheBook) {
    const SeqNum accepted = engine.submit(order(1, 1, Side::Buy, 100, 10)).sequence;

    // Journal sequence numbers have to be gap-free, so that a gap can only ever
    // mean data loss. Numbering a command that was turned away before the book
    // would punch a hole in that numbering for a perfectly healthy reason, and
    // recovery could no longer tell a rejected command from a lost one.
    const SubmitOutcome rejected = engine.submit(order(2, 99, Side::Buy, 100, 10));
    EXPECT_EQ(rejected.reject, RejectReason::UnknownInstrument);
    EXPECT_EQ(rejected.sequence, 0u) << "no position in the stream was consumed";

    EXPECT_EQ(engine.submit(order(3, 1, Side::Buy, 100, 10)).sequence, accepted + 1);
}

TEST_F(EngineTest, NumbersRejectionsThatComeFromTheBookItself) {
    const SeqNum accepted = engine.submit(order(1, 1, Side::Buy, 100, 10)).sequence;

    // A duplicate order id is a decision of the matching logic, not a gate in
    // front of it. It is journaled and replayed like any other command, and
    // reproduces the same rejection.
    const SubmitOutcome duplicate = engine.submit(order(1, 1, Side::Buy, 101, 10));
    EXPECT_EQ(duplicate.reject, RejectReason::DuplicateOrderId);
    EXPECT_EQ(duplicate.sequence, accepted + 1);
}

TEST_F(EngineTest, StampsPriorityFromItsOwnSequenceAndClock) {
    clock.set(555'000);
    engine.submit(order(1, 1, Side::Buy, 100, 10));

    const Order* resting = engine.book(InstrumentId{1})->find(OrderId{1});
    ASSERT_NE(resting, nullptr);
    EXPECT_EQ(resting->sequence, 3u) << "after the two instrument registrations";
    EXPECT_EQ(resting->accepted_at, 555'000);

    clock.advance(1000);
    engine.submit(order(2, 1, Side::Buy, 100, 10));
    const Order* second = engine.book(InstrumentId{1})->find(OrderId{2});
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(second->sequence, 4u);
    EXPECT_EQ(second->accepted_at, 556'000);
}

// --- Ownership -------------------------------------------------------------

TEST_F(EngineTest, RefusesToCancelAnotherAccountsOrder) {
    engine.submit(order(1, 1, Side::Buy, 100, 10, /*account=*/1));

    CancelOrder cancel{OrderId{1}, AccountId{2}, InstrumentId{1}};
    const CancelOutcome outcome = engine.cancel(cancel);

    // Reported as UnknownOrder rather than as a permission failure. Confirming
    // that another account's order exists would let anyone map the book by
    // guessing ids.
    EXPECT_EQ(outcome.reject, RejectReason::UnknownOrder);
    EXPECT_EQ(engine.book(InstrumentId{1})->resting_order_count(), 1u);
}

TEST_F(EngineTest, RefusesToAmendAnotherAccountsOrder) {
    engine.submit(order(1, 1, Side::Buy, 100, 10, /*account=*/1));

    ReplaceOrder replace{OrderId{1}, AccountId{2}, InstrumentId{1}, 99, 5};
    EXPECT_EQ(engine.replace(replace).reject, RejectReason::UnknownOrder);
    EXPECT_EQ(engine.book(InstrumentId{1})->find(OrderId{1})->price, 100);
}

TEST_F(EngineTest, CancelsAndAmendsAnAccountsOwnOrder) {
    engine.submit(order(1, 1, Side::Buy, 100, 10, 1));

    ReplaceOrder replace{OrderId{1}, AccountId{1}, InstrumentId{1}, 99, 10};
    ASSERT_TRUE(engine.replace(replace).accepted());
    EXPECT_EQ(engine.book(InstrumentId{1})->find(OrderId{1})->price, 99);

    CancelOrder cancel{OrderId{1}, AccountId{1}, InstrumentId{1}};
    EXPECT_TRUE(engine.cancel(cancel).accepted());
    EXPECT_EQ(engine.book(InstrumentId{1})->resting_order_count(), 0u);
}

// --- Listener --------------------------------------------------------------

TEST_F(EngineTest, ReportsAcceptanceBeforeTheFillsItCaused) {
    engine.submit(order(1, 1, Side::Sell, 100, 10));
    const SeqNum crossing = engine.submit(order(2, 1, Side::Buy, 100, 10)).sequence;

    ASSERT_EQ(listener.events.size(), 3u);
    EXPECT_EQ(listener.events[0].kind, RecordingListener::Event::Kind::Accepted);
    EXPECT_EQ(listener.events[1].kind, RecordingListener::Event::Kind::Accepted);
    EXPECT_EQ(listener.events[2].kind, RecordingListener::Event::Kind::Fills);
    EXPECT_EQ(listener.events[2].sequence, crossing)
        << "fills are attributed to the command that caused them";
    EXPECT_EQ(listener.events[2].fill_count, 1u);
    EXPECT_EQ(listener.fill_quantities, (std::vector<Quantity>{10}));
}

TEST_F(EngineTest, ReportsRejectionsWithTheirReason) {
    engine.submit(order(1, 1, Side::Buy, 100, 10));
    engine.submit(order(1, 1, Side::Buy, 101, 10));

    ASSERT_EQ(listener.events.size(), 2u);
    EXPECT_EQ(listener.events[1].kind, RecordingListener::Event::Kind::Rejected);
    EXPECT_EQ(listener.events[1].reject, RejectReason::DuplicateOrderId);
    EXPECT_EQ(listener.events[1].order, OrderId{1});
}

TEST_F(EngineTest, ReportsAnAmendmentWithBothSidesOfIt) {
    engine.submit(order(1, 1, Side::Buy, 100, 10));
    ReplaceOrder replace{OrderId{1}, AccountId{1}, InstrumentId{1}, 98, 10};
    ASSERT_TRUE(engine.replace(replace).accepted());

    ASSERT_EQ(listener.previous_prices.size(), 1u);
    EXPECT_EQ(listener.previous_prices[0], 100);
    EXPECT_EQ(listener.amended_prices[0], 98);
}

TEST_F(EngineTest, ReportsAnAmendmentThatRemovedTheOrderAsACancellation) {
    engine.submit(order(1, 1, Side::Sell, 100, 10));
    engine.submit(order(2, 1, Side::Buy, 99, 10));
    listener.events.clear();

    // Amending the bid up to the offer fills it completely, so nothing is left
    // resting and there is no amended order to report.
    ReplaceOrder replace{OrderId{2}, AccountId{1}, InstrumentId{1}, 100, 10};
    ASSERT_TRUE(engine.replace(replace).accepted());

    ASSERT_GE(listener.events.size(), 1u);
    EXPECT_EQ(listener.events[0].kind, RecordingListener::Event::Kind::Cancelled);
    EXPECT_EQ(listener.events[0].order, OrderId{2});
}

TEST_F(EngineTest, DeliversToEveryRegisteredListener) {
    RecordingListener second;
    engine.add_listener(&second);
    engine.submit(order(1, 1, Side::Buy, 100, 10));

    EXPECT_EQ(listener.events.size(), 1u);
    EXPECT_EQ(second.events.size(), 1u);
}

TEST_F(EngineTest, ReusesItsFillBufferWithoutReallocating) {
    engine.submit(order(1, 1, Side::Sell, 100, 1000));
    engine.submit(order(2, 1, Side::Buy, 100, 10));
    ASSERT_FALSE(engine.last_fills().empty());
    const void* buffer = engine.last_fills().data();

    for (std::uint64_t i = 3; i < 200; ++i) {
        engine.submit(order(i, 1, Side::Buy, 100, 1));
    }
    // The buffer is cleared rather than reconstructed on every command, so a
    // venue running for a day does not allocate once per message.
    EXPECT_EQ(engine.last_fills().data(), buffer);
    EXPECT_EQ(engine.last_fills().size(), 1u);
}

TEST_F(EngineTest, ClearsTheFillBufferForACommandThatTradesNothing) {
    engine.submit(order(1, 1, Side::Sell, 100, 10));
    engine.submit(order(2, 1, Side::Buy, 100, 10));
    ASSERT_EQ(engine.last_fills().size(), 1u);

    // A stale fill left in the buffer would be republished as though it had
    // just happened.
    engine.submit(order(3, 1, Side::Buy, 90, 10));
    EXPECT_TRUE(engine.last_fills().empty());
}

}  // namespace
}  // namespace xc
