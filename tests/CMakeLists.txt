include(FetchContent)

# Pinned to a release tag: a test suite that silently changes underneath the
# project would undermine the point of running it on every commit.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    GIT_SHALLOW TRUE
)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# The test targets compile under the project's full warning set with -Werror.
# GoogleTest's headers are not warning-clean against -Wconversion on every
# compiler we build with, and a red CI run caused by a dependency's headers
# teaches people to stop reading CI output. Marking the include directories as
# SYSTEM suppresses diagnostics from those headers without weakening the
# warnings applied to our own code.
foreach(gtest_target IN ITEMS gtest gtest_main)
    get_target_property(gtest_includes ${gtest_target} INTERFACE_INCLUDE_DIRECTORIES)
    set_target_properties(${gtest_target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${gtest_includes}")
endforeach()

add_executable(xc_tests
    model/reference_book.cpp
    test_build_info.cpp
    test_types.cpp
    test_order_storage.cpp
    test_order_book.cpp
    test_self_trade_and_post_only.cpp
    test_replace.cpp
    test_depth.cpp
    test_differential.cpp
    test_invariants.cpp
    test_matching_engine.cpp
    test_risk_limits.cpp
    test_risk_collar_and_rate.cpp
    test_kill_switch.cpp
    test_engine_risk.cpp
    test_byte_codec.cpp
    test_journal_record.cpp
    test_journal_io.cpp
    test_replay.cpp
    test_protocol.cpp
    test_event_loop.cpp
)
target_link_libraries(xc_tests PRIVATE xc::core GTest::gtest_main)
target_include_directories(xc_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

include(GoogleTest)
gtest_discover_tests(xc_tests)
