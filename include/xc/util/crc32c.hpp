#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace xc {

/// CRC-32C (Castagnoli, polynomial 0x1EDC6F41).
///
/// Chosen over the CRC-32 used by zip and gzip because Castagnoli detects more
/// of the multi-bit error patterns that storage actually produces, and because
/// it is the polynomial with hardware support on both x86 (SSE4.2) and ARMv8 --
/// so a future implementation can go faster without changing a single byte on
/// disk.
///
/// This implementation is a portable table-driven one. It is not the fastest
/// available, and it is not claimed to be: journal checksums are computed once
/// per record on a path already dominated by the write itself, and a
/// hand-written intrinsic path would need its own correctness argument on two
/// architectures to save time that is not being spent.
std::uint32_t crc32c(std::span<const std::uint8_t> data, std::uint32_t seed = 0);

}  // namespace xc
