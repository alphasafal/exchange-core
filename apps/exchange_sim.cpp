// Runs a venue: TCP order entry, matching, risk, journalling, UDP market data.
//
//   exchange_sim [--port N] [--feed-port N] [--feed-group ADDR]
//                [--journal DIR] [--symbols A,B,C] [--depth N]
//                [--max-order-qty N] [--collar-bps N] [--stats-seconds N]
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "xc/util/build_info.hpp"
#include "xc/venue.hpp"

namespace {

volatile std::sig_atomic_t g_running = 1;

void handle_signal(int) {
    // Only a flag is set here. Anything more would be calling functions that
    // are not async-signal-safe from a signal handler, which is undefined
    // behaviour that usually appears to work.
    g_running = 0;
}

std::vector<std::string> split(std::string_view text, char separator) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream stream{std::string(text)};
    while (std::getline(stream, current, separator)) {
        if (!current.empty()) {
            parts.push_back(current);
        }
    }
    return parts;
}

void print_usage(const char* program) {
    std::cout << "usage: " << program << " [options]\n\n"
              << "  --port N            TCP order entry port (0 = ephemeral, default 0)\n"
              << "  --feed-port N       UDP market data port (default 0)\n"
              << "  --feed-group ADDR   market data destination (default 127.0.0.1)\n"
              << "  --journal DIR       write a journal to DIR (default: no journal)\n"
              << "  --symbols A,B,C     instruments to list (default AAPL)\n"
              << "  --depth N           depth levels to publish, 0 for top of book only\n"
              << "  --max-order-qty N   per-account maximum order quantity\n"
              << "  --collar-bps N      price collar in basis points, 0 to disable\n"
              << "  --stp POLICY        allow | cancel-incoming | cancel-resting |\n"
              << "                      cancel-both | decrement-both (default cancel-incoming)\n"
              << "  --stats-seconds N   how often to print counters (default 5)\n"
              << "  --help\n";
}

}  // namespace

int main(int argc, char** argv) {
    xc::VenueConfig config;
    config.gateway.bind_address = "127.0.0.1";
    config.gateway.port = 0;
    config.feed.address = "127.0.0.1";
    config.feed.port = 0;
    std::vector<std::string> symbols{"AAPL"};
    std::string journal_directory;
    xc::SelfTradePolicy stp = xc::SelfTradePolicy::CancelIncoming;
    int stats_seconds = 5;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--port") {
            config.gateway.port = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--feed-port") {
            config.feed.port = static_cast<std::uint16_t>(std::stoi(next()));
        } else if (arg == "--feed-group") {
            config.feed.address = next();
        } else if (arg == "--journal") {
            journal_directory = next();
            config.journal = xc::journal::WriterConfig{.directory = journal_directory};
        } else if (arg == "--symbols") {
            symbols = split(next(), ',');
        } else if (arg == "--depth") {
            config.published_depth = static_cast<std::size_t>(std::stoul(next()));
        } else if (arg == "--max-order-qty") {
            config.default_limits.max_order_quantity =
                static_cast<xc::Quantity>(std::stoull(next()));
        } else if (arg == "--collar-bps") {
            config.default_controls.collar_bps = static_cast<std::uint32_t>(std::stoul(next()));
        } else if (arg == "--stp") {
            const std::string policy = next();
            if (policy == "allow") {
                stp = xc::SelfTradePolicy::Allow;
            } else if (policy == "cancel-incoming") {
                stp = xc::SelfTradePolicy::CancelIncoming;
            } else if (policy == "cancel-resting") {
                stp = xc::SelfTradePolicy::CancelResting;
            } else if (policy == "cancel-both") {
                stp = xc::SelfTradePolicy::CancelBoth;
            } else if (policy == "decrement-both") {
                stp = xc::SelfTradePolicy::DecrementBoth;
            } else {
                std::cerr << "unrecognised self-trade policy: " << policy << '\n';
                return 2;
            }
        } else if (arg == "--stats-seconds") {
            stats_seconds = std::stoi(next());
        } else {
            std::cerr << "unrecognised argument: " << arg << "\n\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    for (std::size_t i = 0; i < symbols.size(); ++i) {
        xc::Instrument instrument;
        instrument.id = xc::InstrumentId{i + 1};
        instrument.symbol = symbols[i];
        instrument.tick_size = 1;
        instrument.lot_size = 1;
        instrument.min_quantity = 1;
        instrument.self_trade_policy = stp;
        config.instruments.push_back(instrument);
    }

    xc::Venue venue(std::move(config));
    if (!venue.start()) {
        std::cerr << "error: " << venue.last_error() << '\n';
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    // A client that disconnects mid-write would otherwise kill the process with
    // SIGPIPE. The gateway already handles the resulting error from send().
    std::signal(SIGPIPE, SIG_IGN);

    std::cout << xc::build_info().to_string() << '\n'
              << "order entry:  127.0.0.1:" << venue.order_entry_port() << " (tcp)\n"
              << "market data:  " << venue.feed().port() << " (udp)\n"
              << "instruments:  ";
    for (const std::string& symbol : symbols) {
        std::cout << symbol << ' ';
    }
    std::cout << "\nself-trade:   " << xc::to_string(stp)
              << "\njournal:      " << (journal_directory.empty() ? "(none)" : journal_directory)
              << "\nready. ctrl-c to stop.\n\n"
              << std::flush;

    auto last_report = std::chrono::steady_clock::now();
    while (g_running != 0) {
        if (!venue.poll(50)) {
            std::cerr << "error: " << venue.gateway().last_error() << '\n';
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (stats_seconds > 0 && now - last_report >= std::chrono::seconds(stats_seconds)) {
            last_report = now;
            std::cout << "connections " << venue.gateway().connection_count() << "  commands "
                      << venue.commands_processed() << "  msgs in "
                      << venue.gateway().messages_received() << "  feed seq "
                      << venue.feed_sequence() << "  feed sent " << venue.feed().datagrams_sent()
                      << "  feed failed " << venue.feed().send_failures() << '\n'
                      << std::flush;
        }
    }

    std::cout << "\nstopping.\n";
    venue.stop();
    return 0;
}
