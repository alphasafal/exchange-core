#include "xc/journal/record.hpp"

#include <cstring>

#include "xc/util/byte_codec.hpp"
#include "xc/util/crc32c.hpp"

namespace xc::journal {
namespace {

void encode_payload(const Record& record, std::vector<std::uint8_t>& out) {
    ByteWriter writer(out);
    writer.i64(record.timestamp);

    switch (record.type) {
        case RecordType::InstrumentDefined: {
            const Instrument& instrument = record.instrument;
            writer.u64(instrument.id.value());
            writer.string(instrument.symbol);
            writer.i64(instrument.tick_size);
            writer.u64(instrument.lot_size);
            writer.u64(instrument.min_quantity);
            writer.u8(instrument.display_exponent);
            writer.u8(static_cast<std::uint8_t>(instrument.self_trade_policy));
            break;
        }
        case RecordType::NewOrder: {
            const NewOrder& command = record.new_order;
            writer.u64(command.id.value());
            writer.u64(command.account.value());
            writer.u64(command.instrument.value());
            writer.u8(static_cast<std::uint8_t>(command.side));
            writer.u8(static_cast<std::uint8_t>(command.type));
            writer.u8(static_cast<std::uint8_t>(command.tif));
            writer.boolean(command.post_only);
            writer.i64(command.price);
            writer.u64(command.quantity);
            break;
        }
        case RecordType::CancelOrder: {
            const CancelOrder& command = record.cancel_order;
            writer.u64(command.id.value());
            writer.u64(command.account.value());
            writer.u64(command.instrument.value());
            break;
        }
        case RecordType::ReplaceOrder: {
            const ReplaceOrder& command = record.replace_order;
            writer.u64(command.id.value());
            writer.u64(command.account.value());
            writer.u64(command.instrument.value());
            writer.i64(command.new_price);
            writer.u64(command.new_quantity);
            break;
        }
    }
}

bool decode_payload(RecordType type, std::span<const std::uint8_t> payload, Record& record) {
    // Written as a straight run of reads with a single validity check at the
    // end. The reader latches failure, so a short or malformed payload cannot
    // read past its buffer however many fields follow the damage.
    ByteReader reader(payload);
    record.timestamp = reader.i64();

    switch (type) {
        case RecordType::InstrumentDefined: {
            Instrument& instrument = record.instrument;
            instrument.id = InstrumentId{reader.u64()};
            instrument.symbol = reader.string();
            instrument.tick_size = reader.i64();
            instrument.lot_size = reader.u64();
            instrument.min_quantity = reader.u64();
            instrument.display_exponent = reader.u8();
            instrument.self_trade_policy = reader.enumeration<SelfTradePolicy>(
                static_cast<std::uint8_t>(SelfTradePolicy::DecrementBoth));
            break;
        }
        case RecordType::NewOrder: {
            NewOrder& command = record.new_order;
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
        case RecordType::CancelOrder: {
            CancelOrder& command = record.cancel_order;
            command.id = OrderId{reader.u64()};
            command.account = AccountId{reader.u64()};
            command.instrument = InstrumentId{reader.u64()};
            break;
        }
        case RecordType::ReplaceOrder: {
            ReplaceOrder& command = record.replace_order;
            command.id = OrderId{reader.u64()};
            command.account = AccountId{reader.u64()};
            command.instrument = InstrumentId{reader.u64()};
            command.new_price = reader.i64();
            command.new_quantity = reader.u64();
            break;
        }
    }

    // Trailing bytes mean the payload is not one this build understands --
    // most likely a newer sender's record with a field appended. Accepting it
    // would silently drop that field.
    return reader.fully_consumed();
}

bool is_known_type(std::uint8_t type) {
    switch (static_cast<RecordType>(type)) {
        case RecordType::InstrumentDefined:
        case RecordType::NewOrder:
        case RecordType::CancelOrder:
        case RecordType::ReplaceOrder:
            return true;
    }
    return false;
}

}  // namespace

void encode(const Record& record, std::vector<std::uint8_t>& out) {
    const std::size_t header_start = out.size();
    out.resize(header_start + RecordHeader::kSize, 0);
    encode_payload(record, out);

    const std::size_t payload_length = out.size() - header_start - RecordHeader::kSize;

    // Written directly into the reserved header space now that the payload
    // length is known, which avoids encoding the payload into a scratch buffer
    // and copying it.
    std::uint8_t* header = out.data() + header_start;
    const auto store = [](std::uint8_t* dst, std::uint64_t value, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            dst[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU);
        }
    };
    store(header + 0, payload_length, 4);
    store(header + 8, record.sequence, 8);
    header[16] = static_cast<std::uint8_t>(record.type);
    header[17] = kRecordVersion;
    header[18] = 0;
    header[19] = 0;

    // Covers everything from the sequence number onward -- see the layout note
    // on RecordHeader for why the length and the checksum itself are outside it.
    const std::uint32_t checksum =
        crc32c(std::span<const std::uint8_t>(header + 8, out.size() - header_start - 8));
    store(header + 4, checksum, 4);
}

DecodeResult decode(std::span<const std::uint8_t> data) {
    DecodeResult result;

    if (data.size() < RecordHeader::kSize) {
        result.status = DecodeStatus::Incomplete;
        return result;
    }

    ByteReader header(data.first(RecordHeader::kSize));
    const auto payload_length = header.u32();
    const auto checksum = header.u32();
    const auto sequence = header.u64();
    const std::uint8_t type = header.u8();
    const std::uint8_t version = header.u8();
    const std::uint16_t reserved = header.u16();

    if (version != kRecordVersion || !is_known_type(type) || reserved != 0 ||
        payload_length > RecordHeader::kMaxPayload) {
        // Refuses rather than guessing. Misparsing an unrecognised record would
        // produce a plausible but wrong reconstruction, which is worse than
        // producing none.
        result.status = DecodeStatus::Unreadable;
        return result;
    }

    const std::size_t total = RecordHeader::kSize + payload_length;
    if (data.size() < total) {
        result.status = DecodeStatus::Incomplete;
        return result;
    }

    const std::uint32_t computed =
        crc32c(std::span<const std::uint8_t>(data.data() + 8, total - 8));
    if (computed != checksum) {
        result.status = DecodeStatus::Corrupt;
        return result;
    }

    result.record.type = static_cast<RecordType>(type);
    result.record.sequence = sequence;
    if (!decode_payload(result.record.type, data.subspan(RecordHeader::kSize, payload_length),
                        result.record)) {
        // The checksum matched, so the bytes are the ones that were written --
        // but they do not describe a record this build can act on. Treated as
        // unreadable rather than corrupt, because retrying or repairing will
        // not help.
        result.status = DecodeStatus::Unreadable;
        return result;
    }

    result.status = DecodeStatus::Ok;
    result.consumed = total;
    return result;
}

}  // namespace xc::journal
