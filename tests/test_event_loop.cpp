#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <vector>

#include "xc/net/event_loop.hpp"

namespace xc::net {
namespace {

/// A connected socket pair that closes itself, so a failing assertion cannot
/// leak descriptors into the rest of the suite.
class SocketPair {
  public:
    SocketPair() {
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds_) != 0) {
            fds_[0] = fds_[1] = -1;
        }
    }
    ~SocketPair() { close_both(); }

    int a() const { return fds_[0]; }
    int b() const { return fds_[1]; }
    bool valid() const { return fds_[0] >= 0 && fds_[1] >= 0; }

    void close_b() {
        if (fds_[1] >= 0) {
            ::close(fds_[1]);
            fds_[1] = -1;
        }
    }

  private:
    void close_both() {
        for (int& fd : fds_) {
            if (fd >= 0) {
                ::close(fd);
                fd = -1;
            }
        }
    }
    int fds_[2]{-1, -1};
};

std::vector<Event> poll_once(EventLoop& loop, int timeout_ms = 50) {
    std::vector<Event> events;
    EXPECT_TRUE(loop.wait(timeout_ms, events)) << loop.last_error();
    return events;
}

TEST(EventLoop, ConstructsSuccessfully) {
    EventLoop loop;
    EXPECT_TRUE(loop.valid()) << loop.last_error();
}

TEST(EventLoop, ReportsNothingWhenNothingIsReady) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    EventLoop loop;
    ASSERT_TRUE(loop.add(pair.a(), Interest::Read));

    EXPECT_TRUE(poll_once(loop, 10).empty());
}

TEST(EventLoop, ReportsReadinessWhenDataArrives) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    EventLoop loop;
    ASSERT_TRUE(loop.add(pair.a(), Interest::Read)) << loop.last_error();

    const char payload = 'x';
    ASSERT_EQ(::write(pair.b(), &payload, 1), 1);

    const std::vector<Event> events = poll_once(loop);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].fd, pair.a());
    EXPECT_TRUE(events[0].readable);
}

TEST(EventLoop, ReportsAClosedPeerDistinctlyFromData) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    EventLoop loop;
    ASSERT_TRUE(loop.add(pair.a(), Interest::Read));

    pair.close_b();

    // A hung-up socket reports readable forever and returns zero bytes each
    // time. A server that cannot tell the two apart spins at full CPU on a
    // disconnected client.
    const std::vector<Event> events = poll_once(loop);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].closed);
}

TEST(EventLoop, ReportsWritabilityOnAnIdleSocket) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    EventLoop loop;
    ASSERT_TRUE(loop.add(pair.a(), Interest::Write));

    const std::vector<Event> events = poll_once(loop);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].writable);
}

TEST(EventLoop, MergesReadAndWriteIntoOneEventPerDescriptor) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    EventLoop loop;
    ASSERT_TRUE(loop.add(pair.a(), Interest::ReadWrite));

    const char payload = 'x';
    ASSERT_EQ(::write(pair.b(), &payload, 1), 1);

    // kqueue reports one event per filter, so a socket both readable and
    // writable arrives twice. Handling the same connection twice in one pass is
    // a subtle source of double-processing, so the loop merges them.
    const std::vector<Event> events = poll_once(loop);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].readable);
    EXPECT_TRUE(events[0].writable);
}

TEST(EventLoop, NarrowingInterestStopsTheUnwantedNotification) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    EventLoop loop;
    ASSERT_TRUE(loop.add(pair.a(), Interest::ReadWrite));
    ASSERT_FALSE(poll_once(loop).empty()) << "writable while idle";

    // This is the operation a gateway performs constantly: it only wants write
    // readiness while it has something buffered to send, and must be able to
    // switch it off again or every idle connection wakes the loop forever.
    ASSERT_TRUE(loop.modify(pair.a(), Interest::Read)) << loop.last_error();
    EXPECT_TRUE(poll_once(loop, 10).empty());

    const char payload = 'x';
    ASSERT_EQ(::write(pair.b(), &payload, 1), 1);
    const std::vector<Event> events = poll_once(loop);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events[0].readable);
    EXPECT_FALSE(events[0].writable);
}

TEST(EventLoop, RemovedDescriptorsStopReporting) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    EventLoop loop;
    ASSERT_TRUE(loop.add(pair.a(), Interest::Read));
    ASSERT_TRUE(loop.remove(pair.a())) << loop.last_error();

    const char payload = 'x';
    ASSERT_EQ(::write(pair.b(), &payload, 1), 1);
    EXPECT_TRUE(poll_once(loop, 10).empty());
}

TEST(EventLoop, RemovingAnUnwatchedDescriptorIsNotAnError) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    EventLoop loop;
    // A connection can be torn down along more than one path, and requiring the
    // caller to track whether it is still registered invites double-remove bugs
    // that are worse than the redundant call.
    EXPECT_TRUE(loop.remove(pair.a())) << loop.last_error();
}

TEST(EventLoop, WatchesManyDescriptorsAtOnce) {
    constexpr int kCount = 32;
    std::vector<SocketPair> pairs(kCount);
    EventLoop loop;
    for (const SocketPair& pair : pairs) {
        ASSERT_TRUE(pair.valid());
        ASSERT_TRUE(loop.add(pair.a(), Interest::Read)) << loop.last_error();
    }

    // Write to every other one: the loop must report exactly those, not all of
    // them and not the first few.
    const char payload = 'x';
    for (int i = 0; i < kCount; i += 2) {
        ASSERT_EQ(::write(pairs[static_cast<std::size_t>(i)].b(), &payload, 1), 1);
    }

    const std::vector<Event> events = poll_once(loop);
    EXPECT_EQ(events.size(), static_cast<std::size_t>(kCount / 2));
    for (const Event& event : events) {
        EXPECT_TRUE(event.readable);
    }
}

TEST(SocketOptions, ApplyToARealSocket) {
    SocketPair pair;
    ASSERT_TRUE(pair.valid());
    std::string error;
    EXPECT_TRUE(set_non_blocking(pair.a(), error)) << error;
    EXPECT_TRUE(set_reuse_address(pair.a(), error)) << error;
}

}  // namespace
}  // namespace xc::net
