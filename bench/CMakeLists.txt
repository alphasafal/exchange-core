# Benchmarks link the uninstrumented library on purpose: the allocation counter
# lives in a separate target that only the tests use, so a measurement can never
# accidentally include the cost of measuring allocations.
add_executable(bench_matching bench_matching.cpp)
target_link_libraries(bench_matching PRIVATE xc::core)

add_executable(bench_wire bench_wire.cpp)
target_link_libraries(bench_wire PRIVATE xc::core)
find_package(Threads REQUIRED)
target_link_libraries(bench_wire PRIVATE Threads::Threads)

# The comparison is opt-in: it downloads a third-party engine, and the normal
# build must never depend on the network. Pinned to an exact commit so a
# published comparison can be reproduced against the same code.
if(XC_BENCH_LIQUIBOOK)
    include(FetchContent)
    FetchContent_Declare(liquibook
        GIT_REPOSITORY https://github.com/enewhuis/liquibook.git
        GIT_TAG 2427613b32f1667abae68a01df6af9ba8270f8e7)
    FetchContent_GetProperties(liquibook)
    if(NOT liquibook_POPULATED)
        FetchContent_Populate(liquibook)
    endif()
    add_executable(bench_liquibook compare/bench_liquibook.cpp)
    target_link_libraries(bench_liquibook PRIVATE xc::core)
    # Third-party headers: include as system so this project's -Werror flags
    # judge our code, not theirs.
    target_include_directories(bench_liquibook SYSTEM PRIVATE ${liquibook_SOURCE_DIR}/src)
endif()
