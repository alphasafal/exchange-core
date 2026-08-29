#include "xc/util/allocation_counter.hpp"

#include <cstdlib>
#include <new>

namespace xc {

AllocationCounters& allocation_counters() {
    // A function-local static rather than a namespace-scope object: the
    // replacement operator new below can run before namespace-scope
    // construction would have happened, and reading an object that has not been
    // constructed yet is undefined behaviour that usually appears to work.
    static AllocationCounters counters;
    return counters;
}

bool allocation_counting_enabled() {
#ifdef XC_COUNT_ALLOCATIONS
    return true;
#else
    return false;
#endif
}

}  // namespace xc

#ifdef XC_COUNT_ALLOCATIONS

// Replacing the global allocator rather than wrapping one type's. A wrapper
// only sees allocations someone remembered to route through it, which makes it
// blind to precisely the accidental ones worth finding.
void* operator new(std::size_t size) {
    xc::AllocationCounters& counters = xc::allocation_counters();
    ++counters.allocations;
    counters.bytes_allocated += size;
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    xc::AllocationCounters& counters = xc::allocation_counters();
    ++counters.allocations;
    counters.bytes_allocated += size;
    return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return operator new(size, tag);
}

void operator delete(void* memory) noexcept {
    if (memory != nullptr) {
        ++xc::allocation_counters().deallocations;
    }
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    operator delete(memory);
}

// The sized deletes must be replaced too. If they are left to the default
// implementation the counts do not balance, and a leak check built on them
// would report leaks that are not there.
void operator delete(void* memory, std::size_t) noexcept {
    operator delete(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    operator delete(memory);
}

void operator delete(void* memory, const std::nothrow_t&) noexcept {
    operator delete(memory);
}

void operator delete[](void* memory, const std::nothrow_t&) noexcept {
    operator delete(memory);
}

#endif  // XC_COUNT_ALLOCATIONS
