#include "xc/core/state_digest.hpp"

#include "xc/core/matching_engine.hpp"
#include "xc/core/order_book.hpp"
#include "xc/util/digest.hpp"

namespace xc {
namespace {

void feed_book(Digest& digest, const OrderBook& book) {
    digest.feed(book.instrument().id.value());
    digest.feed(static_cast<std::uint64_t>(book.resting_order_count()));
    book.for_each_resting_order([&](const Order& order) {
        digest.feed(order.id.value());
        digest.feed(order.account.value());
        digest.feed(static_cast<std::uint8_t>(order.side));
        digest.feed(static_cast<std::uint8_t>(order.type));
        digest.feed(static_cast<std::uint8_t>(order.tif));
        digest.feed(order.price);
        digest.feed(order.quantity);
        digest.feed(order.remaining);
        digest.feed(order.sequence);
    });

    // The per-account order id marks decide whether a future order is
    // accepted, so a replay that got them wrong would diverge later even with
    // an identical book. The table's slot order depends on insertion history,
    // so the entries are combined with an order-independent sum of per-entry
    // digests rather than fed in sequence.
    std::uint64_t marks = 0;
    std::uint64_t mark_count = 0;
    book.for_each_order_id_mark([&](AccountId account, OrderId highest) {
        Digest entry;
        entry.feed(account.value());
        entry.feed(highest.value());
        marks += entry.value();
        ++mark_count;
    });
    digest.feed(mark_count);
    digest.feed(marks);
}

}  // namespace

std::uint64_t digest(const OrderBook& book) {
    Digest digest;
    feed_book(digest, book);
    return digest.value();
}

std::uint64_t digest(const MatchingEngine& engine) {
    Digest digest;
    digest.feed(engine.sequence());
    for (const InstrumentId id : engine.instruments()) {
        if (const OrderBook* book = engine.book(id); book != nullptr) {
            feed_book(digest, *book);
        }
    }
    return digest.value();
}

}  // namespace xc
