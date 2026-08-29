#include "xc/util/crc32c.hpp"

#include <array>

namespace xc {
namespace {

constexpr std::uint32_t kReflectedPolynomial = 0x82F63B78U;

/// Built at compile time so there is no initialisation order to reason about
/// and no first-call cost.
constexpr std::array<std::uint32_t, 256> make_table() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t value = i;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1U) != 0U ? (value >> 1U) ^ kReflectedPolynomial : value >> 1U;
        }
        table[i] = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> kTable = make_table();

}  // namespace

std::uint32_t crc32c(std::span<const std::uint8_t> data, std::uint32_t seed) {
    std::uint32_t crc = ~seed;
    for (const std::uint8_t byte : data) {
        crc = kTable[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
    }
    return ~crc;
}

}  // namespace xc
