#pragma once

#include <span>

#include "xc/core/commands.hpp"
#include "xc/core/fill.hpp"
#include "xc/core/order.hpp"

namespace xc {

/// Receives everything the engine decides, in the order it decided it.
///
/// Implementations are registered once at startup and are not owned by the
/// engine. Non-owning raw pointers rather than shared_ptr on purpose: listener
/// lifetimes here are static, and reference-count traffic on a path that runs
/// once per message is pure overhead paid for a guarantee nothing needs.
///
/// Every callback runs on the matching thread and must not block. A listener
/// that does slow work hands its latency straight to the next order in the
/// queue; the market data publisher exists precisely so that publishing can
/// happen somewhere else.
class EngineListener {
  public:
    virtual ~EngineListener() = default;

    virtual void on_order_accepted(SeqNum, const Order&) {}
    virtual void on_order_rejected(SeqNum, const NewOrder&, RejectReason) {}
    virtual void on_order_cancelled(SeqNum, const Order&) {}
    virtual void on_order_replaced(SeqNum, const Order& /*previous*/, const Order& /*amended*/) {}

    /// Fills produced by one command, in the order they occurred. Passed as a
    /// span over the engine's own buffer, which is reused on the next command,
    /// so a listener that needs to keep them must copy.
    virtual void on_fills(SeqNum, std::span<const Fill>) {}
};

}  // namespace xc
