// End-to-end latency: an order over TCP to its acknowledgement back, measured
// open-loop.
//
//   bench_wire [--orders N] [--rate N] [--warmup N]
//
// The venue runs on its own thread and the load generator on this one, so the
// generator is never blocked by the thing it is measuring.
//
// Why open-loop. The obvious way to measure this is to send an order, wait for
// the reply, time it, and repeat. That measures service time, not latency, and
// it has a specific failure: when the venue stalls, the generator stalls with
// it and simply sends fewer orders. The stall is never sampled, because no
// request was in flight to observe it, and the reported tail is missing exactly
// the events anyone cares about. This is coordinated omission, and it makes a
// system look better the worse it behaves.
//
// The fix is to decide when each order *should* be sent, before the run starts,
// and to measure from that intended time rather than from the moment the
// generator got round to sending it. If the generator falls behind, the orders
// it sends late are charged for being late.
//
// Both numbers are reported. The gap between them is the size of the lie the
// closed-loop measurement would have told.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "xc/protocol/messages.hpp"
#include "xc/util/build_info.hpp"
#include "xc/util/histogram.hpp"
#include "xc/venue.hpp"

namespace {

using Clock = std::chrono::steady_clock;

std::int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
        .count();
}

int connect_to(std::uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return -1;
    }
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    return fd;
}

void print_percentiles(std::string_view label, const xc::Histogram& histogram) {
    std::cout << "  " << std::left << std::setw(26) << label << std::right << " n=" << std::setw(8)
              << histogram.count() << "  p50=" << std::setw(8) << histogram.value_at(50.0)
              << "  p90=" << std::setw(8) << histogram.value_at(90.0) << "  p99=" << std::setw(9)
              << histogram.value_at(99.0) << "  p99.9=" << std::setw(10) << histogram.value_at(99.9)
              << "  max=" << std::setw(10) << histogram.max() << "\n";
}

void print_usage(const char* program) {
    std::cout << "usage: " << program << " [options]\n\n"
              << "  --orders N   orders to measure (default 200000)\n"
              << "  --rate N     target orders per second (default 100000)\n"
              << "  --warmup N   orders before measuring (default 20000)\n"
              << "  --help\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t order_count = 200'000;
    std::uint64_t target_rate = 100'000;
    std::uint64_t warmup = 20'000;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--orders") {
            order_count = std::stoull(next());
        } else if (arg == "--rate") {
            target_rate = std::stoull(next());
        } else if (arg == "--warmup") {
            warmup = std::stoull(next());
        } else {
            std::cerr << "unrecognised argument: " << arg << "\n\n";
            print_usage(argv[0]);
            return 2;
        }
    }
    if (target_rate == 0) {
        target_rate = 1;
    }

    std::signal(SIGPIPE, SIG_IGN);
    std::cout << xc::build_info().to_string();

    xc::VenueConfig config;
    config.gateway.bind_address = "127.0.0.1";
    config.gateway.port = 0;
    config.feed.address = "127.0.0.1";
    config.feed.port = 45999;

    xc::Instrument instrument;
    instrument.id = xc::InstrumentId{1};
    instrument.symbol = "BENCH";
    instrument.tick_size = 1;
    instrument.lot_size = 1;
    instrument.min_quantity = 1;
    instrument.self_trade_policy = xc::SelfTradePolicy::Allow;
    instrument.expected_resting_orders = order_count + warmup + 4096;
    config.instruments.push_back(instrument);

    xc::Venue venue(std::move(config));
    if (!venue.start()) {
        std::cerr << "error: " << venue.last_error() << '\n';
        return 1;
    }

    // The venue gets its own thread so the load generator is never blocked by
    // the system it is measuring -- which is the whole point of open-loop.
    std::atomic<bool> running{true};
    std::thread venue_thread([&] {
        while (running.load(std::memory_order_relaxed)) {
            venue.poll(1);
        }
    });

    const int fd = connect_to(venue.order_entry_port());
    if (fd < 0) {
        std::cerr << "error: cannot connect: " << std::strerror(errno) << '\n';
        running = false;
        venue_thread.join();
        return 1;
    }

    const std::uint64_t total = warmup + order_count;
    std::vector<std::int64_t> intended_at(total + 1, 0);
    std::vector<std::int64_t> sent_at(total + 1, 0);
    std::vector<std::int64_t> acked_at(total + 1, 0);

    std::vector<std::uint8_t> request;
    std::vector<std::uint8_t> inbound;
    inbound.reserve(1 << 20);
    std::uint8_t chunk[65536];

    std::uint64_t acked = 0;
    const auto drain = [&] {
        while (true) {
            const ssize_t received = ::recv(fd, chunk, sizeof(chunk), MSG_DONTWAIT);
            if (received <= 0) {
                break;
            }
            inbound.insert(inbound.end(), chunk, chunk + received);
        }
        std::size_t offset = 0;
        while (true) {
            const xc::protocol::DecodeResult decoded = xc::protocol::decode(
                std::span<const std::uint8_t>(inbound.data() + offset, inbound.size() - offset));
            if (decoded.status != xc::protocol::DecodeStatus::Ok) {
                break;
            }
            offset += decoded.consumed;
            if (decoded.message.type == xc::protocol::MessageType::OrderAccepted ||
                decoded.message.type == xc::protocol::MessageType::OrderRejected) {
                const std::uint64_t id =
                    decoded.message.type == xc::protocol::MessageType::OrderAccepted
                        ? decoded.message.ack.order.value()
                        : decoded.message.reject.order.value();
                if (id <= total && acked_at[id] == 0) {
                    acked_at[id] = now_ns();
                    ++acked;
                }
            }
        }
        if (offset > 0) {
            inbound.erase(inbound.begin(), inbound.begin() + static_cast<std::ptrdiff_t>(offset));
        }
    };

    // The schedule is fixed before the run starts. Falling behind it does not
    // move it: an order sent late is charged for being late.
    const auto interval_ns = static_cast<std::int64_t>(1'000'000'000ULL / target_rate);
    const std::int64_t start = now_ns();
    for (std::uint64_t id = 1; id <= total; ++id) {
        const std::int64_t due = start + static_cast<std::int64_t>(id - 1) * interval_ns;
        intended_at[id] = due;

        // Busy-wait rather than sleep: a sleep of a few microseconds is not
        // honoured to anything like that precision, and the error would land in
        // the measurement.
        while (now_ns() < due) {
            drain();
        }

        xc::NewOrder command;
        command.id = xc::OrderId{id};
        command.account = xc::AccountId{1};
        command.instrument = xc::InstrumentId{1};
        command.side = (id % 2 == 0) ? xc::Side::Buy : xc::Side::Sell;
        command.type = xc::OrderType::Limit;
        // Priced not to cross, so the measurement is of the round trip rather
        // than of how much matching one order happened to trigger.
        command.price = (id % 2 == 0) ? 9'000 : 11'000;
        command.quantity = 10;

        request.clear();
        xc::protocol::encode_new_order(request, id, command);

        sent_at[id] = now_ns();
        std::size_t written = 0;
        while (written < request.size()) {
            const ssize_t result =
                ::send(fd, request.data() + written, request.size() - written, 0);
            if (result <= 0) {
                if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    drain();
                    continue;
                }
                std::cerr << "error: send failed at order " << id << '\n';
                id = total;
                break;
            }
            written += static_cast<std::size_t>(result);
        }
        drain();
    }

    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (acked < total && Clock::now() < deadline) {
        drain();
    }
    const std::int64_t finish = now_ns();

    ::close(fd);
    running = false;
    venue_thread.join();

    xc::Histogram from_send;
    xc::Histogram from_intended;
    std::uint64_t missing = 0;
    for (std::uint64_t id = warmup + 1; id <= total; ++id) {
        if (acked_at[id] == 0) {
            ++missing;
            continue;
        }
        from_send.record(static_cast<std::uint64_t>(acked_at[id] - sent_at[id]));
        // Never negative: the intended time is fixed in advance and the send
        // can only be at or after it.
        from_intended.record(static_cast<std::uint64_t>(acked_at[id] - intended_at[id]));
    }

    const double seconds = static_cast<double>(finish - start) / 1e9;
    std::cout << "\nload\n"
              << "  target rate              " << target_rate << " orders/s\n"
              << "  orders measured          " << from_send.count() << '\n'
              << "  unanswered               " << missing << '\n'
              << "  wall time                " << std::fixed << std::setprecision(3) << seconds
              << " s\n"
              << "  achieved rate            " << std::setprecision(0)
              << static_cast<double>(total) / seconds << " orders/s\n"
              << std::defaultfloat;

    std::cout << "\nround-trip latency, nanoseconds: order sent to acknowledgement received\n";
    print_percentiles("from actual send time", from_send);
    print_percentiles("from intended send time", from_intended);

    const std::uint64_t closed_loop_p999 = from_send.value_at(99.9);
    const std::uint64_t open_loop_p999 = from_intended.value_at(99.9);
    std::cout << "\n  The second row is the honest one. The first is what a send-then-wait\n"
                 "  harness would have reported: when the venue stalls, such a harness\n"
                 "  stalls with it and never samples the stall.\n";
    if (open_loop_p999 > closed_loop_p999) {
        std::cout << "  Here that understates p99.9 by " << open_loop_p999 - closed_loop_p999
                  << " ns (" << std::fixed << std::setprecision(1)
                  << static_cast<double>(open_loop_p999) /
                         static_cast<double>(closed_loop_p999 == 0 ? 1 : closed_loop_p999)
                  << "x).\n"
                  << std::defaultfloat;
    } else {
        std::cout << "  Here the two agree, which means the generator kept up with its\n"
                     "  schedule -- the offered load was comfortably within capacity.\n";
    }

    if (missing > 0) {
        std::cout << "\nwarning: " << missing
                  << " orders were never acknowledged and are excluded from the\n"
                     "         percentiles above, which therefore understate the tail.\n";
    }
    return 0;
}
