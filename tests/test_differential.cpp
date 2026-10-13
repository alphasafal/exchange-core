#include <gtest/gtest.h>

#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "flow_generator.hpp"
#include "model/compare.hpp"
#include "model/reference_book.hpp"
#include "order_book_fixture.hpp"
#include "xc/core/order_book.hpp"

namespace xc {
namespace {

using testing::Command;
using testing::describe;
using testing::generate_flow;
using testing::test_instrument;

/// Thin gtest wrappers over the shared comparators in model/compare.hpp. The
/// comparison itself lives there because fuzz_order_flow drives it too, and a
/// fuzzer checking a different notion of "the same" from the test it extends
/// could pass while this one would have failed.
::testing::AssertionResult same_fills(const std::vector<Fill>& actual,
                                      const std::vector<Fill>& expected) {
    const std::string difference = model::diff_fills(actual, expected);
    if (difference.empty()) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << difference;
}

::testing::AssertionResult same_book(const OrderBook& book, const model::ReferenceBook& model) {
    const std::string difference = model::diff_books(book, model);
    if (difference.empty()) {
        return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure() << difference;
}

void run_stream(std::uint64_t seed, SelfTradePolicy policy, std::size_t commands) {
    Instrument instrument = test_instrument();
    instrument.self_trade_policy = policy;

    OrderBook book{instrument};
    model::ReferenceBook reference{instrument};

    const std::vector<Command> stream = generate_flow(seed, commands);
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
