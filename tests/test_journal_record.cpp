#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "xc/journal/record.hpp"
#include "xc/util/crc32c.hpp"

namespace xc::journal {
namespace {

Record new_order_record(SeqNum sequence = 7) {
    Record record;
    record.type = RecordType::NewOrder;
    record.sequence = sequence;
    record.timestamp = -1'234'567;  // negative on purpose: signed round-trip
    record.new_order.id = OrderId{42};
    record.new_order.account = AccountId{9};
    record.new_order.instrument = InstrumentId{3};
    record.new_order.side = Side::Sell;
    record.new_order.type = OrderType::Limit;
    record.new_order.tif = TimeInForce::FillOrKill;
    record.new_order.post_only = true;
    record.new_order.price = 10'250;
    record.new_order.quantity = 999;
    return record;
}

std::vector<std::uint8_t> encoded(const Record& record) {
    std::vector<std::uint8_t> bytes;
    encode(record, bytes);
    return bytes;
}

// --- CRC-32C ---------------------------------------------------------------

TEST(Crc32c, MatchesTheCastagnoliReferenceVectors) {
    // The check value published for CRC-32C: the digest of "123456789".
    const std::string check = "123456789";
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(check.data());
    EXPECT_EQ(crc32c(std::span<const std::uint8_t>(bytes, check.size())), 0xE3069283U);

    // An empty input hashes to zero under this convention.
    EXPECT_EQ(crc32c(std::span<const std::uint8_t>()), 0U);
}

TEST(Crc32c, DetectsASingleFlippedBit) {
    std::vector<std::uint8_t> data(64, 0xA5);
    const std::uint32_t original = crc32c(data);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] ^= 0x01U;
        EXPECT_NE(crc32c(data), original) << "bit flip at byte " << i << " went undetected";
        data[i] ^= 0x01U;
    }
}

// --- Round trips -----------------------------------------------------------

TEST(JournalRecord, RoundTripsANewOrder) {
    const Record original = new_order_record();
    const DecodeResult decoded = decode(encoded(original));

    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    EXPECT_EQ(decoded.record.sequence, original.sequence);
    EXPECT_EQ(decoded.record.timestamp, original.timestamp) << "signed values survive intact";
    EXPECT_EQ(decoded.record.new_order.id, original.new_order.id);
    EXPECT_EQ(decoded.record.new_order.account, original.new_order.account);
    EXPECT_EQ(decoded.record.new_order.instrument, original.new_order.instrument);
    EXPECT_EQ(decoded.record.new_order.side, Side::Sell);
    EXPECT_EQ(decoded.record.new_order.tif, TimeInForce::FillOrKill);
    EXPECT_TRUE(decoded.record.new_order.post_only);
    EXPECT_EQ(decoded.record.new_order.price, 10'250);
    EXPECT_EQ(decoded.record.new_order.quantity, 999u);
    EXPECT_EQ(decoded.consumed, encoded(original).size());
}

TEST(JournalRecord, RoundTripsACancel) {
    Record original;
    original.type = RecordType::CancelOrder;
    original.sequence = 11;
    original.timestamp = 500;
    original.cancel_order = CancelOrder{OrderId{5}, AccountId{6}, InstrumentId{7}};

    const DecodeResult decoded = decode(encoded(original));
    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    EXPECT_EQ(decoded.record.cancel_order.id, OrderId{5});
    EXPECT_EQ(decoded.record.cancel_order.account, AccountId{6});
    EXPECT_EQ(decoded.record.cancel_order.instrument, InstrumentId{7});
}

TEST(JournalRecord, RoundTripsAReplace) {
    Record original;
    original.type = RecordType::ReplaceOrder;
    original.sequence = 12;
    original.replace_order = ReplaceOrder{OrderId{5}, AccountId{6}, InstrumentId{7}, 990, 44};

    const DecodeResult decoded = decode(encoded(original));
    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    EXPECT_EQ(decoded.record.replace_order.new_price, 990);
    EXPECT_EQ(decoded.record.replace_order.new_quantity, 44u);
}

TEST(JournalRecord, RoundTripsAnInstrumentDefinition) {
    Record original;
    original.type = RecordType::InstrumentDefined;
    original.sequence = 1;
    original.instrument.id = InstrumentId{4};
    original.instrument.symbol = "LONG-SYMBOL-NAME";
    original.instrument.tick_size = 25;
    original.instrument.lot_size = 100;
    original.instrument.min_quantity = 100;
    original.instrument.display_exponent = 4;
    original.instrument.self_trade_policy = SelfTradePolicy::DecrementBoth;

    const DecodeResult decoded = decode(encoded(original));
    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    EXPECT_EQ(decoded.record.instrument.symbol, "LONG-SYMBOL-NAME");
    EXPECT_EQ(decoded.record.instrument.tick_size, 25);
    EXPECT_EQ(decoded.record.instrument.lot_size, 100u);
    EXPECT_EQ(decoded.record.instrument.display_exponent, 4);
    EXPECT_EQ(decoded.record.instrument.self_trade_policy, SelfTradePolicy::DecrementBoth);
}

TEST(JournalRecord, RoundTripsAnEmptySymbol) {
    Record original;
    original.type = RecordType::InstrumentDefined;
    original.instrument.symbol.clear();
    const DecodeResult decoded = decode(encoded(original));
    ASSERT_EQ(decoded.status, DecodeStatus::Ok);
    EXPECT_TRUE(decoded.record.instrument.symbol.empty());
}

TEST(JournalRecord, ConcatenatedRecordsDecodeInSequence) {
    std::vector<std::uint8_t> stream;
    for (SeqNum i = 1; i <= 5; ++i) {
        encode(new_order_record(i), stream);
    }

    std::span<const std::uint8_t> cursor(stream);
    for (SeqNum i = 1; i <= 5; ++i) {
        const DecodeResult decoded = decode(cursor);
        ASSERT_EQ(decoded.status, DecodeStatus::Ok);
        EXPECT_EQ(decoded.record.sequence, i);
        cursor = cursor.subspan(decoded.consumed);
    }
    EXPECT_TRUE(cursor.empty()) << "the stream is exactly the records, with no slack";
}

// --- Damage ----------------------------------------------------------------

TEST(JournalRecord, ReportsATruncatedHeaderAsIncomplete) {
    const std::vector<std::uint8_t> bytes = encoded(new_order_record());
    for (std::size_t length = 0; length < RecordHeader::kSize; ++length) {
        const DecodeResult decoded = decode(std::span<const std::uint8_t>(bytes.data(), length));
        EXPECT_EQ(decoded.status, DecodeStatus::Incomplete) << "at length " << length;
    }
}

TEST(JournalRecord, ReportsATruncatedPayloadAsIncomplete) {
    const std::vector<std::uint8_t> bytes = encoded(new_order_record());
    for (std::size_t length = RecordHeader::kSize; length < bytes.size(); ++length) {
        const DecodeResult decoded = decode(std::span<const std::uint8_t>(bytes.data(), length));
        // A journal that ends mid-record is the expected shape of a crash, not
        // an error -- recovery has to be able to tell the two apart.
        EXPECT_EQ(decoded.status, DecodeStatus::Incomplete) << "at length " << length;
    }
}

TEST(JournalRecord, DetectsCorruptionAnywhereInTheRecord) {
    const std::vector<std::uint8_t> original = encoded(new_order_record());

    // Every byte from the sequence number onward is covered by the checksum.
    for (std::size_t i = 8; i < original.size(); ++i) {
        std::vector<std::uint8_t> damaged = original;
        damaged[i] ^= 0xFFU;
        const DecodeResult decoded = decode(damaged);
        EXPECT_NE(decoded.status, DecodeStatus::Ok)
            << "corruption at byte " << i << " was accepted";
    }
}

TEST(JournalRecord, RefusesAnUnknownFormatVersion) {
    std::vector<std::uint8_t> bytes = encoded(new_order_record());
    bytes[17] = kRecordVersion + 1;
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

TEST(JournalRecord, RefusesAnUnknownRecordType) {
    std::vector<std::uint8_t> bytes = encoded(new_order_record());
    bytes[16] = 200;
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

TEST(JournalRecord, RefusesANonZeroReservedField) {
    std::vector<std::uint8_t> bytes = encoded(new_order_record());
    bytes[18] = 1;
    // Reserved bytes are checked so that a future format which uses them cannot
    // be silently misread by this build as though they were absent.
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

TEST(JournalRecord, RefusesAnImplausibleLength) {
    std::vector<std::uint8_t> bytes = encoded(new_order_record());
    bytes[0] = 0xFF;
    bytes[1] = 0xFF;
    bytes[2] = 0xFF;
    bytes[3] = 0xFF;
    // Without a bound, a corrupt length field would make a reader wait for
    // gigabytes that are never coming.
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

TEST(JournalRecord, RefusesAnEnumerationValueOutsideItsRange) {
    Record original = new_order_record();
    std::vector<std::uint8_t> bytes;
    encode(original, bytes);

    // Corrupt the time-in-force byte and repair the checksum, so the record is
    // intact but describes something the engine has no behaviour for. Casting
    // it blindly would produce a value no switch handles.
    const std::size_t tif_offset = RecordHeader::kSize + 8 + 8 + 8 + 8 + 2;
    bytes[tif_offset] = 99;
    const std::uint32_t repaired =
        crc32c(std::span<const std::uint8_t>(bytes.data() + 8, bytes.size() - 8));
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[4 + i] = static_cast<std::uint8_t>((repaired >> (8 * i)) & 0xFFU);
    }

    ASSERT_NE(decode(bytes).status, DecodeStatus::Corrupt) << "the checksum was repaired";
    EXPECT_EQ(decode(bytes).status, DecodeStatus::Unreadable);
}

}  // namespace
}  // namespace xc::journal
