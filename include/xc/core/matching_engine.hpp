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

    Clock& clock_;
    std::unordered_map<InstrumentId, std::unique_ptr<OrderBook>> books_;
    std::unordered_map<std::string, InstrumentId> symbols_;
    std::vector<EngineListener*> listeners_;

    /// Reused across commands so that matching never allocates to report its
    /// results. Cleared, not reconstructed, at the start of each command.
    std::vector<Fill> fills_;

    SeqNum sequence_ = 0;
};

}  // namespace xc
