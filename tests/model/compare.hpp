#pragma once

#include <string>
#include <vector>

#include "model/reference_book.hpp"
#include "xc/core/fill.hpp"
#include "xc/core/order_book.hpp"

namespace xc::model {

/// Comparisons between the optimised book and the reference model.
///
/// These live apart from the differential test because two harnesses use them:
/// that test, which walks seeded streams, and fuzz_order_flow, which lets
/// libFuzzer choose the stream instead. Keeping one implementation is the
/// point -- a fuzzer checking a slightly different notion of "the same" from
/// the test it claims to extend is a fuzzer that can pass while the thing it
/// is supposed to guard is broken.
///
/// Each returns an empty string when the two agree, and otherwise a
/// description of the first difference found, ready to print.

std::string diff_fills(const std::vector<Fill>& engine, const std::vector<Fill>& model);

/// Compares every resting order in matching order, then top of book, then
/// aggregated depth. Order-by-order equality alone would miss an aggregate
/// that drifted from the orders it summarises, which is a real failure mode
/// here: the level totals are maintained incrementally.
std::string diff_books(const OrderBook& engine, const ReferenceBook& model);

}  // namespace xc::model
