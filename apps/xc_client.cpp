// Drives order flow at an exchange_sim and checks what comes back.
//
//   xc_client --port N [--feed-port N] [--orders N] [--account N]
//             [--instrument N] [--quantity N] [--seed N]
//
// Reports what it sent, what the venue acknowledged, what traded, and whether
// the market data feed lost anything.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xc/net/udp_feed.hpp"
#include "xc/protocol/gap_detector.hpp"
#include "xc/protocol/messages.hpp"
#include "xc/util/build_info.hpp"

namespace {

struct Counters {
    std::uint64_t sent = 0;
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t executions = 0;
    std::uint64_t filled_quantity = 0;
};

int connect_to(const std::string& host, std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    return fd;
}

bool send_all(int fd, std::span<const std::uint8_t> bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t written = ::send(fd, bytes.data() + sent, bytes.size() - sent, 0);
        if (written <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

void print_usage(const char* program) {
    std::cout << "usage: " << program << " --port N [options]\n\n"
              << "  --host ADDR      venue address (default 127.0.0.1)\n"
              << "  --port N         TCP order entry port (required)\n"
              << "  --feed-port N    UDP market data port to subscribe to\n"
              << "  --feed-group A   market data address (default 127.0.0.1)\n"
              << "  --orders N       orders to send (default 1000)\n"
              << "  --account N      first account id to trade as (default 1)\n"
              << "  --connections N  simultaneous sessions, one account each (default 2)\n"
              << "  --instrument N   instrument id (default 1)\n"
              << "  --quantity N     order quantity (default 10)\n"
              << "  --seed N         random seed (default 1)\n"
              << "  --help\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    std::string feed_group = "127.0.0.1";
    std::uint16_t port = 0;
    std::uint16_t feed_port = 0;
    std::uint64_t order_count = 1000;
    std::uint64_t account = 1;
    std::size_t connection_count = 2;
    std::uint64_t instrument = 1;
    xc::Quantity quantity = 10;
    std::uint64_t seed = 1;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--host") {
            host = next();
        } else if (arg == "--port") {
            port = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--feed-port") {
            feed_port = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--feed-group") {
            feed_group = next();
        } else if (arg == "--orders") {
            order_count = std::stoull(next());
        } else if (arg == "--account") {
            account = std::stoull(next());
        } else if (arg == "--connections") {
            connection_count = std::stoull(next());
        } else if (arg == "--instrument") {
            instrument = std::stoull(next());
        } else if (arg == "--quantity") {
            quantity = std::stoull(next());
        } else if (arg == "--seed") {
            seed = std::stoull(next());
        } else {
            std::cerr << "unrecognised argument: " << arg << "\n\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    if (port == 0) {
        std::cerr << "error: --port is required\n\n";
        print_usage(argv[0]);
        return 2;
    }

    std::unique_ptr<xc::net::UdpSubscriber> subscriber;
    if (feed_port != 0) {
        xc::net::FeedConfig feed_config;
        feed_config.address = feed_group;
        feed_config.port = feed_port;
        subscriber = std::make_unique<xc::net::UdpSubscriber>(feed_config);
        if (!subscriber->open()) {
            std::cerr << "error: cannot subscribe to the feed: " << subscriber->last_error()
                      << '\n';
            return 1;
        }
    }

    // One connection per account, because a session is pinned to the account it
    // first claims. It also makes the client able to trade with itself without
    // tripping self-trade prevention, which a single-account client cannot do:
    // every crossing order would be cancelled and the run would report no
    // executions at all.
    if (connection_count == 0) {
        connection_count = 1;
    }
    std::vector<int> sessions;
    for (std::size_t i = 0; i < connection_count; ++i) {
        const int session = connect_to(host, port);
        if (session < 0) {
            std::cerr << "error: cannot connect to " << host << ':' << port << ": "
                      << std::strerror(errno) << '\n';
            for (const int open_session : sessions) {
                ::close(open_session);
            }
            return 1;
        }
        sessions.push_back(session);
    }

    std::cout << xc::build_info().to_string() << '\n'
              << "connected to " << host << ':' << port << " with " << sessions.size()
              << " session(s), accounts " << account << ".." << account + connection_count - 1
              << '\n';

    std::mt19937_64 rng(seed);
    Counters counters;
    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> inbound;
    std::uint8_t chunk[16384];

    // The feed is drained as the run proceeds rather than at the end. A
    // subscriber that never reads lets its socket buffer overflow, and would
    // then report loss it caused itself.
    xc::protocol::GapDetector detector;
    bool detector_started = false;
    const auto drain_feed = [&] {
        if (!subscriber) {
            return;
        }
        std::vector<std::uint8_t> datagram;
        while (subscriber->receive(datagram)) {
            const xc::protocol::DecodeResult decoded = xc::protocol::decode(datagram);
            if (decoded.status != xc::protocol::DecodeStatus::Ok) {
                continue;
            }
            if (!detector_started) {
                // Joins wherever the feed happens to be, rather than counting
                // everything published before this client existed as loss.
                detector = xc::protocol::GapDetector(decoded.message.sequence);
                detector_started = true;
            }
            detector.observe(decoded.message.sequence);
        }
    };

    const auto drain_session = [&] {
        for (const int session : sessions) {
            while (true) {
                const ssize_t received = ::recv(session, chunk, sizeof(chunk), MSG_DONTWAIT);
                if (received <= 0) {
                    break;
                }
                inbound.insert(inbound.end(), chunk, chunk + received);
            }
        }

        std::size_t offset = 0;
        while (true) {
            const xc::protocol::DecodeResult decoded = xc::protocol::decode(
                std::span<const std::uint8_t>(inbound.data() + offset, inbound.size() - offset));
            if (decoded.status != xc::protocol::DecodeStatus::Ok) {
                break;
            }
            offset += decoded.consumed;
            switch (decoded.message.type) {
                case xc::protocol::MessageType::OrderAccepted:
                    ++counters.accepted;
                    break;
                case xc::protocol::MessageType::OrderRejected:
                    ++counters.rejected;
                    break;
                case xc::protocol::MessageType::Execution:
                    ++counters.executions;
                    counters.filled_quantity += decoded.message.fill.quantity;
                    break;
                default:
                    break;
            }
        }
        if (offset > 0) {
            inbound.erase(inbound.begin(), inbound.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    };

    for (std::uint64_t i = 1; i <= order_count; ++i) {
        const std::size_t slot = (i - 1) % sessions.size();

        xc::NewOrder command;
        command.id = xc::OrderId{i};
        command.account = xc::AccountId{account + slot};
        command.instrument = xc::InstrumentId{instrument};
        command.side = (rng() % 2 == 0) ? xc::Side::Buy : xc::Side::Sell;
        command.type = xc::OrderType::Limit;
        command.price = 10'000 + static_cast<xc::Price>(rng() % 21) - 10;
        command.quantity = quantity;

        request.clear();
        xc::protocol::encode_new_order(request, i, command);
        if (!send_all(sessions[slot], request)) {
            std::cerr << "error: send failed after " << counters.sent << " orders\n";
            break;
        }
        ++counters.sent;

        // Drained as the run proceeds, not only at the end. A client that only
        // writes fills the venue's outbound buffer and its own receive buffer,
        // and would be disconnected as a slow consumer for no reason other than
        // its own impatience.
        if (i % 64 == 0) {
            drain_session();
            drain_feed();
        }
    }

    // Wait for the venue to answer everything before closing.
    //
    // Not a fixed number of spins: closing a socket that still has unread data
    // queued makes the kernel send RST and discard it, so a client that gave up
    // early would destroy the replies it is about to report on -- and then
    // report the loss as though the venue had caused it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        drain_session();
        drain_feed();
        if (counters.accepted + counters.rejected >= counters.sent) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // One last pass: the final executions and feed messages can trail the
    // acknowledgement that triggered them.
    for (int i = 0; i < 50; ++i) {
        drain_session();
        drain_feed();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    for (const int session : sessions) {
        ::close(session);
    }

    const std::uint64_t answered = counters.accepted + counters.rejected;
    std::cout << "\norders sent:       " << counters.sent << '\n'
              << "accepted:          " << counters.accepted << '\n'
              << "rejected:          " << counters.rejected << '\n'
              << "executions:        " << counters.executions << '\n'
              << "quantity filled:   " << counters.filled_quantity << '\n';

    if (answered < counters.sent) {
        // Stated rather than glossed over: an order the venue never answered is
        // a real discrepancy, whatever caused it.
        std::cout << "\nwarning: " << counters.sent - answered
                  << " orders were never answered before the client gave up waiting.\n";
    }

    if (subscriber) {
        std::cout << "\nmarket data\n"
                  << "  datagrams:       " << subscriber->datagrams_received() << '\n'
                  << "  in order:        " << detector.in_order() << '\n'
                  << "  gaps:            " << detector.gaps() << '\n'
                  << "  messages missing:" << detector.missing() << '\n'
                  << "  duplicates:      " << detector.duplicates() << '\n';
        if (detector.gaps() > 0) {
            // Reported, not hidden. Loss on a UDP feed is expected under load;
            // a client that concealed it would be the actual problem.
            std::cout << "\nnote: the feed lost messages. On loopback this usually means the\n"
                         "      subscriber's socket buffer overflowed while it was busy.\n";
        }
    }
    return 0;
}
