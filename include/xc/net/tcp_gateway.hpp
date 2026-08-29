#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "xc/core/types.hpp"
#include "xc/net/event_loop.hpp"
#include "xc/protocol/messages.hpp"

namespace xc::net {

/// Identifies a client connection for the lifetime of the venue.
///
/// A generation counter, not a file descriptor. Descriptors are reused the
/// moment they are closed, so a reply addressed to a disconnected client by
/// descriptor can land on whoever connected next -- someone else's order
/// acknowledgement delivered to the wrong firm. Handles are never reused.
using ConnectionId = std::uint64_t;

struct GatewayConfig {
    std::string bind_address = "127.0.0.1";

    /// Port to listen on. Zero asks the kernel for an ephemeral one, which is
    /// what the tests use so that concurrent runs cannot collide.
    std::uint16_t port = 0;

    std::size_t max_connections = 1024;

    /// Bytes read from a socket per pass.
    std::size_t read_chunk = 64 * 1024;

    /// How much unsent data may queue for one client before it is disconnected.
    ///
    /// A client that cannot keep up with its own acknowledgements is already
    /// broken, and buffering for it without limit lets one slow reader consume
    /// the venue's memory until every other client suffers. Disconnecting is
    /// the outcome that contains the damage to the party causing it.
    std::size_t max_outbound_bytes = 4U << 20;
};

/// Accepts client connections and turns their byte streams into messages.
///
/// Single-threaded and non-blocking: one loop services every connection, and no
/// operation is allowed to block, because a blocking read on one client would
/// stall order entry for all of them.
///
/// **There is no authentication.** Every message carries the account it claims
/// to act for and the gateway believes it, beyond requiring that a connection
/// keep using the account it started with. A real venue authenticates the
/// session and binds it to an entitled account; that is out of scope here and
/// recorded in docs/limitations.md rather than half-implemented, since a
/// login message that checks nothing is worse than none at all -- it looks
/// like security.
class TcpGateway {
  public:
    explicit TcpGateway(GatewayConfig config);
    ~TcpGateway();

    TcpGateway(const TcpGateway&) = delete;
    TcpGateway& operator=(const TcpGateway&) = delete;

    /// Binds and listens. Returns false and sets last_error() on failure.
    bool start();
    void stop();

    /// Port actually bound. Meaningful after start(), and the way to discover
    /// which port the kernel chose when the configured port was zero.
    std::uint16_t port() const noexcept { return bound_port_; }

    /// Services ready connections once. `timeout_ms` bounds how long it waits.
    bool poll(int timeout_ms);

    /// Called for each decoded message. The handler runs on the polling thread
    /// and must not block.
    void on_message(std::function<void(ConnectionId, const protocol::Message&)> handler) {
        on_message_ = std::move(handler);
    }
    void on_connect(std::function<void(ConnectionId)> handler) { on_connect_ = std::move(handler); }
    void on_disconnect(std::function<void(ConnectionId, std::string_view)> handler) {
        on_disconnect_ = std::move(handler);
    }

    /// Queues bytes for a client. Returns false if the connection is unknown or
    /// was dropped for exceeding its outbound limit.
    bool send(ConnectionId id, std::span<const std::uint8_t> bytes);

    /// Closes a connection and reports why.
    void disconnect(ConnectionId id, std::string_view reason);

    std::size_t connection_count() const noexcept { return by_id_.size(); }
    std::uint64_t messages_received() const noexcept { return messages_received_; }
    std::uint64_t messages_sent() const noexcept { return messages_sent_; }
    std::uint64_t connections_accepted() const noexcept { return connections_accepted_; }
    std::uint64_t connections_dropped_slow() const noexcept { return dropped_slow_; }

    /// The account a connection is bound to, or an invalid id.
    AccountId account_for(ConnectionId id) const;

    const std::string& last_error() const noexcept { return last_error_; }

  private:
    struct Connection {
        ConnectionId id = 0;
        int fd = -1;
        AccountId account;

        std::vector<std::uint8_t> inbound;
        std::vector<std::uint8_t> outbound;

        /// How much of `outbound` has already been written. Sending consumes
        /// from the front, and erasing from the front of a vector on every
        /// partial write would be quadratic on a busy connection.
        std::size_t outbound_sent = 0;

        bool watching_write = false;
    };

    void accept_ready();
    void read_ready(Connection& connection);
    void write_ready(Connection& connection);
    bool drain(Connection& connection);
    void close_connection(ConnectionId id, std::string_view reason);
    void update_write_interest(Connection& connection);

    GatewayConfig config_;
    EventLoop loop_;
    int listen_fd_ = -1;
    std::uint16_t bound_port_ = 0;

    std::unordered_map<ConnectionId, Connection> by_id_;
    std::unordered_map<int, ConnectionId> by_fd_;
    ConnectionId next_id_ = 1;

    std::function<void(ConnectionId, const protocol::Message&)> on_message_;
    std::function<void(ConnectionId)> on_connect_;
    std::function<void(ConnectionId, std::string_view)> on_disconnect_;

    std::vector<Event> events_;
    std::vector<std::uint8_t> read_buffer_;

    std::string last_error_;
    std::uint64_t messages_received_ = 0;
    std::uint64_t messages_sent_ = 0;
    std::uint64_t connections_accepted_ = 0;
    std::uint64_t dropped_slow_ = 0;
};

}  // namespace xc::net
