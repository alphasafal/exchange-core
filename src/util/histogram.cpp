#include "xc/util/histogram.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <limits>

namespace xc {

Histogram::Histogram(std::uint64_t max_value, std::uint32_t significant_bits)
    : significant_bits_(significant_bits < 2 ? 2 : (significant_bits > 20 ? 20 : significant_bits)),
      sub_bucket_count_(std::uint64_t{1} << significant_bits_),
      sub_bucket_half_count_(sub_bucket_count_ / 2),
      sub_bucket_mask_(sub_bucket_count_ - 1),
      max_value_(max_value < sub_bucket_count_ ? sub_bucket_count_ : max_value) {
    counts_.assign(index_for(max_value_) + 1, 0);
}

std::size_t Histogram::index_for(std::uint64_t value) const {
    // The OR floors the magnitude at the sub-bucket range, so small values all
    // land in bucket zero rather than needing a special case.
    const std::uint64_t masked = value | sub_bucket_mask_;
    const auto magnitude = static_cast<std::uint32_t>(63 - std::countl_zero(masked));
    const std::uint32_t bucket =
        magnitude >= significant_bits_ - 1 ? magnitude - (significant_bits_ - 1) : 0;
    const std::uint64_t sub = value >> bucket;

    if (bucket == 0) {
        return static_cast<std::size_t>(sub);
    }
    return static_cast<std::size_t>((bucket + 1) * sub_bucket_half_count_ +
                                    (sub - sub_bucket_half_count_));
}

std::uint64_t Histogram::lowest_value_at(std::size_t index) const {
    if (index < sub_bucket_count_) {
        return index;
    }
    const std::uint64_t bucket = index / sub_bucket_half_count_ - 1;
    const std::uint64_t sub = (index % sub_bucket_half_count_) + sub_bucket_half_count_;
    return sub << bucket;
}

std::uint64_t Histogram::highest_value_at(std::size_t index) const {
    if (index < sub_bucket_count_) {
        return index;
    }
    const std::uint64_t bucket = index / sub_bucket_half_count_ - 1;
    // Every value in a bucket shares its count, so the highest one it could
    // represent is the lowest plus the bucket's width less one.
    return lowest_value_at(index) + (std::uint64_t{1} << bucket) - 1;
}

void Histogram::record(std::uint64_t value) {
    if (value > max_value_) {
        // Counted, never clamped: a clamped outlier looks identical to a sample
        // that really landed at the top of the range, which would let the
        // histogram understate its own worst case.
        ++overflow_;
        if (value > max_) {
            max_ = value;
        }
        return;
    }

    ++counts_[index_for(value)];
    ++count_;
    total_ += value;
    if (value > max_) {
        max_ = value;
    }
    if (count_ == 1 || value < min_) {
        min_ = value;
    }
}

std::uint64_t Histogram::value_at(double percentile) const {
    if (count_ == 0) {
        return 0;
    }
    if (percentile <= 0.0) {
        return min_;
    }
    if (percentile >= 100.0 || overflow_ > 0) {
        // With samples above the range there is no bucket that can answer the
        // top of the distribution, so the true maximum is the only honest
        // answer available.
        if (percentile >= 100.0) {
            return max_;
        }
    }

    const auto target = static_cast<std::uint64_t>(
        (percentile / 100.0) * static_cast<double>(count_ + overflow_) + 0.5);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        seen += counts_[i];
        if (seen >= target) {
            // The highest value the bucket could hold, so a reported latency is
            // never lower than the true one -- but capped at the largest sample
            // actually observed, since a bucket's ceiling can sit above it and
            // reporting a latency that never occurred is its own kind of
            // inaccuracy. Both bounds are at least the true percentile, so
            // taking the smaller keeps the guarantee.
            return std::min(highest_value_at(i), max_);
        }
    }
    return max_;
}

double Histogram::mean() const noexcept {
    if (count_ == 0) {
        return 0.0;
    }
    return static_cast<double>(total_) / static_cast<double>(count_);
}

void Histogram::reset() {
    counts_.assign(counts_.size(), 0);
    count_ = 0;
    total_ = 0;
    min_ = 0;
    max_ = 0;
    overflow_ = 0;
}

void Histogram::merge(const Histogram& other) {
    assert(counts_.size() == other.counts_.size() && "histograms must have the same shape");
    for (std::size_t i = 0; i < counts_.size(); ++i) {
        counts_[i] += other.counts_[i];
    }
    count_ += other.count_;
    total_ += other.total_;
    overflow_ += other.overflow_;
    if (other.max_ > max_) {
        max_ = other.max_;
    }
    if (count_ == other.count_ || (other.count_ > 0 && other.min_ < min_)) {
        min_ = other.min_;
    }
}

}  // namespace xc
