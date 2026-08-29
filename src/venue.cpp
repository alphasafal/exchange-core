#include "xc/venue.hpp"

#include <utility>

namespace xc {

Venue::Venue(VenueConfig config)
    : config_(std::move(config)), gateway_(config_.gateway), feed_(config_.feed) {
    session_buffer_.reserve(4096);
    feed_buffer_.reserve(4096);
}

Venue::~Venue() {
    stop();
}

bool Venue::start() {
    if (config_.journal.has_value()) {
        journal_ = std::make_unique<journal::JournalWriter>(*config_.journal);
        if (!journal_->open()) {
            last_error_ = journal_->last_error();
            return false;
        }
        engine_.set_journal(journal_.get());
    }

    engine_.set_risk_engine(&risk_);
    engine_.set_kill_switch(&kill_);
    engine_.add_listener(this);

    for (const Instrument& instrument : config_.instruments) {
        if (!engine_.add_instrument(instrument)) {
            last_error_ = "could not register instrument " + instrument.symbol;
            return false;
        }
        risk_.configure_instrument(instrument.id, config_.default_controls);
    }

    if (!feed_.open()) {
        last_error_ = feed_.last_error();
        return false;
    }

    gateway_.on_message(
        [this](net::ConnectionId id, const protocol::Message& message) { handle(id, message); });

    if (!gateway_.start()) {
        last_error_ = gateway_.last_error();
        return false;
    }
    return true;
}

void Venue::stop() {
    gateway_.stop();
    feed_.close();
    if (journal_) {
        journal_->close();
    }
}

bool Venue::poll(int timeout_ms) {
    return gateway_.poll(timeout_ms);
}

void Venue::send_session(std::span<const std::uint8_t> bytes) {
    if (current_connection_ != 0) {
        gateway_.send(current_connection_, bytes);
    }
}

void Venue::handle(net::ConnectionId id, const protocol::Message& message) {
    current_connection_ = id;
    ++commands_processed_;

    InstrumentId instrument;
    switch (message.type) {
        case protocol::MessageType::NewOrder:
            instrument = message.new_order.instrument;
            // Every account that trades is given the configured limits on first
            // sight. A venue that silently exempted unconfigured accounts from
            // risk would apply its controls to exactly the clients that had
            // already been onboarded, and to none of the ones that had not.
            if (risk_.limits_for(message.new_order.account) == nullptr) {
                risk_.configure(message.new_order.account, config_.default_limits);
            }
            engine_.submit(message.new_order);
            break;
        case protocol::MessageType::CancelOrder:
            instrument = message.cancel_order.instrument;
            engine_.cancel(message.cancel_order);
            break;
        case protocol::MessageType::ReplaceOrder:
            instrument = message.replace_order.instrument;
            engine_.replace(message.replace_order);
            break;
        default:
            // Session and market data messages are things the venue sends, not
            // things it accepts. A client sending one is either confused or
            // probing, and either way it is ignored rather than acted on.
            current_connection_ = 0;
            return;
    }

    if (instrument.valid()) {
        publish_market_data(instrument);
    }
    current_connection_ = 0;
}

void Venue::publish_market_data(InstrumentId instrument) {
    const OrderBook* book = engine_.book(instrument);
    if (book == nullptr) {
        return;
    }

    feed_buffer_.clear();
    if (config_.published_depth == 0) {
        protocol::encode_top_of_book(feed_buffer_, ++feed_sequence_, book->top_of_book());
    } else {
        book->depth(config_.published_depth, depth_);
        protocol::encode_depth(feed_buffer_, ++feed_sequence_, depth_);
    }
    feed_.publish(feed_buffer_);
}

void Venue::on_order_accepted(SeqNum, const Order& order) {
    session_buffer_.clear();
    protocol::encode_accepted(
        session_buffer_, order.sequence,
        protocol::OrderAck{order.id, order.account, order.instrument, order.filled(),
                           order.remaining, order.accepted_at});
    send_session(session_buffer_);
}

void Venue::on_order_rejected(SeqNum sequence, const NewOrder& command, RejectReason reason) {
    session_buffer_.clear();
    protocol::encode_rejected(
        session_buffer_, sequence,
        protocol::OrderReject{command.id, command.account, reason, clock_.now()});
    send_session(session_buffer_);
}

void Venue::on_order_cancelled(SeqNum sequence, const Order& order) {
    session_buffer_.clear();
    protocol::encode_cancelled(session_buffer_, sequence,
                               protocol::OrderAck{order.id, order.account, order.instrument,
                                                  order.filled(), order.remaining, clock_.now()});
    send_session(session_buffer_);
}

void Venue::on_order_replaced(SeqNum sequence, const Order&, const Order& amended) {
    session_buffer_.clear();
    protocol::encode_replaced(
        session_buffer_, sequence,
        protocol::OrderAck{amended.id, amended.account, amended.instrument, amended.filled(),
                           amended.remaining, clock_.now()});
    send_session(session_buffer_);
}

void Venue::on_fills(SeqNum sequence, std::span<const Fill> fills) {
    for (const Fill& fill : fills) {
        // The client that sent the command gets its execution privately...
        session_buffer_.clear();
        protocol::encode_execution(session_buffer_, sequence, fill);
        send_session(session_buffer_);

        // ...and the trade goes out on the public feed. Both carry the same
        // fill, but on different sequence streams: the session stream is the
        // engine's command order, and the feed stream is its own, so a
        // subscriber's gap detection is unaffected by how many clients happen
        // to be connected.
        feed_buffer_.clear();
        protocol::encode_trade(feed_buffer_, ++feed_sequence_, fill);
        feed_.publish(feed_buffer_);
    }
}

}  // namespace xc
