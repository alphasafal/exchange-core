# Benchmarks link the uninstrumented library on purpose: the allocation counter
# lives in a separate target that only the tests use, so a measurement can never
# accidentally include the cost of measuring allocations.
add_executable(bench_matching bench_matching.cpp)
target_link_libraries(bench_matching PRIVATE xc::core)
