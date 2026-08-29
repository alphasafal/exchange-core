#pragma once

#include <cstdint>
#include <vector>

#include "xc/core/order_book.hpp"

namespace xc::testing {

/// Builds orders for tests without repeating a dozen field initialisers.
///
/// Sequence numbers are assigned automatically in construction order, which is
/// exactly what time priority depends on, so a test that builds orders in the
/// order it wants them ranked gets the right priority for free.
class OrderFactory {
  public:
    explicit OrderFactory(InstrumentId instrument = InstrumentId{1}) : instrument_(instrument) {}

    Order limit(std::uint64_t id, Side side, Price price, Quantity quantity,
                std::uint64_t account = 1) {
        Order order;
        order.id = OrderId{id};
        order.account = AccountId{account};
        order.instrument = instrument_;
        order.side = side;
        order.type = OrderType::Limit;
        order.price = price;
        order.quantity = quantity;
        order.remaining = quantity;
        order.tif = TimeInForce::Day;
        order.sequence = ++sequence_;
        order.accepted_at = static_cast<Nanos>(sequence_) * 1000;
        return order;
    }

    Order market(std::uint64_t id, Side side, Quantity quantity, std::uint64_t account = 1) {
        Order order = limit(id, side, kNoPrice, quantity, account);
        order.type = OrderType::Market;
        order.price = kNoPrice;
        return order;
    }

    Order with_tif(Order order, TimeInForce tif) {
        order.tif = tif;
        return order;
    }

  private:
    InstrumentId instrument_;
    SeqNum sequence_ = 0;
};

inline Instrument test_instrument(Price tick_size = 1, Quantity lot_size = 1) {
    Instrument instrument;
    instrument.id = InstrumentId{1};
    instrument.symbol = "TEST";
    instrument.tick_size = tick_size;
    instrument.lot_size = lot_size;
    instrument.min_quantity = lot_size;
    instrument.self_trade_policy = SelfTradePolicy::Allow;
    return instrument;
}

}  // namespace xc::testing
