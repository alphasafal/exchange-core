#include <gtest/gtest.h>

#include <algorithm>
#include <random>
#include <vector>

#include "xc/util/histogram.hpp"

namespace xc {
namespace {

/// The percentile computed by sorting every sample -- the definition the
/// histogram is an approximation of.
std::uint64_t exact_percentile(std::vector<std::uint64_t> samples, double percentile) {
    std::sort(samples.begin(), samples.end());
    const auto rank =
        static_cast<std::size_t>((percentile / 100.0) * static_cast<double>(samples.size()) + 0.5);
    return samples[std::min(rank == 0 ? 0 : rank - 1, samples.size() - 1)];
}

TEST(Histogram, IsEmptyBeforeAnythingIsRecorded) {
    const Histogram histogram;
    EXPECT_EQ(histogram.count(), 0u);
    EXPECT_EQ(histogram.value_at(50.0), 0u);
    EXPECT_EQ(histogram.mean(), 0.0);
}

TEST(Histogram, RecordsSmallValuesExactly) {
    Histogram histogram(1'000'000, 10);
    for (std::uint64_t value = 0; value < 1024; ++value) {
        histogram.record(value);
    }
    // Below the sub-bucket count every value has its own bucket, so there is no
    // approximation at all in the range where most latencies land.
    EXPECT_EQ(histogram.value_at(0.0), 0u);
    EXPECT_EQ(histogram.min(), 0u);
    EXPECT_EQ(histogram.max(), 1023u);
    EXPECT_EQ(histogram.count(), 1024u);
}

TEST(Histogram, ReportsTheMedianOfAUniformSpread) {
    Histogram histogram;
    for (std::uint64_t value = 1; value <= 10'000; ++value) {
        histogram.record(value);
    }
    const std::uint64_t median = histogram.value_at(50.0);
    EXPECT_NEAR(static_cast<double>(median), 5000.0, 50.0);
}

TEST(Histogram, NeverUnderstatesAPercentile) {
    std::mt19937_64 rng(1234);
    std::vector<std::uint64_t> samples;
    samples.reserve(200'000);
    Histogram histogram;

    // A realistic latency shape: a tight body with a long tail.
    for (int i = 0; i < 200'000; ++i) {
        const std::uint64_t value = (i % 1000 == 0) ? 50'000 + rng() % 500'000 : 400 + rng() % 300;
        samples.push_back(value);
        histogram.record(value);
    }

    for (const double percentile : {50.0, 90.0, 99.0, 99.9}) {
        const std::uint64_t exact = exact_percentile(samples, percentile);
        const std::uint64_t reported = histogram.value_at(percentile);
        // Buckets are reported at their highest value, so the answer may be
        // slightly high and must never be low. A latency figure that errs
        // towards flattering the system is the one kind that is not acceptable.
        EXPECT_GE(reported, exact) << "at p" << percentile;
        EXPECT_LE(static_cast<double>(reported), static_cast<double>(exact) * 1.01 + 2)
            << "at p" << percentile << ": precision should be about 0.1%";
    }
}

TEST(Histogram, SeparatesTheTailFromTheBody) {
    Histogram histogram;
    for (int i = 0; i < 99'000; ++i) {
        histogram.record(500);
    }
    for (int i = 0; i < 1'000; ++i) {
        histogram.record(1'000'000);
    }

    // The whole reason for percentiles: the mean here is about 10,500 and
    // describes nothing that ever happened. The p99 and the max tell the truth.
    EXPECT_NEAR(histogram.mean(), 10'495.0, 100.0);
    EXPECT_EQ(histogram.value_at(50.0), 500u);
    EXPECT_EQ(histogram.value_at(98.0), 500u);
    EXPECT_GE(histogram.value_at(99.5), 1'000'000u);
    EXPECT_EQ(histogram.max(), 1'000'000u);
}

TEST(Histogram, PercentilesIncreaseMonotonically) {
    std::mt19937_64 rng(77);
    Histogram histogram;
    for (int i = 0; i < 50'000; ++i) {
        histogram.record(rng() % 1'000'000);
    }

    std::uint64_t previous = 0;
    for (const double percentile : {1.0, 10.0, 25.0, 50.0, 75.0, 90.0, 99.0, 99.9, 99.99}) {
        const std::uint64_t value = histogram.value_at(percentile);
        EXPECT_GE(value, previous) << "p" << percentile << " went backwards";
        previous = value;
    }
    EXPECT_LE(previous, histogram.max()) << "no percentile may exceed a value that occurred";
}

TEST(Histogram, CountsSamplesAboveItsRangeRatherThanClampingThem) {
    Histogram histogram(1000, 10);
    for (int i = 0; i < 10; ++i) {
        histogram.record(500);
    }
    histogram.record(999'999);

    // A clamped outlier is indistinguishable from a sample that genuinely
    // landed at the top of the range, which is how a histogram ends up
    // understating its own worst case.
    EXPECT_EQ(histogram.overflow_count(), 1u);
    EXPECT_EQ(histogram.count(), 10u) << "in-range samples only";
    EXPECT_EQ(histogram.max(), 999'999u) << "but the true maximum is still known";
    EXPECT_EQ(histogram.value_at(100.0), 999'999u);
}

TEST(Histogram, TracksMinimumAndMaximum) {
    Histogram histogram;
    histogram.record(700);
    histogram.record(50);
    histogram.record(90'000);
    EXPECT_EQ(histogram.min(), 50u);
    EXPECT_EQ(histogram.max(), 90'000u);
}

TEST(Histogram, MergesTwoHistograms) {
    Histogram left;
    Histogram right;
    for (int i = 0; i < 1000; ++i) {
        left.record(100);
        right.record(9000);
    }

    left.merge(right);
    EXPECT_EQ(left.count(), 2000u);
    EXPECT_EQ(left.min(), 100u);
    EXPECT_EQ(left.max(), 9000u);
    EXPECT_EQ(left.value_at(25.0), 100u);
    EXPECT_GE(left.value_at(75.0), 9000u);
}

TEST(Histogram, ResetsCompletely) {
    Histogram histogram;
    for (int i = 0; i < 100; ++i) {
        histogram.record(500);
    }
    histogram.reset();
    EXPECT_EQ(histogram.count(), 0u);
    EXPECT_EQ(histogram.max(), 0u);
    EXPECT_EQ(histogram.value_at(99.0), 0u);
}

TEST(Histogram, AbsorbsAWholeRangeOfValuesWithoutResizing) {
    Histogram histogram;
    // Buckets are sized once at construction. Recording on the measurement path
    // must not allocate, or the instrument perturbs what it measures; the
    // allocation-counting test asserts that directly.
    for (std::uint64_t i = 1; i <= 200'000; ++i) {
        histogram.record((i * 37) % 5'000'000);
    }
    EXPECT_EQ(histogram.count(), 200'000u);
    EXPECT_EQ(histogram.overflow_count(), 0u);
    EXPECT_LE(histogram.value_at(99.9), histogram.max());
}

}  // namespace
}  // namespace xc
