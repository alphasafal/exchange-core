#pragma once

#include <cstddef>
#include <cstdint>

namespace xc {

/// Counts calls to global operator new and delete.
///
/// The matching path claims to allocate nothing in steady state. That claim is
/// easy to make, easy to believe, and easy to break: one std::string, one
/// vector that outgrows its reservation, one std::function assigned in the
/// wrong place, and it is quietly false again with no visible symptom until a
/// tail latency measurement six months later.
///
/// Counting the allocator directly turns it into something a test can assert.
/// The counters are enabled only when XC_COUNT_ALLOCATIONS is defined -- the
/// test build -- so production binaries carry no instrumentation at all.
///
/// Not thread-safe, and deliberately so: the matching thread is the only thing
/// being measured, and making the counters atomic would add exactly the kind of
/// contention the measurement is trying to detect.
struct AllocationCounters {
    std::uint64_t allocations = 0;
    std::uint64_t deallocations = 0;
    std::uint64_t bytes_allocated = 0;
};

/// The process-wide counters.
AllocationCounters& allocation_counters();

/// True when this build actually counts. False in a normal build, where the
/// counters stay at zero -- so a test must check this rather than concluding
/// from a count of zero that nothing allocated.
bool allocation_counting_enabled();

/// Records the counters on construction and reports what happened since.
class AllocationScope {
  public:
    AllocationScope() : start_(allocation_counters()) {}

    std::uint64_t allocations() const {
        return allocation_counters().allocations - start_.allocations;
    }
    std::uint64_t bytes() const {
        return allocation_counters().bytes_allocated - start_.bytes_allocated;
    }

    void reset() { start_ = allocation_counters(); }

  private:
    AllocationCounters start_;
};

}  // namespace xc
