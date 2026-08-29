#pragma once

#include <string>

namespace xc {

/// Toolchain and configuration facts baked into the binary at compile time.
///
/// Every performance number this project publishes has to be attributable to a
/// specific build. Rather than asking a reader to trust that a documented flag
/// set matches the binary that produced a measurement, each executable can
/// print exactly what it was compiled as, and the benchmark harness embeds that
/// output directly in its report.
struct BuildInfo {
    std::string version;      ///< Project version, e.g. "0.1.0".
    std::string compiler_id;  ///< "Clang", "AppleClang", "GNU", "MSVC".
    std::string compiler_version;
    std::string build_type;    ///< "Release", "Debug", "RelWithDebInfo".
    std::string cxx_standard;  ///< Resolved from __cplusplus, not assumed.
    std::string sanitizer;     ///< "none", "address", "thread", "undefined".
    bool assertions_enabled;   ///< True when NDEBUG is not defined.

    /// One line per fact, suitable for pasting into a benchmark report.
    std::string to_string() const;
};

/// Build facts for the translation unit this library was compiled from.
const BuildInfo& build_info();

}  // namespace xc
