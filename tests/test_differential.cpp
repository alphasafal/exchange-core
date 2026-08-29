#include <gtest/gtest.h>

#include <random>
#include <sstream>
#include <vector>

#include "model/reference_book.hpp"
#include "order_book_fixture.hpp"
#include "xc/core/order_book.hpp"

namespace xc {
namespace {

using testing::test_instrument;

// One command in a generated stream. Kept as a plain tagged struct rather than
// a variant so a failing case can be printed verbatim and pasted into a
// regression test.
struct Command {
    enum class Kind { Submit, Cancel, Replace } kind = Kind::Submit;
    Order order;
    OrderId target;
    Price new_price = kNoPrice;
    Quantity new_quantity = 0;
    SeqNum sequence = 0;
};

std::string describe(const Command& command) {
    std::ostringstream out;
    switch (command.kind) {
        case Command::Kind::Submit:
            out << "submit id=" << command.order.id.value()
                << " acct=" << command.order.account.value() << ' ' << to_string(command.order.side)
                << ' ' << to_string(command.order.type) << ' ' << to_string(command.order.tif)
                << " px=" << command.order.price << " qty=" << command.order.quantity
                << (command.order.post_only ? " post_only" : "");
            break;
        case Command::Kind::Cancel:
            out << "cancel id=" << command.target.value();
            break;
        case Command::Kind::Replace:
            out << "replace id=" << command.target.value() << " px=" << command.new_price
                << " qty=" << command.new_quantity;
            break;
    }
    return out.str();
}

/// Generates a stream of commands from a seed.
///
/// The price band is deliberately narrow and the account set deliberately small
/// so that orders actually collide: a generator that spreads uniformly over a
/// wide range produces a book that almost never crosses and almost never
/// self-trades, and tests nothing interesting.
std::vector<Command> generate(std::uint64_t seed, std::size_t count) {
    std::mt19937_64 rng(seed);
    auto pick = [&rng](int low, int high) {
        return std::uniform_int_distribution<int>(low, high)(rng);
    };

    std::vector<Command> commands;
    commands.reserve(count);
    std::vector<OrderId> issued;
    std::uint64_t next_id = 1;
    SeqNum sequence = 0;

    for (std::size_t i = 0; i < count; ++i) {
        Command command;
        command.sequence = ++sequence;

        const int roll = pick(0, 99);
        if (roll < 60 || issued.empty()) {
            command.kind = Command::Kind::Submit;
            Order& order = command.order;
            order.id = OrderId{next_id++};
            order.account = AccountId{static_cast<std::uint64_t>(pick(1, 3))};
            order.instrument = InstrumentId{1};
            order.side = pick(0, 1) == 0 ? Side::Buy : Side::Sell;
            order.quantity = static_cast<Quantity>(pick(1, 100));
            order.remaining = order.quantity;
            order.sequence = command.sequence;
            order.accepted_at = static_cast<Nanos>(command.sequence) * 1000;

            const int type_roll = pick(0, 99);
            if (type_roll < 85) {
                order.type = OrderType::Limit;
                order.price = pick(95, 105);
            } else {
                order.type = OrderType::Market;
                order.price = kNoPrice;
            }

            const int tif_roll = pick(0, 99);
            if (tif_roll < 70) {
                order.tif = TimeInForce::Day;
            } else if (tif_roll < 85) {
                order.tif = TimeInForce::ImmediateOrCancel;
            } else if (tif_roll < 95) {
                order.tif = TimeInForce::FillOrKill;
            } else {
                order.tif = TimeInForce::GoodTilCancelled;
            }

            order.post_only = order.type == OrderType::Limit && pick(0, 99) < 10;
            issued.push_back(order.id);
        } else if (roll < 85) {
            command.kind = Command::Kind::Cancel;
            // Deliberately drawn from every id ever issued, not only live ones,
            // so the unknown-order path is exercised too.
            command.target =
                issued[static_cast<std::size_t>(pick(0, static_cast<int>(issued.size()) - 1))];
        } else {
            command.kind = Command::Kind::Replace;
            command.target =
                issued[static_cast<std::size_t>(pick(0, static_cast<int>(issued.size()) - 1))];
            command.new_price = pick(95, 105);
            command.new_quantity = static_cast<Quantity>(pick(1, 120));
        }
        commands.push_back(command);
    }
    return commands;
}

::testing::AssertionResult same_fills(const std::vector<Fill>& actual,
                                      const std::vector<Fill>& expected) {
    if (actual.size() != expected.size()) {
        return ::testing::AssertionFailure()
               << "fill count " << actual.size() << " != " << expected.size();
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        const Fill& a = actual[i];
        const Fill& e = expected[i];
        if (a.price != e.price || a.quantity != e.quantity ||
            a.aggressor_order != e.aggressor_order || a.resting_order != e.resting_order ||
            a.aggressor_side != e.aggressor_side || a.resting_filled != e.resting_filled) {
            return ::testing::AssertionFailure()
                   << "fill " << i << " differs: engine {px=" << a.price << " qty=" << a.quantity
                   << " agg=" << a.aggressor_order.value() << " rest=" << a.resting_order.value()
                   << "} vs model {px=" << e.price << " qty=" << e.quantity
                   << " agg=" << e.aggressor_order.value() << " rest=" << e.resting_order.value()
                   << "}";
        }
    }
    return ::testing::AssertionSuccess();
}

::testing::AssertionResult same_book(const OrderBook& book, const model::ReferenceBook& model) {
    std::vector<Order> engine_orders;
    book.for_each_resting_order([&](const Order& order) { engine_orders.push_back(order); });
    const std::vector<Order> model_orders = model.resting_orders();

    if (engine_orders.size() != model_orders.size()) {
        return ::testing::AssertionFailure()
               << "resting order count " << engine_orders.size() << " != " << model_orders.size();
    }
    for (std::size_t i = 0; i < engine_orders.size(); ++i) {
        const Order& a = engine_orders[i];
        const Order& e = model_orders[i];
        if (a.id != e.id || a.price != e.price || a.remaining != e.remaining ||
            a.quantity != e.quantity || a.side != e.side || a.sequence != e.sequence) {
            return ::testing::AssertionFailure()
                   << "resting order " << i << " differs: engine {id=" << a.id.value()
                   << " px=" << a.price << " rem=" << a.remaining << " qty=" << a.quantity
                   << " seq=" << a.sequence << "} vs model {id=" << e.id.value()
                   << " px=" << e.price << " rem=" << e.remaining << " qty=" << e.quantity
                   << " seq=" << e.sequence << "}";
        }
    }

    const TopOfBook engine_top = book.top_of_book();
    const TopOfBook model_top = model.top_of_book();
    if (engine_top.bid_price != model_top.bid_price ||
        engine_top.ask_price != model_top.ask_price ||
        engine_top.bid_quantity != model_top.bid_quantity ||
        engine_top.ask_quantity != model_top.ask_quantity) {
        return ::testing::AssertionFailure()
               << "top of book differs: engine {" << engine_top.bid_price << "x"
               << engine_top.bid_quantity << " / " << engine_top.ask_price << "x"
               << engine_top.ask_quantity << "} vs model {" << model_top.bid_price << "x"
               << model_top.bid_quantity << " / " << model_top.ask_price << "x"
               << model_top.ask_quantity << "}";
    }

    DepthSnapshot engine_depth;
    DepthSnapshot model_depth;
    book.depth(10, engine_depth);
    model.depth(10, model_depth);
    if (engine_depth.bids != model_depth.bids || engine_depth.asks != model_depth.asks) {
        return ::testing::AssertionFailure() << "aggregated depth differs";
    }

    return ::testing::AssertionSuccess();
}

void run_stream(std::uint64_t seed, SelfTradePolicy policy, std::size_t commands) {
    Instrument instrument = test_instrument();
    instrument.self_trade_policy = policy;

    OrderBook book{instrument};
    model::ReferenceBook reference{instrument};

    const std::vector<Command> stream = generate(seed, commands);
    for (std::size_t i = 0; i < stream.size(); ++i) {
        const Command& command = stream[i];
        std::vector<Fill> engine_fills;
        std::vector<Fill> model_fills;

        RejectReason engine_reject = RejectReason::None;
        RejectReason model_reject = RejectReason::None;
        Quantity engine_filled = 0;
        Quantity model_filled = 0;
        Quantity engine_stp = 0;
        Quantity model_stp = 0;

        switch (command.kind) {
            case Command::Kind::Submit: {
                const SubmitResult a = book.submit(command.order, engine_fills);
                const SubmitResult e = reference.submit(command.order, model_fills);
                engine_reject = a.reject;
                model_reject = e.reject;
                engine_filled = a.filled;
                model_filled = e.filled;
                engine_stp = a.stp_cancelled;
                model_stp = e.stp_cancelled;
                break;
            }
            case Command::Kind::Cancel: {
                engine_reject = book.cancel(command.target).reject;
                model_reject = reference.cancel(command.target).reject;
                break;
            }
            case Command::Kind::Replace: {
                const ReplaceResult a = book.replace(
                    command.target, command.new_price, command.new_quantity, command.sequence,
                    static_cast<Nanos>(command.sequence) * 1000, engine_fills);
                const ReplaceResult e = reference.replace(
                    command.target, command.new_price, command.new_quantity, command.sequence,
                    static_cast<Nanos>(command.sequence) * 1000, model_fills);
                engine_reject = a.reject;
                model_reject = e.reject;
                engine_filled = a.filled;
                model_filled = e.filled;
                engine_stp = a.stp_cancelled;
                model_stp = e.stp_cancelled;
                break;
            }
        }

        const std::string context = "seed=" + std::to_string(seed) +
                                    " policy=" + std::string(to_string(policy)) +
                                    " step=" + std::to_string(i) + " command: " + describe(command);

        ASSERT_EQ(engine_reject, model_reject) << context;
        ASSERT_EQ(engine_filled, model_filled) << context;
        ASSERT_EQ(engine_stp, model_stp) << context;
        ASSERT_TRUE(same_fills(engine_fills, model_fills)) << context;
        ASSERT_TRUE(same_book(book, reference)) << context;
    }
}

class DifferentialTest : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(DifferentialTest, MatchesTheReferenceModelWithoutSelfTradePrevention) {
    run_stream(GetParam(), SelfTradePolicy::Allow, 2000);
}

TEST_P(DifferentialTest, MatchesTheReferenceModelUnderCancelIncoming) {
    run_stream(GetParam(), SelfTradePolicy::CancelIncoming, 2000);
}

TEST_P(DifferentialTest, MatchesTheReferenceModelUnderCancelResting) {
    run_stream(GetParam(), SelfTradePolicy::CancelResting, 2000);
}

TEST_P(DifferentialTest, MatchesTheReferenceModelUnderCancelBoth) {
    run_stream(GetParam(), SelfTradePolicy::CancelBoth, 2000);
}

TEST_P(DifferentialTest, MatchesTheReferenceModelUnderDecrementBoth) {
    run_stream(GetParam(), SelfTradePolicy::DecrementBoth, 2000);
}

// Fixed seeds rather than a random one per run. A test that picks its own seed
// finds a bug on Tuesday and cannot reproduce it on Wednesday; the fuzz targets
// added later are where unbounded exploration belongs.
INSTANTIATE_TEST_SUITE_P(Seeds, DifferentialTest,
                         ::testing::Values(1, 2, 3, 17, 42, 99, 12345, 987654321));

}  // namespace
}  // namespace xc
