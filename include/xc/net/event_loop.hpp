#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xc::net {

/// What a descriptor is being watched for.
enum class Interest : std::uint8_t {
    Read = 1,
    Write = 2,
    ReadWrite = 3,
};

/// What happened to a descriptor.
struct Event {
    int fd = -1;
    bool readable = false;
    bool writable = false;

    /// The peer closed, or the connection failed.
    ///
    /// Reported separately rather than folded into `readable` because the
    /// difference matters: a socket that is readable has data, while one that
    /// has hung up will report readable forever and hand back zero bytes each
    /// time. Conflating them is how a server ends up spinning at full CPU on a
    /// disconnected client.
    bool closed = false;
};

/// A readiness loop over kqueue on macOS and epoll on Linux.
///
/// Both are edge-capable, both scale with the number of *ready* descriptors
/// rather than the number watched, and both are the mechanism their platform is
/// tuned for. select() and poll() would be one portable implementation instead
/// of two, and both rescan every watched descriptor on every call, so their
/// cost grows with idle connections -- exactly the connections a venue has most
/// of.
///
/// The interface is deliberately small: add, modify, remove, and wait. Anything
/// richer would have to be emulated on one platform or the other, and an
/// abstraction that lies about what the kernel does is worse than none.
///
/// Level-triggered, not edge-triggered. Edge triggering is faster in principle
/// and unforgiving in practice -- a single partial read that forgets to drain
/// the socket loses the notification and stalls that connection until it
/// happens to receive more. This is not the place to trade a class of silent
/// stalls for a system call.
class EventLoop {
  public:
    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /// True when the loop was created successfully.
    bool valid() const noexcept { return handle_ >= 0; }

    bool add(int fd, Interest interest);
    bool modify(int fd, Interest interest);
    bool remove(int fd);

    /// Waits up to `timeout_ms` for readiness and appends what it finds to
    /// `out`. A negative timeout waits indefinitely. Returns false on a real
    /// error; an interruption by a signal is reported as success with no
    /// events, since that is a normal thing to happen to a blocking call and
    /// not something a caller should have to distinguish.
    bool wait(int timeout_ms, std::vector<Event>& out);

    const std::string& last_error() const noexcept { return last_error_; }

  private:
    int handle_ = -1;
    std::string last_error_;

    /// Scratch space for the platform's event structures, reused across calls
    /// so that polling never allocates.
    std::vector<std::uint8_t> scratch_;
};

/// Puts a descriptor into non-blocking mode.
bool set_non_blocking(int fd, std::string& error);

/// Disables Nagle's algorithm on a TCP socket.
///
/// Nagle holds a small write back waiting for more data to coalesce with, which
/// is exactly wrong for order entry: the messages here are small and each one
/// is latency-critical, so waiting to make a larger packet trades the only
/// thing that matters for bandwidth nobody is short of.
bool set_no_delay(int fd, std::string& error);

/// Allows a listening socket to rebind while an old one lingers in TIME_WAIT,
/// so a restarted venue does not have to wait out the kernel's timer.
bool set_reuse_address(int fd, std::string& error);

}  // namespace xc::net
