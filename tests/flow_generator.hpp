#pragma once

#include <cstdint>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "xc/core/order.hpp"
#include "xc/core/types.hpp"

namespace xc::testing {

/// One command in a generated stream.
///
/// A plain tagged struct rather than a variant so that a failing case can be
/// printed verbatim and pasted straight into a regression test.
struct Command {
    enum class Kind { Submit, Cancel, Replace } kind = Kind::Submit;
    Order order;
    OrderId target;
    Price new_price = kNoPrice;
    Quantity new_quantity = 0;
    SeqNum sequence = 0;
};

inline std::string describe(const Command& command) {
    std::ostringstream out;
    switch (command.kind) {
        case Command::Kind::Submit:
            out << "submit id=" << command.order.id.value()
                << " acct=" << command.order.account.value() << ' ' << to_string(command.order.side)
                << ' ' << to_string(command.order.type) << ' ' << to_string(command.order.tif)
                << " px=" << command.order.price << " qty=" << command.order.quantity
                << (command.order.post_only ? " post_only" : "");
            break;
        case Command::Kind::Cancel:
            out << "cancel id=" << command.target.value();
            break;
        case Command::Kind::Replace:
            out << "replace id=" << command.target.value() << " px=" << command.new_price
                << " qty=" << command.new_quantity;
            break;
    }
    return out.str();
}

/// Generates a reproducible stream of commands from a seed.
///
/// The price band is deliberately narrow and the account set deliberately small
/// so that orders actually collide. A generator that spreads uniformly over a
/// wide price range produces a book that rarely crosses and never self-trades,
/// which exercises almost none of the logic worth testing.
inline std::vector<Command> generate_flow(std::uint64_t seed, std::size_t count) {
    std::mt19937_64 rng(seed);
    auto pick = [&rng](int low, int high) {
        return std::uniform_int_distribution<int>(low, high)(rng);
    };

    std::vector<Command> commands;
    commands.reserve(count);
    std::vector<OrderId> issued;
    std::uint64_t next_id = 1;
    SeqNum sequence = 0;

    for (std::size_t i = 0; i < count; ++i) {
        Command command;
        command.sequence = ++sequence;

        const int roll = pick(0, 99);
        if (roll < 60 || issued.empty()) {
            command.kind = Command::Kind::Submit;
            Order& order = command.order;
            order.id = OrderId{next_id++};
            order.account = AccountId{static_cast<std::uint64_t>(pick(1, 3))};
            order.instrument = InstrumentId{1};
            order.side = pick(0, 1) == 0 ? Side::Buy : Side::Sell;
            order.quantity = static_cast<Quantity>(pick(1, 100));
            order.remaining = order.quantity;
            order.sequence = command.sequence;
            order.accepted_at = static_cast<Nanos>(command.sequence) * 1000;

            if (pick(0, 99) < 85) {
                order.type = OrderType::Limit;
                order.price = pick(95, 105);
            } else {
                order.type = OrderType::Market;
                order.price = kNoPrice;
            }

            const int tif_roll = pick(0, 99);
            if (tif_roll < 70) {
                order.tif = TimeInForce::Day;
            } else if (tif_roll < 85) {
                order.tif = TimeInForce::ImmediateOrCancel;
            } else if (tif_roll < 95) {
                order.tif = TimeInForce::FillOrKill;
            } else {
                order.tif = TimeInForce::GoodTilCancelled;
            }

            order.post_only = order.type == OrderType::Limit && pick(0, 99) < 10;
            issued.push_back(order.id);
        } else if (roll < 85) {
            command.kind = Command::Kind::Cancel;
            // Drawn from every id ever issued, not only live ones, so the
            // unknown-order path is exercised as well.
            command.target =
                issued[static_cast<std::size_t>(pick(0, static_cast<int>(issued.size()) - 1))];
        } else {
            command.kind = Command::Kind::Replace;
            command.target =
                issued[static_cast<std::size_t>(pick(0, static_cast<int>(issued.size()) - 1))];
            command.new_price = pick(95, 105);
            command.new_quantity = static_cast<Quantity>(pick(1, 120));
        }
        commands.push_back(command);
    }
    return commands;
}

}  // namespace xc::testing
