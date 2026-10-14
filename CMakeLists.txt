cmake_minimum_required(VERSION 3.22)
project(exchange_core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
# Warnings-as-errors is off by default so that a consumer on an unfamiliar
# compiler can still build the library. Every preset and every CI job turns it
# on, which is where it actually earns its keep.
option(XC_WERROR       "Treat warnings as errors"                      OFF)
option(XC_NATIVE_ARCH  "Optimise for the building machine's CPU"       OFF)
option(XC_BUILD_TESTS  "Build the test suite"                          ON)
option(XC_BUILD_BENCH  "Build the benchmark harnesses"                 ON)
# option() coerces its value to a boolean, so the sanitizer selector has to be
# a cache STRING or "none" silently becomes OFF and reaches the compiler as
# -fsanitize=OFF.
option(XC_BUILD_FUZZERS "Build the libFuzzer targets (Clang only)" OFF)
option(XC_LTO "Link-time optimisation for release builds" OFF)

set(XC_SANITIZER "none" CACHE STRING "Sanitizer to enable: none|address|thread|undefined")
set_property(CACHE XC_SANITIZER PROPERTY STRINGS none address thread undefined)

# ---------------------------------------------------------------------------
# Warning and optimisation flags
# ---------------------------------------------------------------------------
add_library(xc_flags INTERFACE)

if(MSVC)
    target_compile_options(xc_flags INTERFACE /W4 /permissive-)
    if(XC_WERROR)
        target_compile_options(xc_flags INTERFACE /WX)
    endif()
else()
    target_compile_options(xc_flags INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion
        -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
        -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough
    )
    if(XC_WERROR)
        target_compile_options(xc_flags INTERFACE -Werror)
    endif()
endif()

# -march=native is deliberately opt-in. Baking it into the default build makes
# benchmark numbers non-reproducible across machines and can emit instructions
# the consumer's CPU does not have.
if(XC_NATIVE_ARCH AND NOT MSVC)
    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=native" XC_HAS_MARCH_NATIVE)
    if(XC_HAS_MARCH_NATIVE)
        target_compile_options(xc_flags INTERFACE -march=native)
    else()
        check_cxx_compiler_flag("-mcpu=native" XC_HAS_MCPU_NATIVE)
        if(XC_HAS_MCPU_NATIVE)
            target_compile_options(xc_flags INTERFACE -mcpu=native)
        endif()
    endif()
endif()

# Link-time optimisation.
#
# The matching path crosses translation units constantly -- order_book.cpp calls
# into price_level.cpp and order_pool.cpp on every command -- and without LTO
# none of that can inline. Off by default because it roughly triples link time
# and makes a profile harder to read; on in the preset used for benchmarks,
# where the question is how fast the code can go rather than how fast it builds.
if(XC_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT xc_ipo_ok OUTPUT xc_ipo_why)
    if(xc_ipo_ok)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        message(STATUS "Link-time optimisation: enabled")
    else()
        message(WARNING "XC_LTO requested but unsupported here: ${xc_ipo_why}")
    endif()
endif()

if(NOT XC_SANITIZER STREQUAL "none")
    if(MSVC)
        message(FATAL_ERROR "XC_SANITIZER is not supported with MSVC")
    endif()
    message(STATUS "Sanitizer enabled: ${XC_SANITIZER}")
    target_compile_options(xc_flags INTERFACE
        -fsanitize=${XC_SANITIZER} -fno-omit-frame-pointer -g)
    target_link_options(xc_flags INTERFACE -fsanitize=${XC_SANITIZER})
endif()

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(xc_core STATIC
    src/core/matching_engine.cpp
    src/core/order_book.cpp
    src/core/state_digest.cpp
    src/core/order_pool.cpp
    src/core/price_level.cpp
    src/journal/reader.cpp
    src/journal/record.cpp
    src/journal/replayer.cpp
    src/journal/writer.cpp
    src/net/event_loop.cpp
    src/net/tcp_gateway.cpp
    src/net/udp_feed.cpp
    src/protocol/messages.cpp
    src/risk/kill_switch.cpp
    src/risk/risk_engine.cpp
    src/venue.cpp
    src/util/build_info.cpp
    src/util/crc32c.cpp
    src/util/histogram.cpp
)
target_include_directories(xc_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(xc_core PUBLIC xc_flags)
add_library(xc::core ALIAS xc_core)

# Build metadata is compiled in rather than discovered at runtime, so any
# reported measurement can name the exact toolchain that produced the binary.
target_compile_definitions(xc_core PRIVATE
    XC_VERSION="${PROJECT_VERSION}"
    XC_COMPILER_ID="${CMAKE_CXX_COMPILER_ID}"
    XC_COMPILER_VERSION="${CMAKE_CXX_COMPILER_VERSION}"
    XC_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
    XC_SANITIZER_NAME="${XC_SANITIZER}"
)

# Allocation counting lives in its own library, linked only into the test
# binary.
#
# Replacing global operator new is a link-time decision that applies to the
# whole executable, so linking this in instruments every allocation the test
# makes -- including ones inside xc_core, which is the point. Keeping it out of
# xc_core means shipped binaries and the benchmarks carry no instrumentation at
# all, and cannot be accidentally measured with it enabled.
add_library(xc_alloc_counter STATIC src/util/allocation_counter.cpp)
target_include_directories(xc_alloc_counter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(xc_alloc_counter PUBLIC xc_flags)
# A sanitizer runtime replaces global operator new and delete for its own
# bookkeeping, and so does this counter. On Linux the two definitions collide
# at link time; on macOS the sanitizer interposes dynamically and one
# replacement silently wins, which is worse, because the counts are then wrong
# rather than absent.
#
# So counting is compiled out whenever a sanitizer is active, and that fact is
# propagated to dependents so the allocation tests can skip instead of
# observing zero allocations and declaring success. Tests passing because the
# thing they measure was switched off is the exact failure that
# allocation_counting_enabled() was added to catch.
if(XC_SANITIZER STREQUAL "none")
    target_compile_definitions(xc_alloc_counter PRIVATE XC_COUNT_ALLOCATIONS)
else()
    target_compile_definitions(xc_alloc_counter PUBLIC XC_SANITIZER_OWNS_ALLOCATOR)
endif()

add_subdirectory(apps)

if(XC_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(XC_BUILD_TESTS)
    enable_testing()
endif()

# fuzz/ holds the libFuzzer targets and the portable corpus runners that replay
# their oracles as ordinary tests, so it is needed for either -- and must come
# after enable_testing(), or its add_test() calls register nothing.
if(XC_BUILD_FUZZERS OR XC_BUILD_TESTS)
    add_subdirectory(fuzz)
endif()

if(XC_BUILD_TESTS)
    add_subdirectory(tests)
endif()

install(TARGETS xc_core ARCHIVE DESTINATION lib)
install(DIRECTORY include/xc DESTINATION include)
