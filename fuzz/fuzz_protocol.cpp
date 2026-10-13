/// Fuzzes the wire decoder on arbitrary bytes.
///
/// The decoder is the only part of this system that reads input written by
/// someone else. Everything downstream -- the engine, the risk checks, the
/// journal -- is reachable only through it, so a decoder that can be made to
/// read past a buffer or hand up a value it never saw is the whole venue's
/// problem rather than the protocol layer's.
///
/// The oracle is not "does it decode". Most random bytes are not a message and
/// should be rejected; that is the expected outcome, not a finding. What is
/// checked here is that rejection is *well-behaved*: no read outside the
/// buffer (ASan), no undefined behaviour on the way (UBSan), a consumed count
/// that stays inside the input, and a decoder that cannot report success while
/// its reader is in a failed state.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "xc/protocol/gap_detector.hpp"
#include "xc/protocol/messages.hpp"

namespace {

using xc::protocol::DecodeResult;
using xc::protocol::DecodeStatus;
using xc::protocol::FrameHeader;

/// True only for the types this build defines. A decoder that hands up
/// anything else has produced a value that the first switch downstream will
/// fall off the end of.
bool is_known(xc::protocol::MessageType type) {
    switch (type) {
        case xc::protocol::MessageType::NewOrder:
        case xc::protocol::MessageType::CancelOrder:
        case xc::protocol::MessageType::ReplaceOrder:
        case xc::protocol::MessageType::OrderAccepted:
        case xc::protocol::MessageType::OrderRejected:
        case xc::protocol::MessageType::OrderCancelled:
        case xc::protocol::MessageType::OrderReplaced:
        case xc::protocol::MessageType::Execution:
        case xc::protocol::MessageType::Trade:
        case xc::protocol::MessageType::TopOfBook:
        case xc::protocol::MessageType::DepthSnapshot:
        case xc::protocol::MessageType::Heartbeat:
            return true;
    }
    return false;
}

void check(bool condition, const char* what) {
    if (!condition) {
        // abort() rather than assert(): NDEBUG is set in this build type, and a
        // fuzz oracle that an optimiser is allowed to delete is not an oracle.
        std::fputs(what, stderr);
        std::fputc('\n', stderr);
        std::abort();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::span<const std::uint8_t> input(data, size);

    // Decode repeatedly, the way a reader drains a TCP buffer. One call only
    // would never reach the second message in a frame-boundary bug.
    std::size_t offset = 0;
    xc::protocol::GapDetector gaps;
    int decoded = 0;

    while (offset <= size) {
        const std::span<const std::uint8_t> remaining = input.subspan(offset);
        const DecodeResult result = xc::protocol::decode(remaining);

        if (result.status != DecodeStatus::Ok) {
            // A decoder that reports failure must not also claim to have
            // consumed anything: a caller that trusted the count would skip
            // bytes it never successfully read, and resynchronise onto a
            // boundary that was never there.
            check(result.consumed == 0, "non-zero consumed on a failed decode");
            break;
        }

        check(result.consumed >= FrameHeader::kSize, "a frame cannot be shorter than its header");
        check(result.consumed <= remaining.size(), "consumed more bytes than were supplied");
        check(is_known(result.message.type), "decoded a message type this build does not define");

        // Re-encoding is not checked here. The decoder accepts fields the
        // encoder would never produce -- a depth snapshot's level count is
        // bounded on the way in, not on the way out -- so round-trip equality
        // is a property of generated messages, which test_protocol.cpp covers,
        // and not of arbitrary input.

        // Whatever came out has to be safe to feed onward; the gap detector is
        // the first thing a subscriber does with a decoded header.
        static_cast<void>(gaps.observe(result.message.sequence));

        offset += result.consumed;
        if (++decoded > 4096) {
            break;  // a long run of empty heartbeats need not be followed forever
        }
    }

    check(offset <= size, "decoding walked past the end of the input");
    return 0;
}
