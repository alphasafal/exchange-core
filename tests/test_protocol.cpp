#include <gtest/gtest.h>

#include <vector>

#include "xc/protocol/gap_detector.hpp"
#include "xc/protocol/messages.hpp"

namespace xc::protocol {
namespace {

std::vector<std::uint8_t> framed_new_order() {
    NewOrder command;
    command.id = OrderId{101};
    command.account = AccountId{7};
    command.instrument = InstrumentId{3};
    command.side = Side::Sell;
    command.type = OrderType::Limit;
    command.tif = TimeInForce::ImmediateOrCancel;
    command.post_only = true;
    command.price = 12'345;
    command.quantity = 500;

    std::vector<std::uint8_t> bytes;
    encode_new_order(bytes, 99, command);
    return bytes;
}

Fill sample_fill() {
    Fill fill;
    fill.id = TradeId{55};
    fill.instrument = InstrumentId{2};
    fill.price = 9'900;
    fill.quantity = 42;
    fill.aggressor_order = OrderId{1};
    fill.resting_order = OrderId{2};
    fill.aggressor_account = AccountId{10};
    fill.resting_account = AccountId{11};
    fill.aggressor_side = Side::Buy;
    fill.resting_filled = true;
    fill.timestamp = -55;
    return fill;
}

// --- Round trips -----------------------------------------------------------

TEST(Protocol, RoundTripsANewOrder) {
    const std::vector<std::uint8_t> bytes = framed_new_order();
    const DecodeResult decoded = decode(bytes);

    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    EXPECT_EQ(decoded.consumed, bytes.size());
    EXPECT_EQ(decoded.message.type, MessageType::NewOrder);
    EXPECT_EQ(decoded.message.sequence, 99u);
    EXPECT_EQ(decoded.message.new_order.id, OrderId{101});
    EXPECT_EQ(decoded.message.new_order.side, Side::Sell);
    EXPECT_EQ(decoded.message.new_order.tif, TimeInForce::ImmediateOrCancel);
    EXPECT_TRUE(decoded.message.new_order.post_only);
    EXPECT_EQ(decoded.message.new_order.price, 12'345);
    EXPECT_EQ(decoded.message.new_order.quantity, 500u);
}

TEST(Protocol, RoundTripsEveryMessageType) {
    std::vector<std::uint8_t> stream;
    const OrderAck ack{OrderId{1}, AccountId{2}, InstrumentId{3}, 10, 20, 999};
    const OrderReject reject{OrderId{4}, AccountId{5}, RejectReason::RiskLimit, -7};

    encode_cancel(stream, 1, CancelOrder{OrderId{1}, AccountId{2}, InstrumentId{3}});
    encode_replace(stream, 2, ReplaceOrder{OrderId{1}, AccountId{2}, InstrumentId{3}, 50, 5});
    encode_accepted(stream, 3, ack);
    encode_cancelled(stream, 4, ack);
    encode_replaced(stream, 5, ack);
    encode_rejected(stream, 6, reject);
    encode_execution(stream, 7, sample_fill());
    encode_trade(stream, 8, sample_fill());
    encode_heartbeat(stream, 9);

    const MessageType expected[] = {
        MessageType::CancelOrder,    MessageType::ReplaceOrder,  MessageType::OrderAccepted,
        MessageType::OrderCancelled, MessageType::OrderReplaced, MessageType::OrderRejected,
        MessageType::Execution,      MessageType::Trade,         MessageType::Heartbeat,
    };

    std::span<const std::uint8_t> cursor(stream);
    for (std::size_t i = 0; i < std::size(expected); ++i) {
        const DecodeResult decoded = decode(cursor);
        ASSERT_EQ(decoded.status, DecodeStatus::Ok) << "message " << i;
        EXPECT_EQ(decoded.message.type, expected[i]);
        EXPECT_EQ(decoded.message.sequence, i + 1);
        cursor = cursor.subspan(decoded.consumed);
    }
    EXPECT_TRUE(cursor.empty());
}

TEST(Protocol, RoundTripsAFillWithBothSides) {
    std::vector<std::uint8_t> bytes;
    encode_trade(bytes, 1, sample_fill());
    const DecodeResult decoded = decode(bytes);

    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    const Fill& fill = decoded.message.fill;
    EXPECT_EQ(fill.price, 9'900);
    EXPECT_EQ(fill.quantity, 42u);
    EXPECT_EQ(fill.aggressor_account, AccountId{10});
    EXPECT_EQ(fill.resting_account, AccountId{11});
    EXPECT_EQ(fill.aggressor_side, Side::Buy);
    EXPECT_TRUE(fill.resting_filled);
    EXPECT_EQ(fill.timestamp, -55);
}

TEST(Protocol, RoundTripsTopOfBookIncludingAnEmptySide) {
    xc::TopOfBook top;
    top.instrument = InstrumentId{4};
    top.bid_price = 100;
    top.bid_quantity = 5;
    // Ask deliberately absent.

    std::vector<std::uint8_t> bytes;
    encode_top_of_book(bytes, 12, top);
    const DecodeResult decoded = decode(bytes);

    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    EXPECT_TRUE(decoded.message.top_of_book.has_bid());
    EXPECT_FALSE(decoded.message.top_of_book.has_ask())
        << "an empty side must survive the wire as empty, not as a zero price";
    EXPECT_EQ(decoded.message.top_of_book.sequence, 12u);
}

TEST(Protocol, RoundTripsADepthSnapshot) {
    xc::DepthSnapshot depth;
    depth.instrument = InstrumentId{9};
    depth.bids = {{100, 10, 1}, {99, 20, 2}};
    depth.asks = {{101, 30, 3}};

    std::vector<std::uint8_t> bytes;
    encode_depth(bytes, 77, depth);
    const DecodeResult decoded = decode(bytes);

    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    EXPECT_EQ(decoded.message.depth.instrument, InstrumentId{9});
    EXPECT_EQ(decoded.message.depth.sequence, 77u);
    EXPECT_EQ(decoded.message.depth.bids, depth.bids);
    EXPECT_EQ(decoded.message.depth.asks, depth.asks);
}

TEST(Protocol, RoundTripsAnEmptyDepthSnapshot) {
    xc::DepthSnapshot depth;
    depth.instrument = InstrumentId{9};

    std::vector<std::uint8_t> bytes;
    encode_depth(bytes, 1, depth);
    const DecodeResult decoded = decode(bytes);
    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    EXPECT_TRUE(decoded.message.depth.bids.empty());
    EXPECT_TRUE(decoded.message.depth.asks.empty());
}

// --- Framing on a byte stream ----------------------------------------------

TEST(Protocol, ReportsAnyPrefixAsIncomplete) {
    const std::vector<std::uint8_t> bytes = framed_new_order();
    for (std::size_t length = 0; length < bytes.size(); ++length) {
        // TCP delivers bytes, not messages. A reader has to be able to wait for
        // the rest without treating a partial message as an error.
        const DecodeResult decoded = decode(std::span<const std::uint8_t>(bytes.data(), length));
        EXPECT_EQ(decoded.status, DecodeStatus::Incomplete) << "at length " << length;
    }
}

TEST(Protocol, DecodesMessagesPackedBackToBack) {
    std::vector<std::uint8_t> stream;
    for (SeqNum i = 1; i <= 20; ++i) {
        encode_heartbeat(stream, i);
    }

    std::span<const std::uint8_t> cursor(stream);
    SeqNum expected = 1;
    while (!cursor.empty()) {
        const DecodeResult decoded = decode(cursor);
        ASSERT_EQ(decoded.status, DecodeStatus::Ok);
        EXPECT_EQ(decoded.message.sequence, expected++);
        cursor = cursor.subspan(decoded.consumed);
    }
    EXPECT_EQ(expected, 21u);
}

// --- Malformed input -------------------------------------------------------

TEST(Protocol, RefusesAnUnknownVersion) {
    std::vector<std::uint8_t> bytes = framed_new_order();
    bytes[3] = kProtocolVersion + 1;
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

TEST(Protocol, RefusesAnUnknownMessageType) {
    std::vector<std::uint8_t> bytes = framed_new_order();
    bytes[2] = 250;
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

TEST(Protocol, RefusesAPayloadLongerThanTheProtocolAllows) {
    std::vector<std::uint8_t> bytes = framed_new_order();
    bytes[0] = 0xFF;
    bytes[1] = 0xFF;
    // Without a bound a reader would wait forever for bytes that are not coming
    // -- a one-line denial of service against every connection.
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

TEST(Protocol, RefusesAPayloadWithTrailingBytes) {
    NewOrder command;
    command.id = OrderId{1};
    command.account = AccountId{1};
    command.instrument = InstrumentId{1};
    command.quantity = 1;

    std::vector<std::uint8_t> bytes;
    encode_new_order(bytes, 1, command);
    // Lengthen the declared payload and supply the extra byte, as a newer
    // sender with an added field would.
    bytes[0] = static_cast<std::uint8_t>(bytes[0] + 1);
    bytes.push_back(0);
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable)
        << "silently ignoring a field a sender meant to be read is not safe";
}

TEST(Protocol, RefusesADepthCountLargerThanThePayload) {
    xc::DepthSnapshot depth;
    depth.instrument = InstrumentId{1};
    depth.bids = {{100, 10, 1}};

    std::vector<std::uint8_t> bytes;
    encode_depth(bytes, 1, depth);

    // Claim 4000 bid levels in a payload holding one. A count field is what a
    // malformed message abuses first, and the decoder must size against the
    // bytes actually present before it reserves anything.
    bytes[FrameHeader::kSize + 8] = 0xA0;
    bytes[FrameHeader::kSize + 9] = 0x0F;
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

TEST(Protocol, RefusesAnEnumerationOutsideItsRange) {
    std::vector<std::uint8_t> bytes = framed_new_order();
    // Side lives immediately after three 64-bit identifiers.
    bytes[FrameHeader::kSize + 24] = 7;
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

TEST(Protocol, NeverReadsPastTheBufferOnArbitraryBytes) {
    // A stand-in for the fuzz target: every one-byte mutation of a valid
    // message must produce a decision, never a crash or a read past the end.
    const std::vector<std::uint8_t> original = framed_new_order();
    for (std::size_t i = 0; i < original.size(); ++i) {
        for (const std::uint8_t mask :
             {std::uint8_t{0x01}, std::uint8_t{0x40}, std::uint8_t{0xFF}}) {
            std::vector<std::uint8_t> mutated = original;
            mutated[i] = static_cast<std::uint8_t>(mutated[i] ^ mask);
            const DecodeResult decoded = decode(mutated);
            if (decoded.status == DecodeStatus::Ok) {
                EXPECT_LE(decoded.consumed, mutated.size());
            }
        }
    }
}

// --- Gap detection ---------------------------------------------------------

TEST(GapDetector, AcceptsAContiguousStream) {
    GapDetector detector;
    for (SeqNum i = 1; i <= 100; ++i) {
        EXPECT_EQ(detector.observe(i), SequenceCheck::InOrder);
    }
    EXPECT_EQ(detector.in_order(), 100u);
    EXPECT_EQ(detector.gaps(), 0u);
}

TEST(GapDetector, ReportsMissingMessagesAndCountsThem) {
    GapDetector detector;
    detector.observe(1);
    detector.observe(2);

    EXPECT_EQ(detector.observe(6), SequenceCheck::Gap);
    EXPECT_EQ(detector.gaps(), 1u);
    EXPECT_EQ(detector.missing(), 3u) << "3, 4 and 5 never arrived";

    // The expectation jumps past the gap. A subscriber left waiting for a
    // message that will never arrive would treat the whole rest of the session
    // as out of order.
    EXPECT_EQ(detector.observe(7), SequenceCheck::InOrder);
}

TEST(GapDetector, DropsDuplicatesRatherThanApplyingThemTwice) {
    GapDetector detector;
    detector.observe(1);
    detector.observe(2);

    // Networks duplicate packets. Applying an increment twice leaves a book
    // wrong in a way nothing downstream can detect.
    EXPECT_EQ(detector.observe(2), SequenceCheck::Duplicate);
    EXPECT_EQ(detector.observe(1), SequenceCheck::Duplicate);
    EXPECT_EQ(detector.duplicates(), 2u);
    EXPECT_EQ(detector.observe(3), SequenceCheck::InOrder);
}

TEST(GapDetector, ResynchronisesFromASnapshot) {
    GapDetector detector;
    detector.observe(1);
    ASSERT_EQ(detector.observe(500), SequenceCheck::Gap);

    // A snapshot valid as of 500 already contains every increment up to it, so
    // the next message to apply is 501.
    detector.resynchronise(500);
    EXPECT_EQ(detector.expected(), 501u);
    EXPECT_EQ(detector.observe(501), SequenceCheck::InOrder);
}

TEST(GapDetector, CanStartMidStream) {
    // A subscriber that joins a running feed starts from the first sequence it
    // is told about rather than reporting every earlier message as missing.
    GapDetector detector(1000);
    EXPECT_EQ(detector.observe(1000), SequenceCheck::InOrder);
    EXPECT_EQ(detector.missing(), 0u);
}

}  // namespace
}  // namespace xc::protocol
