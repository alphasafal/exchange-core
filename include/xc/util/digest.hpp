#pragma once

#include <cstdint>
#include <string_view>

namespace xc {

/// A 64-bit FNV-1a hash, used to fingerprint engine state.
///
/// Not a cryptographic hash and not claimed to be: nothing here defends against
/// an adversary choosing inputs to collide. The requirement is different --
/// that two runs which produce the same state produce the same fingerprint, and
/// that a single changed field in a single order changes it. FNV-1a is
/// specified precisely enough that the value is identical across compilers and
/// platforms, which matters because a determinism check whose expected value
/// depends on the machine proves nothing.
class Digest {
  public:
    void feed(std::uint64_t value) noexcept {
        // Fed byte by byte, least significant first, so the result does not
        // depend on the host's byte order.
        for (int i = 0; i < 8; ++i) {
            feed_byte(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFU));
        }
    }

    void feed(std::int64_t value) noexcept { feed(static_cast<std::uint64_t>(value)); }
    void feed(std::uint32_t value) noexcept { feed(static_cast<std::uint64_t>(value)); }
    void feed(std::uint8_t value) noexcept { feed_byte(value); }
    void feed(bool value) noexcept { feed_byte(value ? 1U : 0U); }

    void feed(std::string_view text) noexcept {
        feed(static_cast<std::uint64_t>(text.size()));
        for (const char c : text) {
            feed_byte(static_cast<std::uint8_t>(c));
        }
    }

    std::uint64_t value() const noexcept { return state_; }

  private:
    void feed_byte(std::uint8_t byte) noexcept {
        state_ ^= byte;
        state_ *= kPrime;
    }

    static constexpr std::uint64_t kOffsetBasis = 1469598103934665603ULL;
    static constexpr std::uint64_t kPrime = 1099511628211ULL;

    std::uint64_t state_ = kOffsetBasis;
};

}  // namespace xc
