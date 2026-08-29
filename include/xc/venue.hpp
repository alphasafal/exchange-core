#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "xc/core/matching_engine.hpp"
#include "xc/journal/writer.hpp"
#include "xc/net/tcp_gateway.hpp"
#include "xc/net/udp_feed.hpp"
#include "xc/protocol/messages.hpp"
#include "xc/risk/kill_switch.hpp"
#include "xc/risk/risk_engine.hpp"

namespace xc {

struct VenueConfig {
    net::GatewayConfig gateway;
    net::FeedConfig feed;

    /// Journal directory. Absent runs the venue without durability, which is
    /// what the benchmarks use to isolate matching cost from storage cost.
    std::optional<journal::WriterConfig> journal;

    std::vector<Instrument> instruments;

    /// Applied to every account that trades. Left at defaults, this is
    /// unlimited.
    risk::AccountLimits default_limits;
    risk::InstrumentControls default_controls;

    /// Levels of depth published alongside each top-of-book update. Zero
    /// publishes only the touch.
    std::size_t published_depth = 0;
};

/// A single-node venue: order entry, matching, risk, journalling and market
/// data, wired together and driven from one thread.
///
/// One thread on purpose. Matching is what makes the venue's behaviour a pure
/// function of its command sequence, and every component that could reorder
/// commands relative to each other -- the gateway, the journal, the feed -- is
/// therefore driven from the same loop. Throughput scales by running a venue
/// per instrument group, not by threading one book.
class Venue final : public EngineListener {
  public:
    explicit Venue(VenueConfig config);
    ~Venue() override;

    bool start();
    void stop();

    /// Services order entry once, up to `timeout_ms`.
    bool poll(int timeout_ms);

    std::uint16_t order_entry_port() const noexcept { return gateway_.port(); }

    MatchingEngine& engine() noexcept { return engine_; }
    risk::RiskEngine& risk() noexcept { return risk_; }
    risk::KillSwitch& kill_switch() noexcept { return kill_; }
    net::TcpGateway& gateway() noexcept { return gateway_; }
    net::UdpPublisher& feed() noexcept { return feed_; }

    /// Sequence number of the last datagram published on the market data feed.
    SeqNum feed_sequence() const noexcept { return feed_sequence_; }

    std::uint64_t commands_processed() const noexcept { return commands_processed_; }

    const std::string& last_error() const noexcept { return last_error_; }

    // EngineListener. Session replies go to the connection that sent the
    // command; market data goes to every subscriber.
    void on_order_accepted(SeqNum sequence, const Order& order) override;
    void on_order_rejected(SeqNum sequence, const NewOrder& command, RejectReason reason) override;
    void on_order_cancelled(SeqNum sequence, const Order& order) override;
    void on_order_replaced(SeqNum sequence, const Order& previous, const Order& amended) override;
    void on_fills(SeqNum sequence, std::span<const Fill> fills) override;

  private:
    void handle(net::ConnectionId id, const protocol::Message& message);
    void publish_market_data(InstrumentId instrument);
    void send_session(std::span<const std::uint8_t> bytes);

    VenueConfig config_;
    SteadyClock clock_;
    MatchingEngine engine_{clock_};
    risk::RiskEngine risk_;
    risk::KillSwitch kill_;
    std::unique_ptr<journal::JournalWriter> journal_;
    net::TcpGateway gateway_;
    net::UdpPublisher feed_;

    /// The connection whose command is being processed, so listener callbacks
    /// know where a session reply belongs. Valid only inside handle().
    net::ConnectionId current_connection_ = 0;

    SeqNum feed_sequence_ = 0;
    std::uint64_t commands_processed_ = 0;
    std::string last_error_;

    /// Reused encoding buffers, so publishing a fill allocates nothing after
    /// the first few messages.
    std::vector<std::uint8_t> session_buffer_;
    std::vector<std::uint8_t> feed_buffer_;
    DepthSnapshot depth_;
};

}  // namespace xc
