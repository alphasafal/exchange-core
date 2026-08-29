#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace xc::net {

struct FeedConfig {
    /// Where datagrams are sent. A multicast group address turns on multicast
    /// socket options automatically; a unicast address is sent to directly.
    std::string address = "127.0.0.1";
    std::uint16_t port = 0;

    /// Interface to send from and receive on.
    std::string interface_address = "127.0.0.1";

    /// Multicast hop limit. One by default, so a development venue cannot leak
    /// its feed onto the wider network by accident.
    std::uint8_t ttl = 1;

    /// Whether a multicast sender also receives its own traffic. On for tests
    /// and local runs, where publisher and subscriber share a host.
    bool loopback = true;

    /// Largest datagram published.
    ///
    /// Held below a typical 1500-byte Ethernet MTU on purpose. A larger
    /// datagram is fragmented by IP, and a single lost fragment discards the
    /// whole datagram -- so oversized market data messages do not degrade under
    /// loss, they disappear, and they take the rest of the message with them.
    std::size_t max_datagram = 1400;
};

/// Sends market data datagrams.
///
/// UDP, not TCP, and the reason is a design choice rather than a shortcut. A
/// market data feed has many subscribers and no useful notion of
/// retransmission: by the time a lost quote could be resent it describes a
/// market that no longer exists. TCP would also make one slow subscriber's
/// congestion window the whole feed's problem. Loss is instead made detectable
/// by sequence numbers, and repaired by resynchronising from a snapshot rather
/// than by replaying stale increments.
class UdpPublisher {
  public:
    explicit UdpPublisher(FeedConfig config);
    ~UdpPublisher();

    UdpPublisher(const UdpPublisher&) = delete;
    UdpPublisher& operator=(const UdpPublisher&) = delete;

    bool open();
    void close();

    /// Sends one datagram. Refuses anything above the configured size rather
    /// than letting IP fragment it.
    bool publish(std::span<const std::uint8_t> payload);

    /// Port actually in use, which matters when the configured port was zero.
    std::uint16_t port() const noexcept { return bound_port_; }

    std::uint64_t datagrams_sent() const noexcept { return datagrams_sent_; }
    std::uint64_t bytes_sent() const noexcept { return bytes_sent_; }

    /// Datagrams that could not be sent -- a full socket buffer, or a payload
    /// over the size limit.
    ///
    /// Counted rather than retried or blocked on. Blocking here would let the
    /// network stall the matching thread, which is the one thing a market data
    /// feed must never do to the venue it reports on.
    std::uint64_t send_failures() const noexcept { return send_failures_; }

    const std::string& last_error() const noexcept { return last_error_; }

  private:
    FeedConfig config_;
    int fd_ = -1;
    std::uint16_t bound_port_ = 0;
    bool multicast_ = false;
    std::vector<std::uint8_t> destination_;  // sockaddr_in, kept opaque here

    std::string last_error_;
    std::uint64_t datagrams_sent_ = 0;
    std::uint64_t bytes_sent_ = 0;
    std::uint64_t send_failures_ = 0;
};

/// Receives market data datagrams.
class UdpSubscriber {
  public:
    explicit UdpSubscriber(FeedConfig config);
    ~UdpSubscriber();

    UdpSubscriber(const UdpSubscriber&) = delete;
    UdpSubscriber& operator=(const UdpSubscriber&) = delete;

    /// Binds, and joins the multicast group when the address is one.
    bool open();
    void close();

    /// Receives one datagram into `out`, replacing its contents. Returns false
    /// when nothing was waiting; the socket is non-blocking.
    bool receive(std::vector<std::uint8_t>& out);

    /// Descriptor, for registering with an EventLoop.
    int fd() const noexcept { return fd_; }
    std::uint16_t port() const noexcept { return bound_port_; }

    std::uint64_t datagrams_received() const noexcept { return datagrams_received_; }

    const std::string& last_error() const noexcept { return last_error_; }

  private:
    FeedConfig config_;
    int fd_ = -1;
    std::uint16_t bound_port_ = 0;
    bool multicast_ = false;

    std::string last_error_;
    std::uint64_t datagrams_received_ = 0;
};

/// True when `address` is in the IPv4 multicast range, 224.0.0.0/4.
bool is_multicast_address(const std::string& address);

}  // namespace xc::net
