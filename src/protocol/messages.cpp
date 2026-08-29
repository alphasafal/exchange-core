#include "xc/protocol/messages.hpp"

#include "xc/util/byte_codec.hpp"

namespace xc::protocol {
namespace {

/// Writes the frame header with a placeholder length, and returns where the
/// message began so the length can be filled in once the payload is known.
///
/// Encoding straight into the destination and back-patching avoids building the
/// payload in a scratch buffer and copying it, which on an outbound feed would
/// be a second pass over every byte the venue publishes.
std::size_t begin_frame(std::vector<std::uint8_t>& out, MessageType type, SeqNum sequence) {
    const std::size_t start = out.size();
    ByteWriter writer(out);
    writer.u16(0);
    writer.u8(static_cast<std::uint8_t>(type));
    writer.u8(kProtocolVersion);
    writer.u64(sequence);
    return start;
}

void end_frame(std::vector<std::uint8_t>& out, std::size_t start) {
    const std::size_t payload = out.size() - start - FrameHeader::kSize;
    out[start] = static_cast<std::uint8_t>(payload & 0xFFU);
    out[start + 1] = static_cast<std::uint8_t>((payload >> 8) & 0xFFU);
}

void write_fill(ByteWriter& writer, const Fill& fill) {
    writer.u64(fill.id.value());
    writer.u64(fill.instrument.value());
    writer.i64(fill.price);
    writer.u64(fill.quantity);
    writer.u64(fill.aggressor_order.value());
    writer.u64(fill.resting_order.value());
    writer.u64(fill.aggressor_account.value());
    writer.u64(fill.resting_account.value());
    writer.u8(static_cast<std::uint8_t>(fill.aggressor_side));
    writer.boolean(fill.resting_filled);
    writer.i64(fill.timestamp);
}

void read_fill(ByteReader& reader, Fill& fill) {
    fill.id = TradeId{reader.u64()};
    fill.instrument = InstrumentId{reader.u64()};
    fill.price = reader.i64();
    fill.quantity = reader.u64();
    fill.aggressor_order = OrderId{reader.u64()};
    fill.resting_order = OrderId{reader.u64()};
    fill.aggressor_account = AccountId{reader.u64()};
    fill.resting_account = AccountId{reader.u64()};
    fill.aggressor_side = reader.enumeration<Side>(static_cast<std::uint8_t>(Side::Sell));
    fill.resting_filled = reader.boolean();
    fill.timestamp = reader.i64();
}

void write_ack(ByteWriter& writer, const OrderAck& ack) {
    writer.u64(ack.order.value());
    writer.u64(ack.account.value());
    writer.u64(ack.instrument.value());
    writer.u64(ack.filled);
    writer.u64(ack.remaining);
    writer.i64(ack.timestamp);
}

void read_ack(ByteReader& reader, OrderAck& ack) {
    ack.order = OrderId{reader.u64()};
    ack.account = AccountId{reader.u64()};
    ack.instrument = InstrumentId{reader.u64()};
    ack.filled = reader.u64();
    ack.remaining = reader.u64();
    ack.timestamp = reader.i64();
}

bool is_known_type(std::uint8_t type) {
    switch (static_cast<MessageType>(type)) {
        case MessageType::NewOrder:
        case MessageType::CancelOrder:
        case MessageType::ReplaceOrder:
        case MessageType::OrderAccepted:
        case MessageType::OrderRejected:
        case MessageType::OrderCancelled:
        case MessageType::OrderReplaced:
        case MessageType::Execution:
        case MessageType::Trade:
        case MessageType::TopOfBook:
        case MessageType::DepthSnapshot:
        case MessageType::Heartbeat:
            return true;
    }
    return false;
}

/// Bound on levels per side in a depth snapshot.
///
/// A length-prefixed repeating group is where a malformed message does the most
/// damage: a count field claiming four billion levels would have the decoder
/// reserve memory for them before it discovered the bytes were not there.
constexpr std::uint16_t kMaxDepthLevels = 256;

}  // namespace

void encode_new_order(std::vector<std::uint8_t>& out, SeqNum sequence, const NewOrder& command) {
    const std::size_t start = begin_frame(out, MessageType::NewOrder, sequence);
    ByteWriter writer(out);
    writer.u64(command.id.value());
    writer.u64(command.account.value());
    writer.u64(command.instrument.value());
    writer.u8(static_cast<std::uint8_t>(command.side));
    writer.u8(static_cast<std::uint8_t>(command.type));
    writer.u8(static_cast<std::uint8_t>(command.tif));
    writer.boolean(command.post_only);
    writer.i64(command.price);
    writer.u64(command.quantity);
    end_frame(out, start);
}

void encode_cancel(std::vector<std::uint8_t>& out, SeqNum sequence, const CancelOrder& command) {
    const std::size_t start = begin_frame(out, MessageType::CancelOrder, sequence);
    ByteWriter writer(out);
    writer.u64(command.id.value());
    writer.u64(command.account.value());
    writer.u64(command.instrument.value());
    end_frame(out, start);
}

void encode_replace(std::vector<std::uint8_t>& out, SeqNum sequence, const ReplaceOrder& command) {
    const std::size_t start = begin_frame(out, MessageType::ReplaceOrder, sequence);
    ByteWriter writer(out);
    writer.u64(command.id.value());
    writer.u64(command.account.value());
    writer.u64(command.instrument.value());
    writer.i64(command.new_price);
    writer.u64(command.new_quantity);
    end_frame(out, start);
}

namespace {
void encode_ack_as(std::vector<std::uint8_t>& out, SeqNum sequence, MessageType type,
                   const OrderAck& ack) {
    const std::size_t start = begin_frame(out, type, sequence);
    ByteWriter writer(out);
    write_ack(writer, ack);
    end_frame(out, start);
}
}  // namespace

void encode_accepted(std::vector<std::uint8_t>& out, SeqNum sequence, const OrderAck& ack) {
    encode_ack_as(out, sequence, MessageType::OrderAccepted, ack);
}

void encode_cancelled(std::vector<std::uint8_t>& out, SeqNum sequence, const OrderAck& ack) {
    encode_ack_as(out, sequence, MessageType::OrderCancelled, ack);
}

void encode_replaced(std::vector<std::uint8_t>& out, SeqNum sequence, const OrderAck& ack) {
    encode_ack_as(out, sequence, MessageType::OrderReplaced, ack);
}

void encode_rejected(std::vector<std::uint8_t>& out, SeqNum sequence, const OrderReject& reject) {
    const std::size_t start = begin_frame(out, MessageType::OrderRejected, sequence);
    ByteWriter writer(out);
    writer.u64(reject.order.value());
    writer.u64(reject.account.value());
    writer.u8(static_cast<std::uint8_t>(reject.reason));
    writer.i64(reject.timestamp);
    end_frame(out, start);
}

void encode_execution(std::vector<std::uint8_t>& out, SeqNum sequence, const Fill& fill) {
    const std::size_t start = begin_frame(out, MessageType::Execution, sequence);
    ByteWriter writer(out);
    write_fill(writer, fill);
    end_frame(out, start);
}

void encode_trade(std::vector<std::uint8_t>& out, SeqNum sequence, const Fill& fill) {
    const std::size_t start = begin_frame(out, MessageType::Trade, sequence);
    ByteWriter writer(out);
    write_fill(writer, fill);
    end_frame(out, start);
}

void encode_top_of_book(std::vector<std::uint8_t>& out, SeqNum sequence, const xc::TopOfBook& top) {
    const std::size_t start = begin_frame(out, MessageType::TopOfBook, sequence);
    ByteWriter writer(out);
    writer.u64(top.instrument.value());
    writer.i64(top.bid_price);
    writer.u64(top.bid_quantity);
    writer.i64(top.ask_price);
    writer.u64(top.ask_quantity);
    end_frame(out, start);
}

void encode_depth(std::vector<std::uint8_t>& out, SeqNum sequence, const xc::DepthSnapshot& depth) {
    const std::size_t start = begin_frame(out, MessageType::DepthSnapshot, sequence);
    ByteWriter writer(out);
    writer.u64(depth.instrument.value());
    const auto write_side = [&writer](const std::vector<DepthLevel>& levels) {
        const auto count =
            static_cast<std::uint16_t>(std::min<std::size_t>(levels.size(), kMaxDepthLevels));
        writer.u16(count);
        for (std::uint16_t i = 0; i < count; ++i) {
            writer.i64(levels[i].price);
            writer.u64(levels[i].quantity);
            writer.u32(levels[i].order_count);
        }
    };
    write_side(depth.bids);
    write_side(depth.asks);
    end_frame(out, start);
}

void encode_heartbeat(std::vector<std::uint8_t>& out, SeqNum sequence) {
    const std::size_t start = begin_frame(out, MessageType::Heartbeat, sequence);
    end_frame(out, start);
}

DecodeResult decode(std::span<const std::uint8_t> data) {
    DecodeResult result;

    if (data.size() < FrameHeader::kSize) {
        result.status = DecodeStatus::Incomplete;
        return result;
    }

    ByteReader header(data.first(FrameHeader::kSize));
    const std::uint16_t payload_length = header.u16();
    const std::uint8_t type = header.u8();
    const std::uint8_t version = header.u8();
    const SeqNum sequence = header.u64();

    if (version != kProtocolVersion || !is_known_type(type) ||
        payload_length > FrameHeader::kMaxPayload) {
        result.status = DecodeStatus::Unreadable;
        return result;
    }

    const std::size_t total = FrameHeader::kSize + payload_length;
    if (data.size() < total) {
        // Routine on a stream: TCP delivers bytes, not messages.
        result.status = DecodeStatus::Incomplete;
        return result;
    }

    Message& message = result.message;
    message.type = static_cast<MessageType>(type);
    message.sequence = sequence;

    ByteReader reader(data.subspan(FrameHeader::kSize, payload_length));
    switch (message.type) {
        case MessageType::NewOrder: {
            NewOrder& command = message.new_order;
            command.id = OrderId{reader.u64()};
            command.account = AccountId{reader.u64()};
            command.instrument = InstrumentId{reader.u64()};
            command.side = reader.enumeration<Side>(static_cast<std::uint8_t>(Side::Sell));
            command.type =
                reader.enumeration<OrderType>(static_cast<std::uint8_t>(OrderType::Market));
            command.tif = reader.enumeration<TimeInForce>(
                static_cast<std::uint8_t>(TimeInForce::GoodTilCancelled));
            command.post_only = reader.boolean();
            command.price = reader.i64();
            command.quantity = reader.u64();
            break;
        }
        case MessageType::CancelOrder: {
            CancelOrder& command = message.cancel_order;
            command.id = OrderId{reader.u64()};
            command.account = AccountId{reader.u64()};
            command.instrument = InstrumentId{reader.u64()};
            break;
        }
        case MessageType::ReplaceOrder: {
            ReplaceOrder& command = message.replace_order;
            command.id = OrderId{reader.u64()};
            command.account = AccountId{reader.u64()};
            command.instrument = InstrumentId{reader.u64()};
            command.new_price = reader.i64();
            command.new_quantity = reader.u64();
            break;
        }
        case MessageType::OrderAccepted:
        case MessageType::OrderCancelled:
        case MessageType::OrderReplaced:
            read_ack(reader, message.ack);
            break;
        case MessageType::OrderRejected: {
            OrderReject& reject = message.reject;
            reject.order = OrderId{reader.u64()};
            reject.account = AccountId{reader.u64()};
            reject.reason = reader.enumeration<RejectReason>(
                static_cast<std::uint8_t>(RejectReason::RateLimit));
            reject.timestamp = reader.i64();
            break;
        }
        case MessageType::Execution:
        case MessageType::Trade:
            read_fill(reader, message.fill);
            break;
        case MessageType::TopOfBook: {
            xc::TopOfBook& top = message.top_of_book;
            top.sequence = sequence;
            top.instrument = InstrumentId{reader.u64()};
            top.bid_price = reader.i64();
            top.bid_quantity = reader.u64();
            top.ask_price = reader.i64();
            top.ask_quantity = reader.u64();
            break;
        }
        case MessageType::DepthSnapshot: {
            xc::DepthSnapshot& depth = message.depth;
            depth.clear();
            depth.sequence = sequence;
            depth.instrument = InstrumentId{reader.u64()};
            const auto read_side = [&reader](std::vector<DepthLevel>& levels) {
                const std::uint16_t count = reader.u16();
                if (!reader.ok() || count > kMaxDepthLevels) {
                    return false;
                }
                // Sized against what the buffer can actually hold before
                // anything is reserved. A count field is the field a malformed
                // message abuses first.
                if (reader.remaining() < static_cast<std::size_t>(count) * 20) {
                    return false;
                }
                levels.reserve(count);
                for (std::uint16_t i = 0; i < count; ++i) {
                    DepthLevel level;
                    level.price = reader.i64();
                    level.quantity = reader.u64();
                    level.order_count = reader.u32();
                    levels.push_back(level);
                }
                return true;
            };
            if (!read_side(depth.bids) || !read_side(depth.asks)) {
                result.status = DecodeStatus::Unreadable;
                return result;
            }
            break;
        }
        case MessageType::Heartbeat:
            break;
    }

    if (!reader.fully_consumed()) {
        // Either the payload ran short, or it carried a field this build does
        // not know about. Neither is safe to act on.
        result.status = DecodeStatus::Unreadable;
        return result;
    }

    result.status = DecodeStatus::Ok;
    result.consumed = total;
    return result;
}

}  // namespace xc::protocol
