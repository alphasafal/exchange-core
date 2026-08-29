#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "xc/core/clock.hpp"
#include "xc/core/commands.hpp"
#include "xc/core/engine_listener.hpp"
#include "xc/core/order_book.hpp"
#include "xc/risk/kill_switch.hpp"
#include "xc/risk/risk_engine.hpp"

namespace xc {

/// A command's position in the engine's total ordering, attached to whatever
/// the engine decided about it.
template<typename Result>
struct Sequenced : Result {
    SeqNum sequence = 0;
};

using SubmitOutcome = Sequenced<SubmitResult>;
using CancelOutcome = Sequenced<CancelResult>;
using ReplaceOutcome = Sequenced<ReplaceResult>;

/// Routes commands to per-instrument books and imposes a total order on them.
///
/// The engine is single-threaded by design. That is not a limitation waiting to
/// be lifted -- it is what makes the venue's behaviour a pure function of the
/// command sequence, and therefore what makes deterministic replay possible at
/// all. Throughput scales by partitioning instruments across engines, not by
/// threading one book.
///
/// **Sequence before process.** Every command is assigned its sequence number
/// on arrival, before any matching happens, and that number is what gets
/// journaled. Gateway threads may deliver commands in any order the network
/// hands them over; once the engine has stamped them, the order is fixed and
/// reproducible. Replaying the journal therefore reconstructs the same state
/// even though the original arrival order was never deterministic.
///
/// Rejected commands consume a sequence number too. Skipping them would make
/// the journal's numbering depend on decisions taken during processing, which
/// is the dependency this design exists to remove.
class MatchingEngine {
  public:
    /// The clock is borrowed, not owned, and must outlive the engine. Replay
    /// supplies a ManualClock driven from the journal; production supplies a
    /// SteadyClock.
    explicit MatchingEngine(Clock& clock);

    /// Registers an instrument. Returns false if the id or symbol is already
    /// taken. All instruments must be registered before trading begins.
    bool add_instrument(const Instrument& instrument);

    const Instrument* find_instrument(InstrumentId id) const;
    const Instrument* find_instrument(std::string_view symbol) const;

    /// Installs pre-trade risk. Borrowed, not owned. Optional: an engine with
    /// no risk component matches everything it is given, which is what the
    /// differential and benchmark harnesses want.
    ///
    /// When installed, every new order passes through it *before* reaching the
    /// book, and everything the book then does -- fills, rests, cancels, and
    /// quantity withdrawn by self-trade prevention -- is reported back so that
    /// exposure stays accurate. Missing any one of those paths would leak
    /// exposure that is never released, and the account would slowly lose the
    /// ability to trade with nothing in any log to explain it.
    void set_risk_engine(risk::RiskEngine* risk) noexcept { risk_ = risk; }

    /// Installs the kill switch. Borrowed, not owned.
    void set_kill_switch(risk::KillSwitch* kill) noexcept { kill_ = kill; }

    SubmitOutcome submit(const NewOrder& command);
    CancelOutcome cancel(const CancelOrder& command);
    ReplaceOutcome replace(const ReplaceOrder& command);

    /// Registers a listener. Not owned; must outlive the engine.
    void add_listener(EngineListener* listener);

    const OrderBook* book(InstrumentId id) const;

    /// Sequence number of the most recently accepted command. Zero before any
    /// command has been processed.
    SeqNum sequence() const noexcept { return sequence_; }

    /// Fills produced by the most recent command. Reused on the next one.
    std::span<const Fill> last_fills() const noexcept { return fills_; }

  private:
    SeqNum next_sequence() noexcept { return ++sequence_; }
    OrderBook* book_for(InstrumentId id);

    /// Releases everything the book just did back to the risk component:
    /// positions from fills, exposure from prevention withdrawals, and open
    /// order counts from both.
    void settle_risk(InstrumentId instrument);

    Clock& clock_;
    risk::RiskEngine* risk_ = nullptr;
    risk::KillSwitch* kill_ = nullptr;
    std::unordered_map<InstrumentId, std::unique_ptr<OrderBook>> books_;
    std::unordered_map<std::string, InstrumentId> symbols_;
    std::vector<EngineListener*> listeners_;

    /// Reused across commands so that matching never allocates to report its
    /// results. Cleared, not reconstructed, at the start of each command.
    std::vector<Fill> fills_;

    /// Resting orders that self-trade prevention removed or shrank during the
    /// current command. Reused like the fill buffer.
    std::vector<Withdrawal> withdrawals_;

    SeqNum sequence_ = 0;
};

}  // namespace xc
