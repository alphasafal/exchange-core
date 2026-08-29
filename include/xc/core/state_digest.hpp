#pragma once

#include <cstdint>

namespace xc {

class MatchingEngine;
class OrderBook;

/// Fingerprints an order book: every resting order, in matching order.
///
/// Covers the fields that decide future behaviour -- identity, ownership, side,
/// price, quantity, remaining quantity and sequence -- and the order they would
/// be matched in. It deliberately does not cover anything derived, such as
/// level totals or depth, because a digest that included them could report a
/// difference that no future command could ever expose.
std::uint64_t digest(const OrderBook& book);

/// Fingerprints an entire engine: its command sequence and every book, walked
/// in instrument-id order.
///
/// Instruments are ordered explicitly rather than iterated from the hash map
/// they live in. Hash iteration order is unspecified and can differ between
/// runs of the same binary, which would make the digest disagree with itself
/// and turn a determinism check into a coin toss.
std::uint64_t digest(const MatchingEngine& engine);

}  // namespace xc
