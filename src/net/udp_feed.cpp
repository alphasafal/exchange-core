#include "xc/net/udp_feed.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "xc/net/event_loop.hpp"

namespace xc::net {
namespace {

std::string errno_message(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

bool fill_address(const std::string& text, std::uint16_t port, sockaddr_in& out,
                  std::string& error) {
    out = sockaddr_in{};
    out.sin_family = AF_INET;
    out.sin_port = htons(port);
    if (::inet_pton(AF_INET, text.c_str(), &out.sin_addr) != 1) {
        error = "invalid address: " + text;
        return false;
    }
    return true;
}

}  // namespace

bool is_multicast_address(const std::string& address) {
    in_addr parsed{};
    if (::inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
        return false;
    }
    // 224.0.0.0/4 -- the top four bits are 1110.
    const std::uint32_t host_order = ntohl(parsed.s_addr);
    return (host_order & 0xF0000000U) == 0xE0000000U;
}

// --- Publisher -------------------------------------------------------------

UdpPublisher::UdpPublisher(FeedConfig config) : config_(std::move(config)) {
    destination_.resize(sizeof(sockaddr_in));
}

UdpPublisher::~UdpPublisher() {
    close();
}

bool UdpPublisher::open() {
    multicast_ = is_multicast_address(config_.address);

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        last_error_ = errno_message("socket");
        return false;
    }

    auto* destination = reinterpret_cast<sockaddr_in*>(destination_.data());
    if (!fill_address(config_.address, config_.port, *destination, last_error_)) {
        close();
        return false;
    }

    if (multicast_) {
        in_addr interface{};
        if (::inet_pton(AF_INET, config_.interface_address.c_str(), &interface) != 1) {
            last_error_ = "invalid interface address: " + config_.interface_address;
            close();
            return false;
        }
        // Pinned explicitly rather than left to the routing table. A venue that
        // publishes out of whichever interface happens to win a route lookup is
        // one network change away from broadcasting its feed somewhere it
        // should not.
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &interface, sizeof(interface)) != 0) {
            last_error_ = errno_message("set IP_MULTICAST_IF");
            close();
            return false;
        }
        const auto ttl = config_.ttl;
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) != 0) {
            last_error_ = errno_message("set IP_MULTICAST_TTL");
            close();
            return false;
        }
        const std::uint8_t loopback = config_.loopback ? 1 : 0;
        if (::setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loopback, sizeof(loopback)) != 0) {
            last_error_ = errno_message("set IP_MULTICAST_LOOP");
            close();
            return false;
        }
    }

    // Non-blocking so that a full socket buffer is reported rather than
    // stalling the caller. The matching thread publishes from here, and it must
    // never wait on the network.
    if (!set_non_blocking(fd_, last_error_)) {
        close();
        return false;
    }

    bound_port_ = config_.port;
    return true;
}

void UdpPublisher::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UdpPublisher::publish(std::span<const std::uint8_t> payload) {
    if (fd_ < 0) {
        return false;
    }
    if (payload.size() > config_.max_datagram) {
        // Refused rather than fragmented. A fragmented datagram is lost in its
        // entirety when any one fragment is, so oversized market data does not
        // degrade under loss -- it disappears.
        ++send_failures_;
        last_error_ = "payload exceeds the maximum datagram size";
        return false;
    }

    const auto* destination = reinterpret_cast<const sockaddr_in*>(destination_.data());
    const ssize_t sent =
        ::sendto(fd_, payload.data(), payload.size(), 0,
                 reinterpret_cast<const sockaddr*>(destination), sizeof(sockaddr_in));
    if (sent < 0) {
        // Counted, never retried and never blocked on: letting the network
        // stall the matching thread is the one thing a feed must not do to the
        // venue it reports on. A subscriber detects the loss by sequence number.
        ++send_failures_;
        last_error_ = errno_message("sendto");
        return false;
    }

    ++datagrams_sent_;
    bytes_sent_ += static_cast<std::uint64_t>(sent);
    return true;
}

// --- Subscriber ------------------------------------------------------------

UdpSubscriber::UdpSubscriber(FeedConfig config) : config_(std::move(config)) {}

UdpSubscriber::~UdpSubscriber() {
    close();
}

bool UdpSubscriber::open() {
    multicast_ = is_multicast_address(config_.address);

    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        last_error_ = errno_message("socket");
        return false;
    }
    if (!set_reuse_address(fd_, last_error_)) {
        close();
        return false;
    }

    sockaddr_in bind_address{};
    bind_address.sin_family = AF_INET;
    bind_address.sin_port = htons(config_.port);
    // A multicast subscriber binds the wildcard address: binding the group
    // itself is not portable, and binding the interface address would reject
    // datagrams addressed to the group.
    bind_address.sin_addr.s_addr =
        multicast_ ? htonl(INADDR_ANY) : inet_addr(config_.interface_address.c_str());

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&bind_address), sizeof(bind_address)) != 0) {
        last_error_ = errno_message("bind");
        close();
        return false;
    }

    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    }

    if (multicast_) {
        ip_mreq request{};
        if (::inet_pton(AF_INET, config_.address.c_str(), &request.imr_multiaddr) != 1 ||
            ::inet_pton(AF_INET, config_.interface_address.c_str(), &request.imr_interface) != 1) {
            last_error_ = "invalid multicast group or interface";
            close();
            return false;
        }
        if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &request, sizeof(request)) != 0) {
            last_error_ = errno_message("join multicast group");
            close();
            return false;
        }
    }

    if (!set_non_blocking(fd_, last_error_)) {
        close();
        return false;
    }
    return true;
}

void UdpSubscriber::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool UdpSubscriber::receive(std::vector<std::uint8_t>& out) {
    if (fd_ < 0) {
        return false;
    }

    // Sized for the largest datagram the publisher will send. A short buffer
    // would silently truncate: recvfrom discards the remainder of a datagram
    // that does not fit, and the caller would decode a message that looks
    // complete and is not.
    out.resize(config_.max_datagram);
    while (true) {
        const ssize_t received = ::recv(fd_, out.data(), out.size(), 0);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            out.clear();
            return false;  // EAGAIN: nothing waiting.
        }
        out.resize(static_cast<std::size_t>(received));
        ++datagrams_received_;
        return true;
    }
}

}  // namespace xc::net
