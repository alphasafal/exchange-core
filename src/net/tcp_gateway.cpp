#include "xc/net/tcp_gateway.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace xc::net {
namespace {

std::string errno_message(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

/// Compact a partially drained outbound buffer once the dead prefix dominates.
///
/// Erasing from the front on every partial write would be quadratic on a busy
/// connection; never compacting would let the buffer grow without bound even
/// though its live contents are small.
constexpr std::size_t kCompactThreshold = 64 * 1024;

}  // namespace

TcpGateway::TcpGateway(GatewayConfig config) : config_(std::move(config)) {
    read_buffer_.resize(config_.read_chunk);
    events_.reserve(256);
}

TcpGateway::~TcpGateway() {
    stop();
}

bool TcpGateway::start() {
    if (!loop_.valid()) {
        last_error_ = loop_.last_error();
        return false;
    }

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        last_error_ = errno_message("socket");
        return false;
    }

    if (!set_reuse_address(listen_fd_, last_error_) || !set_non_blocking(listen_fd_, last_error_)) {
        stop();
        return false;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config_.port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &address.sin_addr) != 1) {
        last_error_ = "invalid bind address: " + config_.bind_address;
        stop();
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        last_error_ = errno_message("bind");
        stop();
        return false;
    }
    if (::listen(listen_fd_, 128) != 0) {
        last_error_ = errno_message("listen");
        stop();
        return false;
    }

    // Read back what was actually bound, so a configured port of zero -- an
    // ephemeral port chosen by the kernel -- is discoverable by the caller.
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        bound_port_ = ntohs(bound.sin_port);
    }

    if (!loop_.add(listen_fd_, Interest::Read)) {
        last_error_ = loop_.last_error();
        stop();
        return false;
    }
    return true;
}

void TcpGateway::stop() {
    for (auto& [id, connection] : by_id_) {
        loop_.remove(connection.fd);
        ::close(connection.fd);
    }
    by_id_.clear();
    by_fd_.clear();

    if (listen_fd_ >= 0) {
        loop_.remove(listen_fd_);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

void TcpGateway::accept_ready() {
    // Accepts until the queue is empty rather than one per pass. A single
    // accept per readiness notification means a burst of connections is drained
    // one per loop iteration, which under load looks exactly like the venue
    // refusing to accept clients.
    while (true) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            last_error_ = errno_message("accept");
            return;
        }

        if (by_id_.size() >= config_.max_connections) {
            // Refused immediately rather than accepted and dropped later: a
            // client told no at connect time can retry elsewhere, while one
            // accepted and silently dropped cannot tell what happened.
            ::close(fd);
            continue;
        }

        std::string error;
        if (!set_non_blocking(fd, error) || !set_no_delay(fd, error)) {
            ::close(fd);
            continue;
        }

        const ConnectionId id = next_id_++;
        Connection connection;
        connection.id = id;
        connection.fd = fd;
        connection.inbound.reserve(config_.read_chunk);

        if (!loop_.add(fd, Interest::Read)) {
            ::close(fd);
            continue;
        }

        by_fd_.emplace(fd, id);
        by_id_.emplace(id, std::move(connection));
        ++connections_accepted_;

        if (on_connect_) {
            on_connect_(id);
        }
    }
}

void TcpGateway::read_ready(Connection& connection) {
    const ConnectionId id = connection.id;

    while (true) {
        const ssize_t received = ::recv(connection.fd, read_buffer_.data(), read_buffer_.size(), 0);
        if (received == 0) {
            close_connection(id, "peer closed the connection");
            return;
        }
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // Socket drained.
            }
            if (errno == EINTR) {
                continue;
            }
            close_connection(id, errno_message("recv"));
            return;
        }

        Connection* live = &by_id_.at(id);
        live->inbound.insert(live->inbound.end(), read_buffer_.begin(),
                             read_buffer_.begin() + received);

        // TCP delivers bytes, not messages: one read can contain several
        // messages, or a fraction of one, and both must be handled.
        std::size_t offset = 0;
        while (true) {
            const protocol::DecodeResult decoded = protocol::decode(std::span<const std::uint8_t>(
                live->inbound.data() + offset, live->inbound.size() - offset));
            if (decoded.status == protocol::DecodeStatus::Incomplete) {
                break;  // Wait for the rest.
            }
            if (decoded.status == protocol::DecodeStatus::Unreadable) {
                // There is no framing marker to resynchronise on, so the
                // remaining bytes on this connection cannot be trusted to start
                // a message. Closing is the only honest response.
                close_connection(id, "malformed message");
                return;
            }

            offset += decoded.consumed;
            ++messages_received_;

            // A connection may act for exactly one account. Not authentication
            // -- the account is still whatever the client claimed first -- but
            // it does stop one session interleaving orders for several firms.
            const AccountId claimed = decoded.message.new_order.account.valid()
                                          ? decoded.message.new_order.account
                                      : decoded.message.cancel_order.account.valid()
                                          ? decoded.message.cancel_order.account
                                          : decoded.message.replace_order.account;
            if (claimed.valid()) {
                if (!live->account.valid()) {
                    live->account = claimed;
                } else if (live->account != claimed) {
                    close_connection(id, "connection changed account");
                    return;
                }
            }

            if (on_message_) {
                on_message_(id, decoded.message);
            }

            // The handler may have closed this connection, and the map may have
            // rehashed while it ran, so the pointer is re-resolved rather than
            // reused.
            const auto it = by_id_.find(id);
            if (it == by_id_.end()) {
                return;
            }
            live = &it->second;
        }

        if (offset > 0) {
            live->inbound.erase(live->inbound.begin(),
                                live->inbound.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    }
}

bool TcpGateway::drain(Connection& connection) {
    while (connection.outbound_sent < connection.outbound.size()) {
        const std::size_t pending = connection.outbound.size() - connection.outbound_sent;
        const ssize_t written =
            ::send(connection.fd, connection.outbound.data() + connection.outbound_sent, pending,
#if defined(MSG_NOSIGNAL)
                   MSG_NOSIGNAL
#else
                   0
#endif
            );
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return true;  // Socket full; finish when it drains.
            }
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        connection.outbound_sent += static_cast<std::size_t>(written);
    }

    if (connection.outbound_sent == connection.outbound.size()) {
        connection.outbound.clear();
        connection.outbound_sent = 0;
    } else if (connection.outbound_sent >= kCompactThreshold) {
        connection.outbound.erase(
            connection.outbound.begin(),
            connection.outbound.begin() + static_cast<std::ptrdiff_t>(connection.outbound_sent));
        connection.outbound_sent = 0;
    }
    return true;
}

void TcpGateway::update_write_interest(Connection& connection) {
    const bool wants = connection.outbound_sent < connection.outbound.size();
    if (wants == connection.watching_write) {
        return;
    }
    // Write readiness is only registered while there is something to send.
    // Watching it permanently would wake the loop for every idle connection on
    // every pass, since an idle socket is almost always writable.
    connection.watching_write = wants;
    loop_.modify(connection.fd, wants ? Interest::ReadWrite : Interest::Read);
}

void TcpGateway::write_ready(Connection& connection) {
    const ConnectionId id = connection.id;
    if (!drain(connection)) {
        close_connection(id, errno_message("send"));
        return;
    }
    const auto it = by_id_.find(id);
    if (it != by_id_.end()) {
        update_write_interest(it->second);
    }
}

bool TcpGateway::send(ConnectionId id, std::span<const std::uint8_t> bytes) {
    const auto it = by_id_.find(id);
    if (it == by_id_.end()) {
        return false;
    }
    Connection& connection = it->second;

    const std::size_t queued = connection.outbound.size() - connection.outbound_sent;
    if (queued + bytes.size() > config_.max_outbound_bytes) {
        ++dropped_slow_;
        close_connection(id, "outbound buffer limit exceeded");
        return false;
    }

    connection.outbound.insert(connection.outbound.end(), bytes.begin(), bytes.end());
    ++messages_sent_;

    // Written immediately rather than deferred to the next poll. The common
    // case is a socket with room, and going straight to the syscall keeps an
    // acknowledgement from waiting for another trip round the loop.
    if (!drain(connection)) {
        close_connection(id, errno_message("send"));
        return false;
    }
    update_write_interest(connection);
    return true;
}

void TcpGateway::close_connection(ConnectionId id, std::string_view reason) {
    const auto it = by_id_.find(id);
    if (it == by_id_.end()) {
        return;
    }
    const int fd = it->second.fd;
    loop_.remove(fd);
    ::close(fd);
    by_fd_.erase(fd);
    by_id_.erase(it);

    if (on_disconnect_) {
        on_disconnect_(id, reason);
    }
}

void TcpGateway::disconnect(ConnectionId id, std::string_view reason) {
    close_connection(id, reason);
}

AccountId TcpGateway::account_for(ConnectionId id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? AccountId{} : it->second.account;
}

bool TcpGateway::poll(int timeout_ms) {
    events_.clear();
    if (!loop_.wait(timeout_ms, events_)) {
        last_error_ = loop_.last_error();
        return false;
    }

    for (const Event& event : events_) {
        if (event.fd == listen_fd_) {
            accept_ready();
            continue;
        }

        const auto fd_it = by_fd_.find(event.fd);
        if (fd_it == by_fd_.end()) {
            continue;  // Closed earlier in this same pass.
        }
        const ConnectionId id = fd_it->second;

        // Writability is serviced before readability. A connection with data
        // queued should be relieved of it before more work is added, or a
        // client that is both sending and slow to read can grow its outbound
        // buffer faster than it is drained.
        if (event.writable) {
            const auto it = by_id_.find(id);
            if (it != by_id_.end()) {
                write_ready(it->second);
            }
        }

        if (event.readable) {
            const auto it = by_id_.find(id);
            if (it != by_id_.end()) {
                read_ready(it->second);
            }
        }

        // Handled last: a closing socket may still have unread bytes, and
        // discarding a client's final orders because it hung up immediately
        // after sending them would lose work it had every right to expect done.
        if (event.closed && by_id_.contains(id)) {
            close_connection(id, "peer hung up");
        }
    }
    return true;
}

}  // namespace xc::net
