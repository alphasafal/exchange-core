#include <gtest/gtest.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "flow_generator.hpp"
#include "order_book_fixture.hpp"
#include "xc/core/order_book.hpp"

namespace xc {
namespace {

using testing::Command;
using testing::describe;
using testing::generate_flow;
using testing::test_instrument;

/// Properties that must hold after every command, whatever the command was.
///
/// The differential tests answer "does the book agree with the model?". These
/// answer "is the book self-consistent?", which is a different question: two
/// implementations can agree with each other and both be wrong, and a
/// structural corruption that has not yet changed an observable answer would
/// slip past a comparison but not past these.
class InvariantChecker {
  public:
    explicit InvariantChecker(const OrderBook& book) : book_(book) {}

    /// Records the fills produced by one command so that book state can be
    /// reconciled against the entire fill history.
    void observe(const std::vector<Fill>& fills) {
        for (const Fill& fill : fills) {
            EXPECT_GT(fill.quantity, 0u) << "a fill of nothing is not a fill";
            EXPECT_GT(fill.price, 0) << "fills must carry a real price";
            EXPECT_NE(fill.aggressor_order, fill.resting_order)
                << "an order cannot trade with itself";
            EXPECT_GT(fill.id.value(), last_trade_id_) << "trade ids must strictly increase";
            last_trade_id_ = fill.id.value();

            filled_by_order_[fill.aggressor_order] += fill.quantity;
            filled_by_order_[fill.resting_order] += fill.quantity;
            traded_ += fill.quantity;
        }
    }

    ::testing::AssertionResult check() const {
        std::vector<Order> resting;
        book_.for_each_resting_order([&](const Order& order) { resting.push_back(order); });

        // 1. Nothing resting is empty or over-filled. A zero-remaining order on
        //    the book is invisible to depth but still occupies queue position.
        for (const Order& order : resting) {
            if (order.remaining == 0) {
                return fail("resting order ", order, " has no remaining quantity");
            }
            if (order.remaining > order.quantity) {
                return fail("resting order ", order, " has remaining above its total");
            }
            if (!book_.instrument().is_valid_price(order.price)) {
                return fail("resting order ", order, " is off the tick grid");
            }
        }

        // 2. Bookkeeping agrees with reality: the index, the slab and the
        //    traversal must all count the same orders.
        if (resting.size() != book_.resting_order_count()) {
            return ::testing::AssertionFailure()
                   << "traversal found " << resting.size() << " orders but the index holds "
                   << book_.resting_order_count();
        }
        if (book_.pool().live() != book_.resting_order_count()) {
            return ::testing::AssertionFailure()
                   << "slab holds " << book_.pool().live() << " live nodes for "
                   << book_.resting_order_count() << " resting orders -- a node leaked";
        }
        for (const Order& order : resting) {
            const Order* looked_up = book_.find(order.id);
            if (looked_up == nullptr) {
                return fail("resting order ", order, " is not reachable through the index");
            }
            if (looked_up->remaining != order.remaining || looked_up->price != order.price) {
                return fail("index disagrees with the level for order ", order, "");
            }
        }

        // 3. The book is not crossed. A resting bid at or above a resting offer
        //    is liquidity that should already have traded.
        const std::optional<Price> bid = book_.best_bid();
        const std::optional<Price> ask = book_.best_ask();
        if (bid.has_value() && ask.has_value() && *bid >= *ask) {
            return ::testing::AssertionFailure()
                   << "book is crossed: bid " << *bid << " >= ask " << *ask;
        }

        // 4. Aggregated depth is exactly the sum of the orders behind it.
        //    Level totals are maintained incrementally, so this is where a
        //    missed reduce() or a stale unlink would surface.
        std::map<std::pair<int, Price>, DepthLevel> expected;
        for (const Order& order : resting) {
            DepthLevel& level = expected[{static_cast<int>(order.side), order.price}];
            level.price = order.price;
            level.quantity += order.remaining;
            ++level.order_count;
        }

        DepthSnapshot snapshot;
        book_.depth(1024, snapshot);
        for (const auto& [side, levels] :
             {std::pair{Side::Buy, &snapshot.bids}, std::pair{Side::Sell, &snapshot.asks}}) {
            for (const DepthLevel& level : *levels) {
                const auto it = expected.find({static_cast<int>(side), level.price});
                if (it == expected.end()) {
                    return ::testing::AssertionFailure()
                           << "depth reports a level at " << level.price
                           << " with no orders behind it";
                }
                if (it->second.quantity != level.quantity ||
                    it->second.order_count != level.order_count) {
                    return ::testing::AssertionFailure()
                           << "level " << level.price << " reports " << level.quantity << "x"
                           << level.order_count << " but holds " << it->second.quantity << "x"
                           << it->second.order_count;
                }
            }
        }
        if (snapshot.bids.size() + snapshot.asks.size() != expected.size()) {
            return ::testing::AssertionFailure()
                   << "depth reports " << snapshot.bids.size() + snapshot.asks.size()
                   << " levels but the orders form " << expected.size();
        }

        // 5. Depth is strictly ordered and strictly positive. Two entries at
        //    one price would mean a level was split.
        for (std::size_t i = 1; i < snapshot.bids.size(); ++i) {
            if (snapshot.bids[i].price >= snapshot.bids[i - 1].price) {
                return ::testing::AssertionFailure() << "bids are not strictly descending";
            }
        }
        for (std::size_t i = 1; i < snapshot.asks.size(); ++i) {
            if (snapshot.asks[i].price <= snapshot.asks[i - 1].price) {
                return ::testing::AssertionFailure() << "asks are not strictly ascending";
            }
        }

        // 6. Every resting order's filled quantity equals what the fill stream
        //    says it traded. This is the invariant that ties published
        //    executions to book state -- if they can drift apart, a client's
        //    position and the venue's disagree.
        for (const Order& order : resting) {
            const auto it = filled_by_order_.find(order.id);
            const Quantity from_fills = it == filled_by_order_.end() ? 0 : it->second;
            if (from_fills != order.filled()) {
                return fail("order ", order,
                            " has filled " + std::to_string(order.filled()) +
                                " but the fill stream reports " + std::to_string(from_fills));
            }
        }

        return ::testing::AssertionSuccess();
    }

    /// Total quantity that changed hands, counted once per trade.
    Quantity traded() const { return traded_; }

  private:
    template<typename... Rest>
    static ::testing::AssertionResult fail(const char* prefix, const Order& order,
                                           const std::string& suffix) {
        return ::testing::AssertionFailure()
               << prefix << "{id=" << order.id.value() << " px=" << order.price
               << " qty=" << order.quantity << " rem=" << order.remaining << "} " << suffix;
    }
    static ::testing::AssertionResult fail(const char* prefix, const Order& order,
                                           const char* suffix) {
        return fail(prefix, order, std::string(suffix));
    }

    const OrderBook& book_;
    std::unordered_map<OrderId, Quantity> filled_by_order_;
    Quantity traded_ = 0;
    std::uint64_t last_trade_id_ = 0;
};

void run_invariants(std::uint64_t seed, SelfTradePolicy policy, std::size_t command_count) {
    Instrument instrument = test_instrument();
    instrument.self_trade_policy = policy;

    OrderBook book{instrument};
    InvariantChecker checker{book};

    const std::vector<Command> stream = generate_flow(seed, command_count);
    Quantity submitted = 0;

    for (std::size_t i = 0; i < stream.size(); ++i) {
        const Command& command = stream[i];
        std::vector<Fill> fills;

        switch (command.kind) {
            case Command::Kind::Submit:
                if (book.submit(command.order, fills).accepted()) {
                    submitted += command.order.quantity;
                }
                break;
            case Command::Kind::Cancel:
                book.cancel(command.target);
                break;
            case Command::Kind::Replace:
                book.replace(command.target, command.new_price, command.new_quantity,
                             command.sequence, static_cast<Nanos>(command.sequence) * 1000, fills);
                break;
        }

        checker.observe(fills);
        ASSERT_TRUE(checker.check()) << "seed=" << seed << " policy=" << to_string(policy)
                                     << " step=" << i << " command: " << describe(command);
    }

    // A stream that never traded would satisfy every invariant above while
    // testing nothing, so assert the workload was actually interesting.
    EXPECT_GT(checker.traded(), 0u) << "the generated flow produced no trades at all";
    EXPECT_GT(submitted, 0u);
}

class InvariantTest : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(InvariantTest, HoldAcrossRandomFlowWithoutPrevention) {
    run_invariants(GetParam(), SelfTradePolicy::Allow, 3000);
}

TEST_P(InvariantTest, HoldAcrossRandomFlowUnderCancelIncoming) {
    run_invariants(GetParam(), SelfTradePolicy::CancelIncoming, 3000);
}

TEST_P(InvariantTest, HoldAcrossRandomFlowUnderCancelResting) {
    run_invariants(GetParam(), SelfTradePolicy::CancelResting, 3000);
}

TEST_P(InvariantTest, HoldAcrossRandomFlowUnderDecrementBoth) {
    run_invariants(GetParam(), SelfTradePolicy::DecrementBoth, 3000);
}

INSTANTIATE_TEST_SUITE_P(Seeds, InvariantTest, ::testing::Values(1, 7, 23, 101, 555, 4096, 777777));

}  // namespace
}  // namespace xc
