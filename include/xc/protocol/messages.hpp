#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "xc/core/commands.hpp"
#include "xc/core/depth.hpp"
#include "xc/core/fill.hpp"
#include "xc/core/types.hpp"

namespace xc::protocol {

/// Wire format version. A peer that meets a version it does not know closes the
/// connection rather than guessing at the layout.
inline constexpr std::uint8_t kProtocolVersion = 1;

enum class MessageType : std::uint8_t {
    // Client to venue.
    NewOrder = 1,
    CancelOrder = 2,
    ReplaceOrder = 3,

    // Venue to the client that sent the command.
    OrderAccepted = 16,
    OrderRejected = 17,
    OrderCancelled = 18,
    OrderReplaced = 19,
    Execution = 20,

    // Venue to every market data subscriber.
    Trade = 32,
    TopOfBook = 33,
    DepthSnapshot = 34,

    /// Sent on an otherwise idle feed.
    ///
    /// Without it, a subscriber cannot distinguish a quiet market from a dead
    /// connection, and would sit happily on a feed that stopped an hour ago.
    /// The heartbeat carries the current sequence number, so a subscriber that
    /// missed the last message before the market went quiet still finds out.
    Heartbeat = 48,
};

/// Fixed 12-byte prefix on every message.
///
///   offset  size  field
///        0     2  payload length, excluding this header
///        2     1  message type
///        3     1  protocol version
///        4     8  sequence number
///
/// The length is first so that a reader can frame the stream before it
/// interprets anything else, and the sequence number is in the header rather
/// than the payload so gap detection never has to parse a message body -- a
/// subscriber can spot a gap in a message type it does not even implement.
struct FrameHeader {
    static constexpr std::size_t kSize = 12;
    static constexpr std::uint16_t kMaxPayload = 8192;

    std::uint16_t payload_length = 0;
    MessageType type = MessageType::Heartbeat;
    std::uint8_t version = kProtocolVersion;
    SeqNum sequence = 0;
};

/// Reported to the client that sent a command.
struct OrderAck {
    OrderId order;
    AccountId account;
    InstrumentId instrument;
    Quantity filled = 0;
    Quantity remaining = 0;
    Nanos timestamp = 0;
};

struct OrderReject {
    OrderId order;
    AccountId account;
    RejectReason reason = RejectReason::None;
    Nanos timestamp = 0;
};

/// A decoded message. One struct with a member per payload, matching the
/// journal's approach: the few unused bytes cost nothing next to the socket
/// read, and it keeps both the decoder and its tests obvious.
struct Message {
    MessageType type = MessageType::Heartbeat;
    SeqNum sequence = 0;

    NewOrder new_order;
    CancelOrder cancel_order;
    ReplaceOrder replace_order;
    OrderAck ack;
    OrderReject reject;
    Fill fill;
    xc::TopOfBook top_of_book;
    xc::DepthSnapshot depth;
};

// --- Encoding --------------------------------------------------------------
// Each appends one complete framed message to `out`.

void encode_new_order(std::vector<std::uint8_t>& out, SeqNum sequence, const NewOrder& command);
void encode_cancel(std::vector<std::uint8_t>& out, SeqNum sequence, const CancelOrder& command);
void encode_replace(std::vector<std::uint8_t>& out, SeqNum sequence, const ReplaceOrder& command);
void encode_accepted(std::vector<std::uint8_t>& out, SeqNum sequence, const OrderAck& ack);
void encode_cancelled(std::vector<std::uint8_t>& out, SeqNum sequence, const OrderAck& ack);
void encode_replaced(std::vector<std::uint8_t>& out, SeqNum sequence, const OrderAck& ack);
void encode_rejected(std::vector<std::uint8_t>& out, SeqNum sequence, const OrderReject& reject);
void encode_execution(std::vector<std::uint8_t>& out, SeqNum sequence, const Fill& fill);
void encode_trade(std::vector<std::uint8_t>& out, SeqNum sequence, const Fill& fill);
void encode_top_of_book(std::vector<std::uint8_t>& out, SeqNum sequence, const xc::TopOfBook& top);
void encode_depth(std::vector<std::uint8_t>& out, SeqNum sequence, const xc::DepthSnapshot& depth);
void encode_heartbeat(std::vector<std::uint8_t>& out, SeqNum sequence);

// --- Decoding --------------------------------------------------------------

enum class DecodeStatus : std::uint8_t {
    Ok,
    /// Not all of the message has arrived. On a stream this is routine: TCP
    /// delivers bytes, not messages, and a reader must be able to wait for more
    /// without treating a partial message as an error.
    Incomplete,
    /// The bytes cannot be a message this build understands. On a stream there
    /// is no way to find the start of the next one, so the connection is closed
    /// rather than resynchronised by guesswork.
    Unreadable,
};

struct DecodeResult {
    DecodeStatus status = DecodeStatus::Incomplete;
    std::size_t consumed = 0;
    Message message;
};

/// Decodes the message at the start of `data`.
DecodeResult decode(std::span<const std::uint8_t> data);

}  // namespace xc::protocol
