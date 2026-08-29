// Rebuilds engine state from a journal, and reports what it found.
//
//   replay --journal DIR [--verify HASH] [--depth N] [--quiet]
//
// Exits non-zero when the journal is not usable, or when --verify was given a
// digest the replay did not reproduce, so it can be used as a check in a script
// rather than only read by a person.
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

#include "xc/core/matching_engine.hpp"
#include "xc/core/state_digest.hpp"
#include "xc/journal/replayer.hpp"
#include "xc/util/build_info.hpp"

namespace {

void print_usage(const char* program) {
    std::cout << "usage: " << program << " --journal DIR [options]\n\n"
              << "  --journal DIR   directory holding the journal segments\n"
              << "  --verify HASH   fail unless the replayed state digest equals HASH\n"
              << "  --depth N       print N levels of depth per instrument (default 5)\n"
              << "  --quiet         print only the state digest\n"
              << "  --help          show this message\n";
}

std::string_view describe(xc::journal::RecoveryOutcome outcome) {
    switch (outcome) {
        case xc::journal::RecoveryOutcome::Clean:
            return "clean";
        case xc::journal::RecoveryOutcome::TornTail:
            return "torn tail (crash during a write)";
        case xc::journal::RecoveryOutcome::Damaged:
            return "damaged";
        case xc::journal::RecoveryOutcome::SequenceGap:
            return "records missing";
        case xc::journal::RecoveryOutcome::Unreadable:
            return "unreadable";
    }
    return "unknown";
}

void print_depth(const xc::MatchingEngine& engine, std::size_t levels) {
    xc::DepthSnapshot snapshot;
    for (const xc::InstrumentId id : engine.instruments()) {
        const xc::OrderBook* book = engine.book(id);
        if (book == nullptr) {
            continue;
        }
        book->depth(levels, snapshot);
        std::cout << "\n  " << book->instrument().symbol << "  (" << book->resting_order_count()
                  << " resting)\n";
        std::cout << "        bid                ask\n";
        for (std::size_t i = 0; i < levels; ++i) {
            const bool has_bid = i < snapshot.bids.size();
            const bool has_ask = i < snapshot.asks.size();
            if (!has_bid && !has_ask) {
                break;
            }
            std::cout << "    ";
            if (has_bid) {
                std::cout << std::setw(8) << snapshot.bids[i].quantity << " @ " << std::setw(8)
                          << snapshot.bids[i].price;
            } else {
                std::cout << std::setw(21) << " ";
            }
            std::cout << "   ";
            if (has_ask) {
                std::cout << std::setw(8) << snapshot.asks[i].price << " x " << std::setw(8)
                          << snapshot.asks[i].quantity;
            }
            std::cout << '\n';
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    std::string journal_directory;
    std::string expected_digest;
    std::size_t depth = 5;
    bool quiet = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--quiet") {
            quiet = true;
        } else if (arg == "--journal" && i + 1 < argc) {
            journal_directory = argv[++i];
        } else if (arg == "--verify" && i + 1 < argc) {
            expected_digest = argv[++i];
        } else if (arg == "--depth" && i + 1 < argc) {
            depth = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else {
            std::cerr << "unrecognised argument: " << arg << "\n\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    if (journal_directory.empty()) {
        std::cerr << "error: --journal is required\n\n";
        print_usage(argv[0]);
        return 2;
    }

    // Replay drives time from the journal. Reading a wall clock here would
    // stamp timestamps the original run never produced.
    xc::ManualClock clock;
    xc::MatchingEngine engine{clock};
    xc::journal::Replayer replayer(engine, clock);
    const xc::journal::ReplayReport report = replayer.replay(journal_directory);

    if (!quiet) {
        std::cout << xc::build_info().to_string() << '\n';
        std::cout << "journal:    " << journal_directory << '\n'
                  << "recovery:   " << describe(report.recovery.outcome) << '\n'
                  << "segments:   " << report.recovery.segments_read << '\n'
                  << "records:    " << report.recovery.records_recovered << '\n'
                  << "sequence:   " << report.recovery.last_sequence << '\n'
                  << "instruments:" << report.instruments_defined << '\n'
                  << "orders:     " << report.orders_submitted << '\n'
                  << "cancels:    " << report.cancels_applied << '\n'
                  << "amendments: " << report.replaces_applied << '\n';
        if (!report.recovery.message.empty()) {
            std::cout << "note:       " << report.recovery.message << '\n';
        }
    }

    std::cout << "state digest: 0x" << std::hex << std::setw(16) << std::setfill('0')
              << report.state_digest << std::dec << std::setfill(' ') << '\n';

    if (!quiet && depth > 0) {
        print_depth(engine, depth);
    }

    if (!report.recovery.usable()) {
        std::cerr << "\nerror: the journal is not usable -- " << report.recovery.message << '\n';
        return 1;
    }

    if (!expected_digest.empty()) {
        const auto expected = std::strtoull(expected_digest.c_str(), nullptr, 0);
        if (expected != report.state_digest) {
            std::cerr << "\nerror: replay produced a different state than expected\n"
                      << "  expected 0x" << std::hex << expected << "\n  got      0x"
                      << report.state_digest << std::dec << '\n';
            return 1;
        }
        std::cout << "verified: replayed state matches\n";
    }

    return 0;
}
