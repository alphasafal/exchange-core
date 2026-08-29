#include "xc/journal/record.hpp"

#include <cstring>

#include "xc/util/crc32c.hpp"

namespace xc::journal {
namespace {

/// Every integer on disk is little-endian, written byte by byte.
///
/// Not memcpy of a packed struct. Byte-wise encoding is correct on any host
/// regardless of its native endianness or alignment rules, and it makes the
/// layout in the header comment the single source of truth rather than a
/// description of whatever the compiler happened to lay out.
template<typename T>
void put(std::vector<std::uint8_t>& out, T value) {
    static_assert(std::is_unsigned_v<T>, "encode unsigned representations only");
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
    }
}

template<typename T>
T get(std::span<const std::uint8_t> data, std::size_t offset) {
    T value = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        value |= static_cast<T>(data[offset + i]) << (8 * i);
    }
    return value;
}

void put_string(std::vector<std::uint8_t>& out, const std::string& value) {
    put<std::uint16_t>(out, static_cast<std::uint16_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

/// Signed values are encoded through their unsigned representation. The
/// conversion is well defined in both directions in C++20, where signed
/// integers are required to be two's complement.
void put_signed(std::vector<std::uint8_t>& out, std::int64_t value) {
    put<std::uint64_t>(out, static_cast<std::uint64_t>(value));
}

std::int64_t get_signed(std::span<const std::uint8_t> data, std::size_t offset) {
    return static_cast<std::int64_t>(get<std::uint64_t>(data, offset));
}

void encode_payload(const Record& record, std::vector<std::uint8_t>& out) {
    put_signed(out, record.timestamp);

    switch (record.type) {
        case RecordType::InstrumentDefined: {
            const Instrument& instrument = record.instrument;
            put<std::uint64_t>(out, instrument.id.value());
            put_string(out, instrument.symbol);
            put_signed(out, instrument.tick_size);
            put<std::uint64_t>(out, instrument.lot_size);
            put<std::uint64_t>(out, instrument.min_quantity);
            put<std::uint8_t>(out, instrument.display_exponent);
            put<std::uint8_t>(out, static_cast<std::uint8_t>(instrument.self_trade_policy));
            break;
        }
        case RecordType::NewOrder: {
            const NewOrder& command = record.new_order;
            put<std::uint64_t>(out, command.id.value());
            put<std::uint64_t>(out, command.account.value());
            put<std::uint64_t>(out, command.instrument.value());
            put<std::uint8_t>(out, static_cast<std::uint8_t>(command.side));
            put<std::uint8_t>(out, static_cast<std::uint8_t>(command.type));
            put<std::uint8_t>(out, static_cast<std::uint8_t>(command.tif));
            put<std::uint8_t>(out, command.post_only ? 1U : 0U);
            put_signed(out, command.price);
            put<std::uint64_t>(out, command.quantity);
            break;
        }
        case RecordType::CancelOrder: {
            const CancelOrder& command = record.cancel_order;
            put<std::uint64_t>(out, command.id.value());
            put<std::uint64_t>(out, command.account.value());
            put<std::uint64_t>(out, command.instrument.value());
            break;
        }
        case RecordType::ReplaceOrder: {
            const ReplaceOrder& command = record.replace_order;
            put<std::uint64_t>(out, command.id.value());
            put<std::uint64_t>(out, command.account.value());
            put<std::uint64_t>(out, command.instrument.value());
            put_signed(out, command.new_price);
            put<std::uint64_t>(out, command.new_quantity);
            break;
        }
    }
}

bool decode_payload(RecordType type, std::span<const std::uint8_t> payload, Record& record) {
    constexpr std::size_t kTimestampSize = 8;
    if (payload.size() < kTimestampSize) {
        return false;
    }
    record.timestamp = get_signed(payload, 0);
    std::size_t offset = kTimestampSize;

    const auto remaining = [&] { return payload.size() - offset; };

    switch (type) {
        case RecordType::InstrumentDefined: {
            if (remaining() < 8 + 2) {
                return false;
            }
            record.instrument.id = InstrumentId{get<std::uint64_t>(payload, offset)};
            offset += 8;
            const auto symbol_length = get<std::uint16_t>(payload, offset);
            offset += 2;
            if (remaining() < symbol_length + std::size_t{8 + 8 + 8 + 1 + 1}) {
                return false;
            }
            record.instrument.symbol.assign(reinterpret_cast<const char*>(payload.data() + offset),
                                            symbol_length);
            offset += symbol_length;
            record.instrument.tick_size = get_signed(payload, offset);
            offset += 8;
            record.instrument.lot_size = get<std::uint64_t>(payload, offset);
            offset += 8;
            record.instrument.min_quantity = get<std::uint64_t>(payload, offset);
            offset += 8;
            record.instrument.display_exponent = payload[offset];
            offset += 1;
            const std::uint8_t policy = payload[offset];
            if (policy > static_cast<std::uint8_t>(SelfTradePolicy::DecrementBoth)) {
                return false;
            }
            record.instrument.self_trade_policy = static_cast<SelfTradePolicy>(policy);
            return true;
        }
        case RecordType::NewOrder: {
            if (remaining() < 8 + 8 + 8 + 4 + 8 + 8) {
                return false;
            }
            NewOrder& command = record.new_order;
            command.id = OrderId{get<std::uint64_t>(payload, offset)};
            offset += 8;
            command.account = AccountId{get<std::uint64_t>(payload, offset)};
            offset += 8;
            command.instrument = InstrumentId{get<std::uint64_t>(payload, offset)};
            offset += 8;

            // Enumerations are range-checked rather than cast blindly. A byte
            // outside the enumeration's range would otherwise become a value no
            // switch handles, and the resulting behaviour would depend on which
            // branch happened to fall through.
            const std::uint8_t side = payload[offset++];
            const std::uint8_t order_type = payload[offset++];
            const std::uint8_t tif = payload[offset++];
            const std::uint8_t post_only = payload[offset++];
            if (side > static_cast<std::uint8_t>(Side::Sell) ||
                order_type > static_cast<std::uint8_t>(OrderType::Market) ||
                tif > static_cast<std::uint8_t>(TimeInForce::GoodTilCancelled) || post_only > 1) {
                return false;
            }
            command.side = static_cast<Side>(side);
            command.type = static_cast<OrderType>(order_type);
            command.tif = static_cast<TimeInForce>(tif);
            command.post_only = post_only == 1;

            command.price = get_signed(payload, offset);
            offset += 8;
            command.quantity = get<std::uint64_t>(payload, offset);
            return true;
        }
        case RecordType::CancelOrder: {
            if (remaining() < 8 + 8 + 8) {
                return false;
            }
            CancelOrder& command = record.cancel_order;
            command.id = OrderId{get<std::uint64_t>(payload, offset)};
            offset += 8;
            command.account = AccountId{get<std::uint64_t>(payload, offset)};
            offset += 8;
            command.instrument = InstrumentId{get<std::uint64_t>(payload, offset)};
            return true;
        }
        case RecordType::ReplaceOrder: {
            if (remaining() < 8 + 8 + 8 + 8 + 8) {
                return false;
            }
            ReplaceOrder& command = record.replace_order;
            command.id = OrderId{get<std::uint64_t>(payload, offset)};
            offset += 8;
            command.account = AccountId{get<std::uint64_t>(payload, offset)};
            offset += 8;
            command.instrument = InstrumentId{get<std::uint64_t>(payload, offset)};
            offset += 8;
            command.new_price = get_signed(payload, offset);
            offset += 8;
            command.new_quantity = get<std::uint64_t>(payload, offset);
            return true;
        }
    }
    return false;
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

    const auto payload_length = get<std::uint32_t>(data, 0);
    const auto checksum = get<std::uint32_t>(data, 4);
    const auto sequence = get<std::uint64_t>(data, 8);
    const std::uint8_t type = data[16];
    const std::uint8_t version = data[17];
    const std::uint16_t reserved = get<std::uint16_t>(data, 18);

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
