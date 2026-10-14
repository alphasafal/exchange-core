/// Emscripten bindings that run the real engine in a browser.
///
/// This is not a reimplementation or a simulation of the matching engine for
/// the web. It is xc_core, compiled to WebAssembly: the same OrderBook, the
/// same MatchingEngine, the same RiskEngine and KillSwitch that the native
/// binaries and the test suite use. If the book is wrong here it is wrong
/// there.
///
/// **What this cannot measure is latency.** Browsers deliberately coarsen
/// their clocks to defeat timing side channels -- performance.now() typically
/// resolves to somewhere between 100 microseconds and a millisecond, against
/// operations that take tens of nanoseconds. Per-operation percentiles are
/// therefore not merely noisy here, they are unmeasurable, and this bridge
/// does not pretend otherwise: it times a whole batch between two clock reads
/// and divides, which the resolution does support. Native p50/p99/p99.9 are in
/// docs/benchmarks.md, measured with a timer that can see them.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "xc/core/clock.hpp"
#include "xc/core/commands.hpp"
#include "xc/core/depth.hpp"
#include "xc/core/engine_listener.hpp"
#include "xc/core/matching_engine.hpp"
#include "xc/core/order_book.hpp"
#include "xc/risk/kill_switch.hpp"
#include "xc/risk/risk_engine.hpp"
#include "xc/util/build_info.hpp"

namespace {

using emscripten::val;

/// xorshift64*, so the flow is reproducible from a seed and costs a few
/// instructions. The demo is driving a matching engine, not testing an RNG.
class Rng {
  public:
    explicit Rng(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ULL : seed) {}

    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545F4914F6CDD1DULL;
    }

    int range(int low, int high) {
        const auto span = static_cast<std::uint64_t>(high - low + 1);
        return low + static_cast<int>(next() % span);
    }

  private:
    std::uint64_t state_;
};

struct Trade {
    double price;
    double quantity;
    bool aggressor_buy;
    std::uint64_t sequence;
};

/// Collects what the engine reports so the page can show a tape rather than
/// only a book. Bounded on purpose: an unbounded tape in a demo that runs for
/// an hour is a memory leak with a nice colour scheme.
class Tape final : public xc::EngineListener {
  public:
    void on_fills(xc::SeqNum sequence, std::span<const xc::Fill> fills) override {
        for (const xc::Fill& fill : fills) {
            trades_.push_back(Trade{static_cast<double>(fill.price),
                                    static_cast<double>(fill.quantity),
                                    fill.aggressor_side == xc::Side::Buy, sequence});
            total_quantity_ += fill.quantity;
            ++total_trades_;
        }
        while (trades_.size() > kMaxTape) {
            trades_.pop_front();
        }
    }

    void on_order_rejected(xc::SeqNum, const xc::NewOrder&, xc::RejectReason reason) override {
        ++rejects_;
        last_reject_ = reason;
    }

    const std::deque<Trade>& trades() const { return trades_; }
    std::uint64_t total_trades() const { return total_trades_; }
    std::uint64_t total_quantity() const { return total_quantity_; }
    std::uint64_t rejects() const { return rejects_; }
    xc::RejectReason last_reject() const { return last_reject_; }

    void clear() {
        trades_.clear();
        total_trades_ = 0;
        total_quantity_ = 0;
        rejects_ = 0;
        last_reject_ = xc::RejectReason::None;
    }

  private:
    static constexpr std::size_t kMaxTape = 64;
    std::deque<Trade> trades_;
    std::uint64_t total_trades_ = 0;
    std::uint64_t total_quantity_ = 0;
    std::uint64_t rejects_ = 0;
    xc::RejectReason last_reject_ = xc::RejectReason::None;
};

class Demo {
  public:
    Demo() { reset(0); }

    void reset(std::uint64_t seed) {
        clock_ = xc::ManualClock{0};
        engine_ = std::make_unique<xc::MatchingEngine>(clock_);
        limits_ = xc::risk::AccountLimits{};
        limits_.max_order_quantity = 500;
        controls_ = xc::risk::InstrumentControls{};
        risk_ = std::make_unique<xc::risk::RiskEngine>();
        kill_ = std::make_unique<xc::risk::KillSwitch>();
        tape_ = Tape{};
        rng_ = Rng{seed == 0 ? 1 : seed};

        xc::Instrument instrument;
        instrument.id = xc::InstrumentId{1};
        instrument.symbol = "DEMO";
        instrument.tick_size = 1;
        instrument.lot_size = 1;
        instrument.min_quantity = 1;
        instrument.self_trade_policy = policy_;
        instrument.expected_resting_orders = 16384;
        engine_->add_instrument(instrument);

        risk_->configure(xc::AccountId{1}, limits_);
        risk_->configure(xc::AccountId{2}, limits_);
        risk_->configure(xc::AccountId{3}, limits_);
        risk_->configure_instrument(xc::InstrumentId{1}, controls_);
        // The collar is measured against a reference price, so without one set
        // it would never bind and the control on the page would do nothing.
        risk_->set_reference_price(xc::InstrumentId{1}, mid_);

        engine_->set_risk_engine(risk_.get());
        engine_->set_kill_switch(kill_.get());
        engine_->add_listener(&tape_);

        next_id_ = 1;
        resting_.clear();
        commands_ = 0;
        elapsed_ns_ = 0.0;
    }

    /// Runs `count` commands and reports how long the batch took.
    ///
    /// Timed as a batch rather than per command because of the clock: see the
    /// note at the top of this file. The returned figure is honest about what
    /// it is -- the average cost of a command in this browser, on this
    /// machine, through WebAssembly.
    double step(int count) {
        if (count <= 0) {
            return 0.0;
        }
        const auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < count; ++i) {
            clock_.advance(1000);
            // Bias toward cancelling once the book is at its target depth.
            //
            // A fixed submit/cancel ratio never reaches a steady state: an
            // order that rests and is not cancelled is permanent, so the book
            // grows without bound and the ladder fills with quantities no real
            // market would show.
            //
            // Steered on the book's own resting count, not on the length of the
            // candidate list below. Those diverge, and the first version of this
            // steered on the list and did not work: an order that *fills* leaves
            // the book but stays in the list, so the list reads high while most
            // cancels drawn from it name orders that are already gone and do
            // nothing. The book kept growing while the generator believed it was
            // trimming.
            const xc::OrderBook* live_book = engine_->book(xc::InstrumentId{1});
            const std::size_t live = live_book == nullptr ? 0 : live_book->resting_order_count();
            const int submit_share = live > kTargetResting ? 15 : 72;

            // Resync the candidate list from the book when it runs dry.
            //
            // The list only ever loses entries, and an order that fills leaves
            // the book without telling it, so under heavy cancelling it drains
            // far faster than submissions refill it. Once empty every command
            // becomes a submission -- the generator stops trimming precisely
            // when the book is at its largest. Rebuilding from the book is O(n)
            // and happens rarely; guessing instead is what produced a 220,000
            // order book in a demo asking for 1,200.
            if (resting_.size() < 64 && live_book != nullptr && live > 0) {
                resting_.clear();
                live_book->for_each_resting_order([this](const xc::Order& resting) {
                    if (resting_.size() < kCandidateCap) {
                        resting_.push_back(
                            Candidate{resting.id.value(), resting.account.value()});
                    }
                });
            }

            const int roll = rng_.range(0, 99);

            if (roll < submit_share || resting_.empty()) {
                xc::NewOrder order;
                order.id = xc::OrderId{next_id_++};
                order.account = xc::AccountId{static_cast<std::uint64_t>(rng_.range(1, 3))};
                order.instrument = xc::InstrumentId{1};
                order.side = rng_.range(0, 1) == 0 ? xc::Side::Buy : xc::Side::Sell;
                order.type = rng_.range(0, 24) == 0 ? xc::OrderType::Market : xc::OrderType::Limit;
                order.tif = xc::TimeInForce::Day;
                order.price = static_cast<xc::Price>(rng_.range(mid_ - 12, mid_ + 12));
                order.quantity = static_cast<xc::Quantity>(rng_.range(1, 40));
                const xc::SubmitOutcome outcome = engine_->submit(order);
                if (outcome.rested) {
                    resting_.push_back(Candidate{order.id.value(), order.account.value()});
                }
            } else {
                const std::size_t slot =
                    static_cast<std::size_t>(rng_.range(0, static_cast<int>(resting_.size()) - 1));
                const Candidate candidate = resting_[slot];
                xc::CancelOrder cancel;
                cancel.id = xc::OrderId{candidate.id};
                cancel.account = xc::AccountId{candidate.account};
                cancel.instrument = xc::InstrumentId{1};
                engine_->cancel(cancel);
                // Dropped whether or not the cancel succeeded. A miss means the
                // order had already filled, which is exactly when the entry
                // needs removing.
                resting_[slot] = resting_.back();
                resting_.pop_back();
            }
            ++commands_;
        }

        const double ns =
            std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start)
                .count();
        elapsed_ns_ += ns;
        return ns;
    }

    val depth(int levels) const {
        xc::DepthSnapshot snapshot;
        const xc::OrderBook* book = engine_->book(xc::InstrumentId{1});
        if (book != nullptr) {
            book->depth(static_cast<std::size_t>(levels), snapshot);
        }

        val bids = val::array();
        val asks = val::array();
        for (std::size_t i = 0; i < snapshot.bids.size(); ++i) {
            bids.set(i, level_to_val(snapshot.bids[i]));
        }
        for (std::size_t i = 0; i < snapshot.asks.size(); ++i) {
            asks.set(i, level_to_val(snapshot.asks[i]));
        }

        val out = val::object();
        out.set("bids", bids);
        out.set("asks", asks);
        return out;
    }

    val stats() const {
        const xc::OrderBook* book = engine_->book(xc::InstrumentId{1});
        val out = val::object();
        out.set("commands", static_cast<double>(commands_));
        out.set("nsPerCommand", commands_ == 0 ? 0.0 : elapsed_ns_ / static_cast<double>(commands_));
        out.set("resting", book == nullptr ? 0.0
                                           : static_cast<double>(book->resting_order_count()));
        out.set("trades", static_cast<double>(tape_.total_trades()));
        out.set("tradedQuantity", static_cast<double>(tape_.total_quantity()));
        out.set("rejects", static_cast<double>(tape_.rejects()));
        out.set("lastReject", std::string(xc::to_string(tape_.last_reject())));
        out.set("halted", kill_->blocks_new_orders(xc::AccountId{1}));
        out.set("sequence", static_cast<double>(engine_->sequence()));

        if (book != nullptr) {
            const xc::TopOfBook top = book->top_of_book();
            out.set("bestBid", top.has_bid() ? static_cast<double>(top.bid_price) : 0.0);
            out.set("bestAsk", top.has_ask() ? static_cast<double>(top.ask_price) : 0.0);
        }
        return out;
    }

    val tape() const {
        val out = val::array();
        int i = 0;
        for (auto it = tape_.trades().rbegin(); it != tape_.trades().rend(); ++it, ++i) {
            val t = val::object();
            t.set("price", it->price);
            t.set("quantity", it->quantity);
            t.set("buy", it->aggressor_buy);
            t.set("sequence", static_cast<double>(it->sequence));
            out.set(i, t);
        }
        return out;
    }

    void set_policy(int policy) {
        policy_ = static_cast<xc::SelfTradePolicy>(policy);
        reset(1);
    }

    void set_collar_bps(int bps) {
        controls_.collar_bps = static_cast<std::uint32_t>(bps < 0 ? 0 : bps);
        risk_->configure_instrument(xc::InstrumentId{1}, controls_);
    }

    void set_max_order_quantity(int quantity) {
        limits_.max_order_quantity = static_cast<xc::Quantity>(quantity < 1 ? 1 : quantity);
        for (std::uint64_t account = 1; account <= 3; ++account) {
            risk_->configure(xc::AccountId{account}, limits_);
        }
    }

    void halt() { kill_->halt_venue(xc::risk::HaltReason::Manual, clock_.now()); }
    void resume() { kill_->resume_venue(); }

    static std::string build() { return xc::build_info().to_string(); }

  private:
    static val level_to_val(const xc::DepthLevel& level) {
        val out = val::object();
        out.set("price", static_cast<double>(level.price));
        out.set("quantity", static_cast<double>(level.quantity));
        out.set("orders", static_cast<double>(level.order_count));
        return out;
    }

    static constexpr int mid_ = 10000;

    /// Resting orders the flow steers toward. Large enough that ten levels a
    /// side are populated, small enough that the ladder shows numbers a reader
    /// can hold in their head.
    static constexpr std::size_t kTargetResting = 1200;

    /// Bound on the candidate list rebuilt from the book, so a resync stays
    /// cheap even if the book is briefly much larger than the target.
    static constexpr std::size_t kCandidateCap = 8192;

    xc::ManualClock clock_{0};
    std::unique_ptr<xc::MatchingEngine> engine_;
    std::unique_ptr<xc::risk::RiskEngine> risk_;
    std::unique_ptr<xc::risk::KillSwitch> kill_;
    xc::risk::AccountLimits limits_;
    xc::risk::InstrumentControls controls_;
    Tape tape_;
    Rng rng_{1};
    xc::SelfTradePolicy policy_ = xc::SelfTradePolicy::CancelIncoming;

    /// A resting order the generator may cancel. The account is carried with
    /// the id because the engine enforces that an account may only cancel its
    /// own orders -- an earlier version of this sent every cancel as account 1
    /// while submitting across three, so two thirds were correctly rejected as
    /// UnknownOrder and the book grew without bound while the generator
    /// believed it was trimming.
    struct Candidate {
        std::uint64_t id;
        std::uint64_t account;
    };

    std::uint64_t next_id_ = 1;
    std::vector<Candidate> resting_;
    std::uint64_t commands_ = 0;
    double elapsed_ns_ = 0.0;
};

}  // namespace

EMSCRIPTEN_BINDINGS(exchange_core) {
    emscripten::class_<Demo>("Demo")
        .constructor<>()
        .function("step", &Demo::step)
        .function("depth", &Demo::depth)
        .function("stats", &Demo::stats)
        .function("tape", &Demo::tape)
        .function("reset", &Demo::reset)
        .function("setPolicy", &Demo::set_policy)
        .function("setCollarBps", &Demo::set_collar_bps)
        .function("setMaxOrderQuantity", &Demo::set_max_order_quantity)
        .function("halt", &Demo::halt)
        .function("resume", &Demo::resume)
        .class_function("build", &Demo::build);
}
