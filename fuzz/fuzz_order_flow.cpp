/// Fuzzes order flow against the reference model.
///
/// This is the differential test from test_differential.cpp with the seeded
/// generator removed: libFuzzer chooses the command stream, guided by the
/// coverage it reaches, instead of an RNG choosing it blindly. The oracle is
/// the same O(N**2) reference matcher and the same comparators, from
/// model/compare.hpp, so a divergence found here is a divergence that test
/// would have reported had it happened to generate the stream.
///
/// The fuzzer's bytes drive each field directly rather than seeding an RNG. A
/// seed-driven generator gives coverage feedback nothing to work with: flipping
/// a bit of the seed changes the entire stream, so libFuzzer cannot tell which
/// edit got it closer to new code. Reading fields straight from the input means
/// one byte moves one decision, which is what makes the guidance useful.
///
/// The price band is narrow and the account set small on purpose, for the same
/// reason the seeded generator does it: orders spread uniformly over a wide
/// range rarely cross and never self-trade, which leaves most of the logic
/// worth testing untouched.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "model/compare.hpp"
#include "model/reference_book.hpp"
#include "xc/core/instrument.hpp"
#include "xc/core/order.hpp"
#include "xc/core/order_book.hpp"
#include "xc/core/types.hpp"

namespace {

using xc::model::ReferenceBook;

/// Pulls decisions out of the fuzzer's input.
///
/// Runs dry rather than wrapping: once the bytes are gone the stream ends,
/// instead of looping over the same input and generating commands the fuzzer
/// did not choose and cannot shrink.
class Input {
  public:
    Input(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    bool empty() const { return offset_ >= size_; }

    std::uint8_t byte() {
        if (offset_ >= size_) {
            return 0;
        }
        return data_[offset_++];
    }

    /// Uniform-ish in [low, high]. The modulo bias is irrelevant here: this
    /// chooses what to try next, not a statistical sample.
    int range(int low, int high) {
        const int span = high - low + 1;
        return low + static_cast<int>(byte() % static_cast<unsigned>(span));
    }

  private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t offset_ = 0;
};

void fail(const std::string& what) {
    std::fputs(what.c_str(), stderr);
    std::fputc('\n', stderr);
    std::abort();
}

xc::Instrument instrument_for(xc::SelfTradePolicy policy) {
    xc::Instrument instrument;
    instrument.id = xc::InstrumentId{1};
    instrument.symbol = "FUZZ";
    instrument.tick_size = 1;
    instrument.lot_size = 1;
    instrument.min_quantity = 1;
    instrument.self_trade_policy = policy;
    return instrument;
}

xc::SelfTradePolicy policy_from(int choice) {
    switch (choice) {
        case 0:
            return xc::SelfTradePolicy::Allow;
        case 1:
            return xc::SelfTradePolicy::CancelIncoming;
        case 2:
            return xc::SelfTradePolicy::CancelResting;
        case 3:
            return xc::SelfTradePolicy::CancelBoth;
        default:
            return xc::SelfTradePolicy::DecrementBoth;
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    // Below this there is not enough input to pick a policy and run a command,
    // and above it the O(N**2) model dominates the iteration time without
    // reaching states a shorter stream cannot.
    if (size < 2 || size > 4096) {
        return 0;
    }

    Input input(data, size);
    const xc::SelfTradePolicy policy = policy_from(input.range(0, 4));
    const xc::Instrument instrument = instrument_for(policy);

    xc::OrderBook book{instrument};
    ReferenceBook model{instrument};

    std::vector<xc::OrderId> issued;
    std::uint64_t next_id = 1;
    xc::SeqNum sequence = 0;

    while (!input.empty()) {
        ++sequence;
        const xc::Nanos now = static_cast<xc::Nanos>(sequence) * 1000;

        std::vector<xc::Fill> engine_fills;
        std::vector<xc::Fill> model_fills;
        xc::RejectReason engine_reject = xc::RejectReason::None;
        xc::RejectReason model_reject = xc::RejectReason::None;
        xc::Quantity engine_filled = 0;
        xc::Quantity model_filled = 0;
        xc::Quantity engine_stp = 0;
        xc::Quantity model_stp = 0;
        std::string what;

        const int roll = input.range(0, 99);

        if (roll < 70 || issued.empty()) {
            xc::Order order;
            order.id = xc::OrderId{next_id++};
            order.account = xc::AccountId{static_cast<std::uint64_t>(input.range(1, 3))};
            order.instrument = instrument.id;
            order.side = input.range(0, 1) == 0 ? xc::Side::Buy : xc::Side::Sell;
            order.type = input.range(0, 9) == 0 ? xc::OrderType::Market : xc::OrderType::Limit;
            switch (input.range(0, 9)) {
                case 0:
                    order.tif = xc::TimeInForce::ImmediateOrCancel;
                    break;
                case 1:
                    order.tif = xc::TimeInForce::FillOrKill;
                    break;
                case 2:
                    order.tif = xc::TimeInForce::GoodTilCancelled;
                    break;
                default:
                    order.tif = xc::TimeInForce::Day;
                    break;
            }
            order.price = static_cast<xc::Price>(input.range(95, 105));
            order.quantity = static_cast<xc::Quantity>(input.range(1, 20));
            order.remaining = order.quantity;
            order.post_only = input.range(0, 9) == 0;
            order.sequence = sequence;
            order.accepted_at = now;

            const xc::SubmitResult a = book.submit(order, engine_fills);
            const xc::SubmitResult e = model.submit(order, model_fills);
            engine_reject = a.reject;
            model_reject = e.reject;
            engine_filled = a.filled;
            model_filled = e.filled;
            engine_stp = a.stp_cancelled;
            model_stp = e.stp_cancelled;
            what = "submit id=" + std::to_string(order.id.value());
            issued.push_back(order.id);
        } else if (roll < 85) {
            const xc::OrderId target = issued[static_cast<std::size_t>(
                input.range(0, static_cast<int>(issued.size()) - 1))];
            engine_reject = book.cancel(target).reject;
            model_reject = model.cancel(target).reject;
            what = "cancel id=" + std::to_string(target.value());
        } else {
            const xc::OrderId target = issued[static_cast<std::size_t>(
                input.range(0, static_cast<int>(issued.size()) - 1))];
            const auto price = static_cast<xc::Price>(input.range(95, 105));
            const auto quantity = static_cast<xc::Quantity>(input.range(1, 20));
            const xc::ReplaceResult a =
                book.replace(target, price, quantity, sequence, now, engine_fills);
            const xc::ReplaceResult e =
                model.replace(target, price, quantity, sequence, now, model_fills);
            engine_reject = a.reject;
            model_reject = e.reject;
            engine_filled = a.filled;
            model_filled = e.filled;
            engine_stp = a.stp_cancelled;
            model_stp = e.stp_cancelled;
            what = "replace id=" + std::to_string(target.value());
        }

        const std::string context = " [policy=" + std::string(xc::to_string(policy)) +
                                    " step=" + std::to_string(sequence) + " " + what + "]";

        if (engine_reject != model_reject) {
            fail("reject reason differs: engine " + std::string(xc::to_string(engine_reject)) +
                 " vs model " + std::string(xc::to_string(model_reject)) + context);
        }
        if (engine_filled != model_filled) {
            fail("filled quantity differs: engine " + std::to_string(engine_filled) + " vs model " +
                 std::to_string(model_filled) + context);
        }
        if (engine_stp != model_stp) {
            fail("self-trade-cancelled quantity differs: engine " + std::to_string(engine_stp) +
                 " vs model " + std::to_string(model_stp) + context);
        }
        if (const std::string difference = xc::model::diff_fills(engine_fills, model_fills);
            !difference.empty()) {
            fail(difference + context);
        }
        if (const std::string difference = xc::model::diff_books(book, model);
            !difference.empty()) {
            fail(difference + context);
        }
    }

    return 0;
}
