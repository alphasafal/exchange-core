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
