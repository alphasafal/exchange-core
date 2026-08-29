#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xc {

/// Writes fixed-width little-endian integers into a byte buffer.
///
/// Byte-wise rather than a memcpy of a packed struct, so the documented layout
/// is the single source of truth rather than a description of whatever the
/// compiler laid out, and the encoding is identical on any host regardless of
/// its native byte order or alignment rules.
class ByteWriter {
  public:
    explicit ByteWriter(std::vector<std::uint8_t>& out) : out_(out), start_(out.size()) {}

    void u8(std::uint8_t value) { out_.push_back(value); }
    void u16(std::uint16_t value) { put(value, 2); }
    void u32(std::uint32_t value) { put(value, 4); }
    void u64(std::uint64_t value) { put(value, 8); }

    /// Signed values travel as their unsigned representation. The conversion is
    /// well defined in both directions in C++20, which requires signed integers
    /// to be two's complement.
    void i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

    void boolean(bool value) { u8(value ? 1U : 0U); }

    /// A 16-bit length followed by the bytes.
    void string(std::string_view value) {
        u16(static_cast<std::uint16_t>(value.size()));
        out_.insert(out_.end(), value.begin(), value.end());
    }

    /// Bytes written by this writer.
    std::size_t written() const noexcept { return out_.size() - start_; }

  private:
    void put(std::uint64_t value, std::size_t width) {
        for (std::size_t i = 0; i < width; ++i) {
            out_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
        }
    }

    std::vector<std::uint8_t>& out_;
    std::size_t start_;
};

/// Reads what ByteWriter wrote, from input that may be malformed.
///
/// Failure is sticky rather than thrown or returned per call. A read that would
/// run past the end sets a flag, and every subsequent read returns zero without
/// touching memory -- so a decoder can be written as a straight run of reads
/// followed by one `ok()` check at the end.
///
/// That matters for more than tidiness. A decoder that must test bounds after
/// every field is a decoder where one missing test is a buffer overrun, and
/// this parses bytes that arrived over a socket from a party the venue does not
/// control. Making the safe shape the natural one is worth more here than
/// saving a branch.
class ByteReader {
  public:
    explicit ByteReader(std::span<const std::uint8_t> data) : data_(data) {}

    std::uint8_t u8() {
        if (!request(1)) {
            return 0;
        }
        return data_[offset_++];
    }

    std::uint16_t u16() { return static_cast<std::uint16_t>(take(2)); }
    std::uint32_t u32() { return static_cast<std::uint32_t>(take(4)); }
    std::uint64_t u64() { return take(8); }
    std::int64_t i64() { return static_cast<std::int64_t>(take(8)); }

    /// Reads a boolean, failing on any encoding other than 0 or 1. A byte that
    /// is neither did not come from this encoder, and accepting it would let a
    /// malformed message become a plausible one.
    bool boolean() {
        const std::uint8_t value = u8();
        if (value > 1) {
            failed_ = true;
            return false;
        }
        return value == 1;
    }

    std::string string() {
        const std::uint16_t length = u16();
        if (!request(length)) {
            return {};
        }
        const auto* begin = reinterpret_cast<const char*>(data_.data() + offset_);
        offset_ += length;
        return std::string(begin, length);
    }

    /// Reads an enumeration, failing when the value is outside its range.
    ///
    /// Casting an out-of-range byte to an enum produces a value no switch
    /// handles, and the resulting behaviour depends on which branch happens to
    /// fall through -- an especially poor way to handle input from the network.
    template<typename Enum>
    Enum enumeration(std::uint8_t highest) {
        const std::uint8_t value = u8();
        if (value > highest) {
            failed_ = true;
            return static_cast<Enum>(0);
        }
        return static_cast<Enum>(value);
    }

    /// False once any read has run past the end or rejected a value.
    bool ok() const noexcept { return !failed_; }

    std::size_t consumed() const noexcept { return offset_; }
    std::size_t remaining() const noexcept { return failed_ ? 0 : data_.size() - offset_; }

    /// True when every byte was consumed and nothing failed. A message with
    /// bytes left over is not one this build understands, and treating it as
    /// valid would silently ignore a field a newer sender meant to be read.
    bool fully_consumed() const noexcept { return !failed_ && offset_ == data_.size(); }

  private:
    bool request(std::size_t bytes) {
        if (failed_ || data_.size() - offset_ < bytes) {
            failed_ = true;
            return false;
        }
        return true;
    }

    std::uint64_t take(std::size_t width) {
        if (!request(width)) {
            return 0;
        }
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i) {
            value |= static_cast<std::uint64_t>(data_[offset_ + i]) << (8 * i);
        }
        offset_ += width;
        return value;
    }

    std::span<const std::uint8_t> data_;
    std::size_t offset_ = 0;
    bool failed_ = false;
};

}  // namespace xc
