#include "xc/util/build_info.hpp"

#include <sstream>

namespace xc {
namespace {

std::string resolve_cxx_standard() {
    // Reported from the macro the compiler actually defines rather than from
    // the standard requested in CMake -- those can differ, and a benchmark
    // report that claims C++20 while the binary was built as C++17 is worse
    // than one that says nothing at all.
    switch (__cplusplus) {
        case 201103L:
            return "C++11";
        case 201402L:
            return "C++14";
        case 201703L:
            return "C++17";
        case 202002L:
            return "C++20";
        case 202302L:
            return "C++23";
        default:
            return "unknown (__cplusplus=" + std::to_string(__cplusplus) + ")";
    }
}

BuildInfo make_build_info() {
    BuildInfo info;
    info.version = XC_VERSION;
    info.compiler_id = XC_COMPILER_ID;
    info.compiler_version = XC_COMPILER_VERSION;
    info.build_type = XC_BUILD_TYPE;
    info.sanitizer = XC_SANITIZER_NAME;
    info.cxx_standard = resolve_cxx_standard();
#ifdef NDEBUG
    info.assertions_enabled = false;
#else
    info.assertions_enabled = true;
#endif
    return info;
}

}  // namespace

std::string BuildInfo::to_string() const {
    std::ostringstream out;
    out << "exchange-core " << version << '\n'
        << "  compiler     : " << compiler_id << ' ' << compiler_version << '\n'
        << "  standard     : " << cxx_standard << '\n'
        << "  build type   : " << build_type << '\n'
        << "  assertions   : " << (assertions_enabled ? "on" : "off") << '\n'
        << "  sanitizer    : " << sanitizer << '\n';
    return out.str();
}

const BuildInfo& build_info() {
    static const BuildInfo info = make_build_info();
    return info;
}

}  // namespace xc
