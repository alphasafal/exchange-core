#include "xc/journal/reader.hpp"

#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

namespace xc::journal {

JournalReader::JournalReader(std::filesystem::path directory) : directory_(std::move(directory)) {}

std::vector<std::filesystem::path> JournalReader::segments() const {
    std::vector<std::pair<std::uint32_t, std::filesystem::path>> found;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
        const std::string name = entry.path().filename().string();
        std::uint32_t index = 0;
        if (std::sscanf(name.c_str(), "segment-%08u.xcj", &index) == 1) {
            found.emplace_back(index, entry.path());
        }
    }

    // Sorted by the index parsed out of the name rather than by directory
    // order, which no filesystem promises, or by modification time, which a
    // backup or a copy would rewrite.
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<std::filesystem::path> paths;
    paths.reserve(found.size());
    for (auto& [index, path] : found) {
        paths.push_back(std::move(path));
    }
    return paths;
}

RecoveryReport JournalReader::read(const std::function<void(const Record&)>& visit) {
    RecoveryReport report;

    for (const std::filesystem::path& path : segments()) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            report.outcome = RecoveryOutcome::Unreadable;
            report.message = "cannot open segment " + path.string();
            return report;
        }

        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                        std::istreambuf_iterator<char>());
        ++report.segments_read;

        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const DecodeResult decoded =
                decode(std::span<const std::uint8_t>(bytes.data() + offset, bytes.size() - offset));
            if (decoded.status == DecodeStatus::Ok) {
                visit(decoded.record);
                ++report.records_recovered;
                report.last_sequence = decoded.record.sequence;
                report.bytes_recovered += decoded.consumed;
                offset += decoded.consumed;
                continue;
            }

            report.damaged_segment = path;
            report.good_bytes_in_damaged_segment = offset;
            if (decoded.status == DecodeStatus::Incomplete) {
                report.outcome = RecoveryOutcome::TornTail;
                report.message = "journal ends part-way through a record at offset " +
                                 std::to_string(offset) + " of " + path.string();
            } else {
                report.outcome = RecoveryOutcome::Damaged;
                report.message =
                    std::string(decoded.status == DecodeStatus::Corrupt ? "checksum failure"
                                                                        : "unreadable record") +
                    " at offset " + std::to_string(offset) + " of " + path.string();
            }
            // Stops at the first bad record rather than scanning past it. There
            // is no framing marker to resynchronise on, so anything found after
            // damage would be bytes that merely happen to parse -- and a
            // plausible reconstruction built from those is worse than an honest
            // short one.
            return report;
        }
    }

    return report;
}

bool JournalReader::truncate_to(const std::filesystem::path& segment, std::uint64_t bytes,
                                std::string& error) {
    if (::truncate(segment.c_str(), static_cast<off_t>(bytes)) != 0) {
        error = std::string("truncate ") + segment.string() + ": " + std::strerror(errno);
        return false;
    }
    return true;
}

}  // namespace xc::journal
