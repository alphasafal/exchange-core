# Two things are built from the same sources here.
#
# The corpus runners link each fuzz target's LLVMFuzzerTestOneInput against a
# plain main and replay the committed corpus through it. They need no special
# compiler, so they are registered as ordinary tests and run on every
# configuration in CI, including the sanitizer ones. That is what turns a crash
# found by fuzzing into a regression test: commit the input, and it fails
# everywhere.
#
# The fuzzers proper need libFuzzer, which is a Clang feature with no GCC
# equivalent and whose runtime is absent from the macOS Command Line Tools --
# it ships with full Xcode. They are built only when asked for, by the fuzz
# preset.

set(XC_FUZZ_ORDER_FLOW_SOURCES
    ${CMAKE_SOURCE_DIR}/tests/model/reference_book.cpp
    ${CMAKE_SOURCE_DIR}/tests/model/compare.cpp)

# --- corpus runners --------------------------------------------------------
if(XC_BUILD_TESTS)
    function(xc_add_corpus_runner target corpus)
        add_executable(corpus_${target} ${target}.cpp replay_corpus.cpp ${ARGN})
        target_link_libraries(corpus_${target} PRIVATE xc::core)
        add_test(NAME Corpus.${target}
                 COMMAND corpus_${target} ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${corpus})
    endfunction()

    xc_add_corpus_runner(fuzz_protocol protocol)
    xc_add_corpus_runner(fuzz_journal journal)
    xc_add_corpus_runner(fuzz_order_flow order_flow ${XC_FUZZ_ORDER_FLOW_SOURCES})
    target_include_directories(corpus_fuzz_order_flow PRIVATE ${CMAKE_SOURCE_DIR}/tests)
endif()

# --- libFuzzer targets -----------------------------------------------------
if(NOT XC_BUILD_FUZZERS)
    return()
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "XC_BUILD_FUZZERS requires Clang -- libFuzzer ships with it and has no GCC equivalent. "
        "Configure with the 'fuzz' preset and CC=clang CXX=clang++.")
endif()

# The library itself is instrumented through XC_SANITIZER, which the fuzz preset
# sets. Only the coverage instrumentation libFuzzer needs to steer itself is
# added here; putting the sanitizer here instead would leave the code under test
# uninstrumented, so the fuzzer would drive it blind and find only the crashes
# loud enough to fault on their own.
function(xc_add_fuzzer target)
    add_executable(${target} ${target}.cpp ${ARGN})
    target_link_libraries(${target} PRIVATE xc::core)
    target_compile_options(${target} PRIVATE -fsanitize=fuzzer -fno-omit-frame-pointer -g)
    target_link_options(${target} PRIVATE -fsanitize=fuzzer)
endfunction()

xc_add_fuzzer(fuzz_protocol)
xc_add_fuzzer(fuzz_journal)

# The order-flow fuzzer drives the same reference model and the same
# comparators the differential test uses, compiled from tests/ rather than
# copied, so the fuzzer cannot drift away from the oracle it extends.
xc_add_fuzzer(fuzz_order_flow ${XC_FUZZ_ORDER_FLOW_SOURCES})
target_include_directories(fuzz_order_flow PRIVATE ${CMAKE_SOURCE_DIR}/tests)
