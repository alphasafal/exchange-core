#pragma once

#include "xc/core/types.hpp"

namespace xc {

/// A request to open a new order.
///
/// Carries only what a client supplies. The engine stamps the sequence number
/// and the timestamp itself: letting a client choose either would hand it
/// control over queue priority.
struct NewOrder {
    OrderId id;
    AccountId account;
    InstrumentId instrument;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::Day;
    Price price = kNoPrice;
    Quantity quantity = 0;
    bool post_only = false;
};

/// A request to withdraw a resting order.
struct CancelOrder {
    OrderId id;
    AccountId account;
    InstrumentId instrument;
};

/// A request to amend a resting order's price or quantity. `new_quantity` is
/// the order's new total, counted from original submission.
struct ReplaceOrder {
    OrderId id;
    AccountId account;
    InstrumentId instrument;
    Price new_price = kNoPrice;
    Quantity new_quantity = 0;
};

}  // namespace xc
