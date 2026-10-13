/// Runs a fuzz target's entry point over files named on the command line.
///
/// libFuzzer is Clang-only and its runtime does not ship with the macOS
/// Command Line Tools, so the fuzzers themselves build on some of the
/// configurations this project supports and not others. The oracles inside
/// them are the valuable part, and there is no reason those should only run
/// where libFuzzer happens to exist.
///
/// This driver links the same LLVMFuzzerTestOneInput against a plain main, so
/// every committed corpus entry is replayed through its oracle in the ordinary
/// test suite -- on both compilers, both platforms, and under AddressSanitizer
/// and UndefinedBehaviorSanitizer. The corpus becomes a regression corpus, and
/// a crash found by a long fuzzing run turns into a file that fails CI
/// everywhere once it is committed.
///
/// What this does not do is explore. It replays exactly what it is given.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

namespace {

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <corpus-dir-or-file>...\n", argv[0]);
        return 2;
    }

    int replayed = 0;
    for (int i = 1; i < argc; ++i) {
        const std::filesystem::path path(argv[i]);
        std::error_code ec;

        if (std::filesystem::is_directory(path, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
                if (!entry.is_regular_file()) {
                    continue;
                }
                const std::vector<std::uint8_t> bytes = read_file(entry.path());
                LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
                ++replayed;
            }
            continue;
        }

        if (!std::filesystem::is_regular_file(path, ec)) {
            std::fprintf(stderr, "not a file or directory: %s\n", argv[i]);
            return 2;
        }
        const std::vector<std::uint8_t> bytes = read_file(path);
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
        ++replayed;
    }

    // An empty corpus directory would otherwise make this exit 0 having checked
    // nothing, which is the same vacuous pass the allocation tests once had.
    if (replayed == 0) {
        std::fprintf(stderr, "no corpus entries were replayed\n");
        return 1;
    }

    std::printf("replayed %d corpus entries\n", replayed);
    return 0;
}
