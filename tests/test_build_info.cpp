#include <gtest/gtest.h>

#include "xc/util/build_info.hpp"

namespace {

TEST(BuildInfo, ReportsPopulatedToolchainFacts) {
    const auto& info = xc::build_info();
    EXPECT_FALSE(info.version.empty());
    EXPECT_FALSE(info.compiler_id.empty());
    EXPECT_FALSE(info.compiler_version.empty());
    EXPECT_FALSE(info.build_type.empty());
}

// The project targets C++20 specifically; several later components depend on
// features from it. Catching a misconfigured standard here is cheaper than
// debugging why a concept or designated initialiser failed to compile.
TEST(BuildInfo, BuildsAsCpp20OrLater) {
    EXPECT_GE(__cplusplus, 202002L);
    EXPECT_EQ(xc::build_info().cxx_standard, "C++20");
}

TEST(BuildInfo, RendersEveryFactIntoItsReport) {
    const std::string report = xc::build_info().to_string();
    EXPECT_NE(report.find("compiler"), std::string::npos);
    EXPECT_NE(report.find("standard"), std::string::npos);
    EXPECT_NE(report.find("build type"), std::string::npos);
    EXPECT_NE(report.find("sanitizer"), std::string::npos);
}

}  // namespace
