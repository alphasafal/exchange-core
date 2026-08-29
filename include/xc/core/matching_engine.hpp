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
#include "xc/journal/writer.hpp"
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
/// **Sequence before state change.** A command is assigned its sequence number
/// once it has passed every gate that could turn it away without touching the
/// book, and immediately before it is journaled -- so the sequence number, the
/// journal record and the state change are the same event. Gateway threads may
/// deliver commands in any order the network hands them over; once the engine
/// has numbered one, the order is fixed and reproducible, and replaying the
/// journal reconstructs the same state even though arrival order never was
/// deterministic.
///
/// The consequence worth stating explicitly is that **journal sequence numbers
/// are gap-free**. A gap therefore means data loss and nothing else. Numbering
/// commands that are refused before the book -- an unknown instrument, a halt,
/// a risk limit -- would punch holes in that numbering for entirely healthy
/// reasons, and recovery would have no way to tell a rejected command from a
/// lost one.
///
/// Rejections that come from the book itself, such as a duplicate order id or
/// an unfillable fill-or-kill, are numbered and journaled like any other
/// command: they are decisions of the matching logic, and replaying them
/// reproduces the same decision.
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

    /// Installs the write-ahead journal. Borrowed, not owned.
    ///
    /// A command is journaled once it has been admitted by risk and before the
    /// book is touched, which is what makes this write-ahead with respect to
    /// the state it protects. Commands refused before that point changed
    /// nothing and are not journaled, so a replay needs the instrument set --
    /// which the journal carries -- but not a copy of the risk configuration.
    ///
    /// If the journal cannot record a command the engine refuses it and halts
    /// the venue. Matching something that could never be replayed is the exact
    /// state a journal exists to prevent, so failing loudly is the only safe
    /// response.
    void set_journal(journal::JournalWriter* journal) noexcept { journal_ = journal; }

    const OrderBook* book(InstrumentId id) const;

    /// Every registered instrument, in ascending id order.
    ///
    /// Ordered explicitly rather than exposing the hash map's iteration order,
    /// which is unspecified and can differ between runs of the same binary --
    /// enough on its own to make a state digest disagree with itself.
    const std::vector<InstrumentId>& instruments() const noexcept { return instrument_order_; }

    /// Forces the sequence number the next command will receive.
    ///
    /// Exists for replay and nothing else. Queue priority is derived from the
    /// sequence number, so a replayed command has to be given the number the
    /// original run gave it or the reconstructed book would be correctly
    /// matched and still differently ordered.
    void restore_sequence(SeqNum last_assigned) noexcept { sequence_ = last_assigned; }

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

    /// Journals a command that has been admitted and is about to be applied.
    /// Returns false when the journal failed, in which case the venue is halted.
    bool journal_command(const journal::Record& record, Nanos now);

    Clock& clock_;
    journal::JournalWriter* journal_ = nullptr;
    risk::RiskEngine* risk_ = nullptr;
    risk::KillSwitch* kill_ = nullptr;
    std::unordered_map<InstrumentId, std::unique_ptr<OrderBook>> books_;
    std::unordered_map<std::string, InstrumentId> symbols_;
    std::vector<InstrumentId> instrument_order_;

    /// Reused so journaling a command does not allocate.
    journal::Record scratch_record_;
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
