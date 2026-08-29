#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xc {

/// A log-linear latency histogram.
///
/// Latency distributions here span several orders of magnitude and the
/// interesting part is the tail, so the two obvious structures are both wrong.
/// Linear buckets fine enough to resolve a sub-microsecond median need millions
/// of buckets to reach a millisecond outlier. Keeping every sample instead
/// costs memory proportional to the run and makes a percentile a sort.
///
/// This stores counts in buckets whose width grows with magnitude: constant
/// *relative* precision across the whole range, in bounded memory, with
/// recording that is a few instructions and no allocation. The construction is
/// the one HdrHistogram uses.
///
/// Percentiles are reported as the highest value in their bucket rather than
/// the lowest, so a reported latency is never lower than the true one. When a
/// number could be wrong in either direction, the direction that flatters the
/// system is the wrong one to choose.
class Histogram {
  public:
    /// `significant_bits` sets the relative precision: 3 bits gives about 6%,
    /// and the default of 10 gives about 0.1%, which is finer than the
    /// measurement noise on any timer this project reads.
    explicit Histogram(std::uint64_t max_value = 60'000'000'000ULL,
                       std::uint32_t significant_bits = 10);

    void record(std::uint64_t value);

    /// Value below which `percentile` percent of samples fall. `percentile` is
    /// given as a percentage, so 99.9 means the 99.9th.
    std::uint64_t value_at(double percentile) const;

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t min() const noexcept { return count_ == 0 ? 0 : min_; }
    std::uint64_t max() const noexcept { return max_; }

    /// Arithmetic mean. Reported alongside percentiles and never instead of
    /// them: a mean hides exactly the tail this histogram exists to show.
    double mean() const noexcept;

    /// Samples above the configured maximum.
    ///
    /// Counted rather than clamped. A clamped outlier is indistinguishable from
    /// a sample that genuinely landed at the top of the range, which would let
    /// the histogram quietly understate its own worst case.
    std::uint64_t overflow_count() const noexcept { return overflow_; }

    void reset();

    /// Merges another histogram of the same shape, for combining per-thread
    /// results.
    void merge(const Histogram& other);

  private:
    std::size_t index_for(std::uint64_t value) const;
    std::uint64_t lowest_value_at(std::size_t index) const;
    std::uint64_t highest_value_at(std::size_t index) const;

    std::uint32_t significant_bits_;
    std::uint64_t sub_bucket_count_;
    std::uint64_t sub_bucket_half_count_;
    std::uint64_t sub_bucket_mask_;
    std::uint64_t max_value_;

    std::vector<std::uint64_t> counts_;
    std::uint64_t count_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t min_ = 0;
    std::uint64_t max_ = 0;
    std::uint64_t overflow_ = 0;
};

}  // namespace xc
