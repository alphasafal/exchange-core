#include "xc/journal/writer.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace xc::journal {
namespace {

constexpr std::string_view kSegmentPrefix = "segment-";
constexpr std::string_view kSegmentSuffix = ".xcj";
constexpr std::size_t kSegmentDigits = 8;
constexpr std::size_t kSegmentNameLength =
    kSegmentPrefix.size() + kSegmentDigits + kSegmentSuffix.size();

/// Persists a file descriptor's contents to durable storage.
///
/// On macOS, fsync() is documented to push data to the drive but *not* to make
/// the drive flush its own write cache -- so a power loss can still lose data
/// that fsync() reported as safe. F_FULLFSYNC is the call that actually
/// guarantees it. Using plain fsync() on Apple platforms would mean this
/// project's durability claims were false on the machine it was developed on,
/// which is exactly the kind of quiet inaccuracy the whole repository is
/// written to avoid.
bool persist(int fd) {
#if defined(__APPLE__)
    return ::fcntl(fd, F_FULLFSYNC) != -1;
#else
    return ::fsync(fd) == 0;
#endif
}

std::string errno_message(std::string_view what) {
    return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

JournalWriter::JournalWriter(WriterConfig config) : config_(std::move(config)) {
    buffer_.reserve(config_.buffer_bytes);
}

JournalWriter::~JournalWriter() {
    close();
}

std::string JournalWriter::segment_name(std::uint32_t index) {
    // Zero-padded so that lexicographic order matches chronological order, in
    // a directory listing as well as in the reader.
    //
    // The buffer is sized for the widest output the format can produce, so the
    // return value cannot indicate truncation; it is checked anyway, because a
    // silently truncated name would produce a segment the reader cannot find.
    char name[kSegmentNameLength + 1];
    const int written = std::snprintf(name, sizeof(name), "segment-%08u.xcj", index);
    if (written != static_cast<int>(kSegmentNameLength)) {
        return {};
    }
    return name;
}

std::optional<std::uint32_t> JournalWriter::parse_segment_name(std::string_view name) {
    if (name.size() != kSegmentNameLength) {
        return std::nullopt;
    }
    if (!name.starts_with(kSegmentPrefix) || !name.ends_with(kSegmentSuffix)) {
        return std::nullopt;
    }

    const std::string_view digits = name.substr(kSegmentPrefix.size(), kSegmentDigits);

    // from_chars accepts no sign and no leading whitespace, and the length
    // check above has already fixed the field width -- so requiring it to
    // consume every character leaves exactly eight decimal digits as the only
    // thing this accepts.
    std::uint32_t index = 0;
    const char* const end = digits.data() + digits.size();
    const std::from_chars_result result = std::from_chars(digits.data(), end, index);
    if (result.ec != std::errc{} || result.ptr != end) {
        return std::nullopt;
    }
    return index;
}

bool JournalWriter::open() {
    std::error_code ec;
    std::filesystem::create_directories(config_.directory, ec);
    if (ec && !std::filesystem::exists(config_.directory)) {
        fail("create journal directory: " + ec.message());
        return false;
    }

    // Continue after the highest existing segment rather than reusing or
    // appending to it. A segment left half-written by a crash must stay exactly
    // as it is, so recovery can see where the previous run stopped.
    std::uint32_t highest = 0;
    for (const auto& entry : std::filesystem::directory_iterator(config_.directory, ec)) {
        if (const std::optional<std::uint32_t> index =
                parse_segment_name(entry.path().filename().string())) {
            highest = std::max(highest, *index);
        }
    }
    segment_index_ = highest;

    return roll_segment();
}

bool JournalWriter::roll_segment() {
    if (fd_ != -1) {
        if (!flush() || !persist(fd_)) {
            fail(errno_message("persist segment before rolling"));
            return false;
        }
        ++syncs_;
        ::close(fd_);
        fd_ = -1;
    }

    if (segment_index_ >= kMaxSegmentIndex) {
        fail("segment index would exceed the name format's eight digits");
        return false;
    }

    ++segment_index_;
    current_path_ = config_.directory / segment_name(segment_index_);

    // O_EXCL so that an existing segment is never silently overwritten. If the
    // name is taken, something is wrong with the caller's assumptions about the
    // directory, and destroying a previous run's journal is the worst possible
    // way to find that out.
    fd_ = ::open(current_path_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0644);
    if (fd_ == -1) {
        fail(errno_message("open segment " + current_path_.string()));
        return false;
    }

    segment_size_ = 0;
    ++segments_opened_;
    return true;
}

bool JournalWriter::write_all(const std::uint8_t* data, std::size_t size) {
    std::size_t written = 0;
    while (written < size) {
        // write() is permitted to transfer fewer bytes than requested, and a
        // short write that went unnoticed would leave a truncated record in the
        // middle of the journal rather than at its end -- where recovery
        // expects damage and knows how to handle it.
        const ssize_t result = ::write(fd_, data + written, size - written);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail(errno_message("write journal segment"));
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

bool JournalWriter::append(const Record& record, Nanos now) {
    if (!healthy_ || fd_ == -1) {
        return false;
    }

    const std::size_t before = buffer_.size();
    encode(record, buffer_);
    const std::size_t encoded_size = buffer_.size() - before;

    ++records_written_;
    bytes_written_ += encoded_size;
    segment_size_ += encoded_size;

    const bool sync_now =
        config_.durability == Durability::Always ||
        (config_.durability == Durability::Interval && now - last_sync_ >= config_.sync_interval);

    if (buffer_.size() >= config_.buffer_bytes || sync_now) {
        if (!flush()) {
            return false;
        }
    }
    if (sync_now && !sync()) {
        return false;
    }
    if (sync_now) {
        last_sync_ = now;
    }

    if (segment_size_ >= config_.segment_bytes) {
        return roll_segment();
    }
    return true;
}

bool JournalWriter::flush() {
    if (buffer_.empty()) {
        return healthy_;
    }
    if (fd_ == -1) {
        return false;
    }
    if (!write_all(buffer_.data(), buffer_.size())) {
        return false;
    }
    // Cleared rather than freed, so a steady-state venue reuses the same
    // allocation for the life of the process.
    buffer_.clear();
    return true;
}

bool JournalWriter::sync() {
    if (fd_ == -1) {
        return false;
    }
    if (!flush()) {
        return false;
    }
    if (!persist(fd_)) {
        fail(errno_message("persist journal"));
        return false;
    }
    ++syncs_;
    return true;
}

void JournalWriter::close() {
    if (fd_ == -1) {
        return;
    }
    flush();
    if (persist(fd_)) {
        ++syncs_;
    }
    ::close(fd_);
    fd_ = -1;
}

void JournalWriter::fail(const std::string& message) {
    healthy_ = false;
    if (last_error_.empty()) {
        // The first failure is the informative one; later ones are usually its
        // consequences.
        last_error_ = message;
    }
}

}  // namespace xc::journal
