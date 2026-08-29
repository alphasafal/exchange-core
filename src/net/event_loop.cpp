#include "xc/net/event_loop.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#if defined(__linux__)
#include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#include <sys/time.h>
#else
#error "no supported readiness mechanism on this platform"
#endif

namespace xc::net {
namespace {

std::string errno_message(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

constexpr int kMaxEventsPerWait = 256;

}  // namespace

bool set_non_blocking(int fd, std::string& error) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags == -1 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        error = errno_message("set non-blocking");
        return false;
    }
    return true;
}

bool set_no_delay(int fd, std::string& error) {
    const int on = 1;
    if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == -1) {
        error = errno_message("set TCP_NODELAY");
        return false;
    }
    return true;
}

bool set_reuse_address(int fd, std::string& error) {
    const int on = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        error = errno_message("set SO_REUSEADDR");
        return false;
    }
    return true;
}

#if defined(__linux__)

EventLoop::EventLoop() {
    handle_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (handle_ < 0) {
        last_error_ = errno_message("epoll_create1");
    }
    scratch_.resize(sizeof(epoll_event) * kMaxEventsPerWait);
}

EventLoop::~EventLoop() {
    if (handle_ >= 0) {
        ::close(handle_);
    }
}

namespace {
std::uint32_t to_epoll(Interest interest) {
    std::uint32_t events = 0;
    if (static_cast<std::uint8_t>(interest) & static_cast<std::uint8_t>(Interest::Read)) {
        events |= EPOLLIN;
    }
    if (static_cast<std::uint8_t>(interest) & static_cast<std::uint8_t>(Interest::Write)) {
        events |= EPOLLOUT;
    }
    return events;
}

bool control(int handle, int op, int fd, Interest interest, std::string& error) {
    epoll_event event{};
    event.events = to_epoll(interest);
    event.data.fd = fd;
    if (::epoll_ctl(handle, op, fd, &event) == -1) {
        error = errno_message("epoll_ctl");
        return false;
    }
    return true;
}
}  // namespace

bool EventLoop::add(int fd, Interest interest) {
    return control(handle_, EPOLL_CTL_ADD, fd, interest, last_error_);
}

bool EventLoop::modify(int fd, Interest interest) {
    return control(handle_, EPOLL_CTL_MOD, fd, interest, last_error_);
}

bool EventLoop::remove(int fd) {
    if (::epoll_ctl(handle_, EPOLL_CTL_DEL, fd, nullptr) == -1 && errno != ENOENT) {
        last_error_ = errno_message("epoll_ctl del");
        return false;
    }
    return true;
}

bool EventLoop::wait(int timeout_ms, std::vector<Event>& out) {
    auto* events = reinterpret_cast<epoll_event*>(scratch_.data());
    const int count = ::epoll_wait(handle_, events, kMaxEventsPerWait, timeout_ms);
    if (count < 0) {
        if (errno == EINTR) {
            // A signal interrupting a blocking wait is normal and not something
            // the caller should have to tell apart from a real failure.
            return true;
        }
        last_error_ = errno_message("epoll_wait");
        return false;
    }

    for (int i = 0; i < count; ++i) {
        Event event;
        event.fd = events[i].data.fd;
        event.readable = (events[i].events & EPOLLIN) != 0;
        event.writable = (events[i].events & EPOLLOUT) != 0;
        event.closed = (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) != 0;
        out.push_back(event);
    }
    return true;
}

#else  // kqueue

EventLoop::EventLoop() {
    handle_ = ::kqueue();
    if (handle_ < 0) {
        last_error_ = errno_message("kqueue");
    }
    scratch_.resize(sizeof(struct kevent) * kMaxEventsPerWait);
}

EventLoop::~EventLoop() {
    if (handle_ >= 0) {
        ::close(handle_);
    }
}

namespace {
bool apply(int handle, int fd, Interest interest, bool enable, std::string& error) {
    // kqueue tracks read and write filters separately, so one Interest becomes
    // up to two changes -- each filter enabled or disabled to match. epoll
    // instead replaces a single mask, which is why the two need different code
    // rather than a shared wrapper pretending they are the same mechanism.
    const bool want_read =
        enable && (static_cast<std::uint8_t>(interest) & static_cast<std::uint8_t>(Interest::Read));
    const bool want_write = enable && (static_cast<std::uint8_t>(interest) &
                                       static_cast<std::uint8_t>(Interest::Write));

    struct kevent changes[2];
    EV_SET(&changes[0], static_cast<std::uintptr_t>(fd), EVFILT_READ,
           want_read ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, nullptr);
    EV_SET(&changes[1], static_cast<std::uintptr_t>(fd), EVFILT_WRITE,
           want_write ? (EV_ADD | EV_ENABLE) : EV_DELETE, 0, 0, nullptr);

    for (auto& change : changes) {
        if (::kevent(handle, &change, 1, nullptr, 0, nullptr) == -1) {
            // Deleting a filter that was never registered is expected whenever
            // interest narrows, and is not a failure.
            if (errno == ENOENT && (change.flags & EV_DELETE) != 0) {
                continue;
            }
            error = errno_message("kevent");
            return false;
        }
    }
    return true;
}
}  // namespace

bool EventLoop::add(int fd, Interest interest) {
    return apply(handle_, fd, interest, true, last_error_);
}

bool EventLoop::modify(int fd, Interest interest) {
    return apply(handle_, fd, interest, true, last_error_);
}

bool EventLoop::remove(int fd) {
    return apply(handle_, fd, Interest::ReadWrite, false, last_error_);
}

bool EventLoop::wait(int timeout_ms, std::vector<Event>& out) {
    auto* events = reinterpret_cast<struct kevent*>(scratch_.data());

    struct timespec timeout {};
    struct timespec* timeout_ptr = nullptr;
    if (timeout_ms >= 0) {
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_nsec = static_cast<long>(timeout_ms % 1000) * 1'000'000;
        timeout_ptr = &timeout;
    }

    const int count = ::kevent(handle_, nullptr, 0, events, kMaxEventsPerWait, timeout_ptr);
    if (count < 0) {
        if (errno == EINTR) {
            return true;
        }
        last_error_ = errno_message("kevent wait");
        return false;
    }

    for (int i = 0; i < count; ++i) {
        const int fd = static_cast<int>(events[i].ident);

        // kqueue reports one filter per event, so a descriptor that is both
        // readable and writable arrives as two. They are merged here, because a
        // caller handling the same connection twice in one pass is a subtle
        // source of double-processing bugs.
        Event* existing = nullptr;
        for (Event& candidate : out) {
            if (candidate.fd == fd) {
                existing = &candidate;
                break;
            }
        }
        if (existing == nullptr) {
            out.push_back(Event{fd, false, false, false});
            existing = &out.back();
        }

        if (events[i].filter == EVFILT_READ) {
            existing->readable = true;
        } else if (events[i].filter == EVFILT_WRITE) {
            existing->writable = true;
        }
        if ((events[i].flags & EV_EOF) != 0 || (events[i].flags & EV_ERROR) != 0) {
            existing->closed = true;
        }
    }
    return true;
}

#endif

}  // namespace xc::net
