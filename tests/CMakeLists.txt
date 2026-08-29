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

add_executable(xc_tests
    test_build_info.cpp
)
target_link_libraries(xc_tests PRIVATE xc::core GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(xc_tests)
