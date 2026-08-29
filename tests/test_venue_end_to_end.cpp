#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <vector>

#include "xc/core/state_digest.hpp"
#include "xc/journal/replayer.hpp"
#include "xc/protocol/gap_detector.hpp"
#include "xc/venue.hpp"

namespace xc {
namespace {

class TempDir {
  public:
    TempDir() {
        path_ = std::filesystem::temp_directory_path() /
                ("xc-venue-test-" + std::to_string(::getpid()) + "-" + std::to_string(++counter_));
        std::filesystem::remove_all(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    const std::filesystem::path& path() const { return path_; }

  private:
    std::filesystem::path path_;
    static inline int counter_ = 0;
};

class Client {
  public:
    explicit Client(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~Client() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }

    bool connected() const { return fd_ >= 0; }

    bool send_all(std::span<const std::uint8_t> bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const ssize_t n = ::send(fd_, bytes.data() + sent, bytes.size() - sent, 0);
            if (n <= 0) {
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// Drains whatever has arrived, without blocking.
    void drain(std::vector<std::uint8_t>& into) {
        std::uint8_t chunk[8192];
        while (true) {
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), MSG_DONTWAIT);
            if (n <= 0) {
                return;
            }
            into.insert(into.end(), chunk, chunk + n);
        }
    }

  private:
    int fd_ = -1;
};

Instrument test_instrument() {
    Instrument instrument;
    instrument.id = InstrumentId{1};
    instrument.symbol = "AAPL";
    instrument.tick_size = 1;
    instrument.lot_size = 1;
    instrument.min_quantity = 1;
    instrument.self_trade_policy = SelfTradePolicy::CancelIncoming;
    return instrument;
}

NewOrder order(std::uint64_t id, std::uint64_t account, Side side, Price price, Quantity quantity) {
    NewOrder command;
    command.id = OrderId{id};
    command.account = AccountId{account};
    command.instrument = InstrumentId{1};
    command.side = side;
    command.price = price;
    command.quantity = quantity;
    return command;
}

/// A venue with order entry on an ephemeral TCP port and market data on an
/// ephemeral UDP port, plus a subscriber already bound to it.
struct Harness {
    explicit Harness(bool with_journal = false) {
        net::FeedConfig feed_config;
        feed_config.address = "127.0.0.1";
        feed_config.port = 0;
        subscriber = std::make_unique<net::UdpSubscriber>(feed_config);
        subscriber_ok = subscriber->open();

        VenueConfig config;
        config.gateway.bind_address = "127.0.0.1";
        config.gateway.port = 0;
        config.feed = feed_config;
        config.feed.port = subscriber->port();
        config.instruments = {test_instrument()};
        if (with_journal) {
            config.journal = journal::WriterConfig{.directory = journal_dir.path()};
        }

        venue = std::make_unique<Venue>(std::move(config));
        started = venue->start();
    }

    /// Polls the venue until `predicate` holds or the budget runs out.
    template<typename Predicate>
    bool pump_until(Predicate predicate, int iterations = 300) {
        for (int i = 0; i < iterations; ++i) {
            if (predicate()) {
                return true;
            }
            venue->poll(5);
        }
        return predicate();
    }

    std::vector<protocol::Message> drain_feed() {
        std::vector<protocol::Message> messages;
        std::vector<std::uint8_t> datagram;
        while (subscriber->receive(datagram)) {
            const protocol::DecodeResult decoded = protocol::decode(datagram);
            if (decoded.status == protocol::DecodeStatus::Ok) {
                messages.push_back(decoded.message);
            }
        }
        return messages;
    }

    TempDir journal_dir;
    std::unique_ptr<net::UdpSubscriber> subscriber;
    std::unique_ptr<Venue> venue;
    bool subscriber_ok = false;
    bool started = false;
};

std::vector<protocol::Message> decode_all(const std::vector<std::uint8_t>& bytes) {
    std::vector<protocol::Message> messages;
    std::span<const std::uint8_t> cursor(bytes);
    while (!cursor.empty()) {
        const protocol::DecodeResult decoded = protocol::decode(cursor);
        if (decoded.status != protocol::DecodeStatus::Ok) {
            break;
        }
        messages.push_back(decoded.message);
        cursor = cursor.subspan(decoded.consumed);
    }
    return messages;
}

TEST(VenueEndToEnd, AcknowledgesAnOrderOverTheWire) {
    Harness harness;
    ASSERT_TRUE(harness.subscriber_ok);
    ASSERT_TRUE(harness.started) << harness.venue->last_error();

    Client client(harness.venue->order_entry_port());
    ASSERT_TRUE(client.connected());

    std::vector<std::uint8_t> request;
    protocol::encode_new_order(request, 1, order(1, 100, Side::Buy, 10'000, 50));
    ASSERT_TRUE(client.send_all(request));

    std::vector<std::uint8_t> replies;
    ASSERT_TRUE(harness.pump_until([&] {
        client.drain(replies);
        return !decode_all(replies).empty();
    }));

    const std::vector<protocol::Message> messages = decode_all(replies);
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages[0].type, protocol::MessageType::OrderAccepted);
    EXPECT_EQ(messages[0].ack.order, OrderId{1});
    EXPECT_EQ(messages[0].ack.remaining, 50u);
}

TEST(VenueEndToEnd, MatchesTwoClientsAndReportsToBothChannels) {
    Harness harness;
    ASSERT_TRUE(harness.subscriber_ok);
    ASSERT_TRUE(harness.started) << harness.venue->last_error();

    Client maker(harness.venue->order_entry_port());
    Client taker(harness.venue->order_entry_port());
    ASSERT_TRUE(maker.connected() && taker.connected());

    std::vector<std::uint8_t> rest;
    protocol::encode_new_order(rest, 1, order(1, 100, Side::Sell, 10'000, 40));
    ASSERT_TRUE(maker.send_all(rest));
    ASSERT_TRUE(harness.pump_until([&] { return harness.venue->commands_processed() == 1; }));

    std::vector<std::uint8_t> cross;
    protocol::encode_new_order(cross, 2, order(2, 200, Side::Buy, 10'000, 40));
    ASSERT_TRUE(taker.send_all(cross));
    ASSERT_TRUE(harness.pump_until([&] { return harness.venue->commands_processed() == 2; }));

    std::vector<std::uint8_t> taker_replies;
    ASSERT_TRUE(harness.pump_until([&] {
        taker.drain(taker_replies);
        const std::vector<protocol::Message> messages = decode_all(taker_replies);
        return std::any_of(messages.begin(), messages.end(), [](const protocol::Message& m) {
            return m.type == protocol::MessageType::Execution;
        });
    }));

    // The aggressor's own execution arrives on its session...
    const std::vector<protocol::Message> session = decode_all(taker_replies);
    const auto execution = std::find_if(
        session.begin(), session.end(),
        [](const protocol::Message& m) { return m.type == protocol::MessageType::Execution; });
    ASSERT_NE(execution, session.end());
    EXPECT_EQ(execution->fill.price, 10'000);
    EXPECT_EQ(execution->fill.quantity, 40u);

    // ...and the trade is on the public feed for every subscriber.
    const std::vector<protocol::Message> feed = harness.drain_feed();
    const auto trade = std::find_if(feed.begin(), feed.end(), [](const protocol::Message& m) {
        return m.type == protocol::MessageType::Trade;
    });
    ASSERT_NE(trade, feed.end());
    EXPECT_EQ(trade->fill.price, 10'000);
    EXPECT_EQ(trade->fill.quantity, 40u);
}

TEST(VenueEndToEnd, PublishesAGapFreeFeedSequence) {
    Harness harness;
    ASSERT_TRUE(harness.subscriber_ok);
    ASSERT_TRUE(harness.started);

    Client client(harness.venue->order_entry_port());
    ASSERT_TRUE(client.connected());

    std::vector<std::uint8_t> batch;
    for (std::uint64_t i = 1; i <= 30; ++i) {
        protocol::encode_new_order(
            batch, i, order(i, 100, i % 2 == 0 ? Side::Buy : Side::Sell, 10'000 + (i % 5), 10));
    }
    ASSERT_TRUE(client.send_all(batch));
    ASSERT_TRUE(harness.pump_until([&] { return harness.venue->commands_processed() == 30; }));

    // Loopback does not reorder or drop, so every published datagram should
    // arrive exactly once and in order. Any gap here is the venue's own
    // numbering, not the network's.
    protocol::GapDetector detector;
    std::size_t seen = 0;
    for (const protocol::Message& message : harness.drain_feed()) {
        EXPECT_EQ(detector.observe(message.sequence), protocol::SequenceCheck::InOrder)
            << "at feed sequence " << message.sequence;
        ++seen;
    }
    EXPECT_GT(seen, 0u);
    EXPECT_EQ(detector.gaps(), 0u);
    EXPECT_EQ(detector.duplicates(), 0u);
}

TEST(VenueEndToEnd, RejectsAnOrderThatBreachesRiskAndSaysWhy) {
    net::FeedConfig feed_config;
    feed_config.address = "127.0.0.1";
    feed_config.port = 0;
    net::UdpSubscriber subscriber(feed_config);
    ASSERT_TRUE(subscriber.open());

    VenueConfig config;
    config.gateway.bind_address = "127.0.0.1";
    config.gateway.port = 0;
    config.feed = feed_config;
    config.feed.port = subscriber.port();
    config.instruments = {test_instrument()};
    config.default_limits.max_order_quantity = 100;

    Venue venue(std::move(config));
    ASSERT_TRUE(venue.start()) << venue.last_error();

    Client client(venue.order_entry_port());
    ASSERT_TRUE(client.connected());

    std::vector<std::uint8_t> request;
    protocol::encode_new_order(request, 1, order(1, 100, Side::Buy, 10'000, 1'000));
    ASSERT_TRUE(client.send_all(request));

    std::vector<std::uint8_t> replies;
    for (int i = 0; i < 300 && decode_all(replies).empty(); ++i) {
        venue.poll(5);
        client.drain(replies);
    }

    const std::vector<protocol::Message> messages = decode_all(replies);
    ASSERT_FALSE(messages.empty());
    EXPECT_EQ(messages[0].type, protocol::MessageType::OrderRejected);
    EXPECT_EQ(messages[0].reject.reason, RejectReason::RiskLimit);
    EXPECT_EQ(venue.engine().book(InstrumentId{1})->resting_order_count(), 0u);
}

TEST(VenueEndToEnd, JournalsWhatItMatchedAndReplaysToTheSameState) {
    Harness harness(/*with_journal=*/true);
    ASSERT_TRUE(harness.subscriber_ok);
    ASSERT_TRUE(harness.started) << harness.venue->last_error();

    // One connection per account: a session is pinned to the account it first
    // claims, so driving several firms down one socket is refused by design.
    Client buyer(harness.venue->order_entry_port());
    Client seller(harness.venue->order_entry_port());
    ASSERT_TRUE(buyer.connected() && seller.connected());

    std::vector<std::uint8_t> buys;
    std::vector<std::uint8_t> sells;
    for (std::uint64_t i = 1; i <= 40; ++i) {
        const bool buying = i % 2 == 0;
        protocol::encode_new_order(
            buying ? buys : sells, i,
            order(i, buying ? 100 : 200, buying ? Side::Buy : Side::Sell, 10'000 + (i % 7), 10));
    }
    ASSERT_TRUE(seller.send_all(sells));
    ASSERT_TRUE(buyer.send_all(buys));
    ASSERT_TRUE(harness.pump_until([&] { return harness.venue->commands_processed() == 40; }));

    const std::uint64_t live = digest(harness.venue->engine());
    harness.venue->stop();

    // The whole point of the journal, exercised through the real network path
    // rather than by calling the engine directly.
    ManualClock clock;
    MatchingEngine rebuilt{clock};
    journal::Replayer replayer(rebuilt, clock);
    const journal::ReplayReport report = replayer.replay(harness.journal_dir.path());

    EXPECT_EQ(report.recovery.outcome, journal::RecoveryOutcome::Clean);
    EXPECT_GT(report.orders_submitted, 0u);
    EXPECT_EQ(report.state_digest, live);
}

TEST(VenueEndToEnd, AHaltStopsNewOrdersAndStillAllowsCancels) {
    Harness harness;
    ASSERT_TRUE(harness.started);

    Client client(harness.venue->order_entry_port());
    ASSERT_TRUE(client.connected());

    std::vector<std::uint8_t> request;
    protocol::encode_new_order(request, 1, order(1, 100, Side::Buy, 10'000, 10));
    ASSERT_TRUE(client.send_all(request));
    ASSERT_TRUE(harness.pump_until([&] { return harness.venue->commands_processed() == 1; }));
    ASSERT_EQ(harness.venue->engine().book(InstrumentId{1})->resting_order_count(), 1u);

    harness.venue->kill_switch().halt_venue(risk::HaltReason::Manual, 0);

    std::vector<std::uint8_t> follow_up;
    protocol::encode_new_order(follow_up, 2, order(2, 100, Side::Buy, 10'000, 10));
    protocol::encode_cancel(follow_up, 3, CancelOrder{OrderId{1}, AccountId{100}, InstrumentId{1}});
    ASSERT_TRUE(client.send_all(follow_up));
    ASSERT_TRUE(harness.pump_until([&] { return harness.venue->commands_processed() == 3; }));

    // The new order is refused and the cancel goes through: a halted venue must
    // never trap orders that are already resting.
    EXPECT_EQ(harness.venue->engine().book(InstrumentId{1})->resting_order_count(), 0u);
}

}  // namespace
}  // namespace xc
