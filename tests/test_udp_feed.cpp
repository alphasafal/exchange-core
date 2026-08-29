#include <gtest/gtest.h>

#include <vector>

#include "xc/net/udp_feed.hpp"
#include "xc/protocol/gap_detector.hpp"
#include "xc/protocol/messages.hpp"

namespace xc::net {
namespace {

/// A publisher and subscriber wired together over loopback unicast.
///
/// Unicast rather than multicast on purpose. The code path is identical --
/// sendto() to a bound socket -- and multicast group membership cannot be
/// exercised reliably in CI, because the Linux loopback interface does not
/// carry the MULTICAST flag and joining a group on it fails. Testing over
/// unicast covers the framing, sequencing and loss-detection logic that
/// actually has behaviour in it; the multicast socket options are configuration
/// and are exercised by the exchange simulator, not asserted on here. That
/// limitation is recorded in docs/limitations.md rather than papered over with
/// a test that quietly does nothing.
struct Feed {
    Feed() {
        FeedConfig subscriber_config;
        subscriber_config.address = "127.0.0.1";
        subscriber_config.port = 0;
        subscriber = std::make_unique<UdpSubscriber>(subscriber_config);
        if (!subscriber->open()) {
            return;
        }

        FeedConfig publisher_config = subscriber_config;
        publisher_config.port = subscriber->port();
        publisher = std::make_unique<UdpPublisher>(publisher_config);
        publisher->open();
    }

    bool ready() const { return subscriber && publisher && subscriber->fd() >= 0; }

    /// Receives with a bounded retry, since a datagram is not necessarily
    /// visible to the receiver the instant sendto() returns.
    bool receive(std::vector<std::uint8_t>& out) {
        for (int i = 0; i < 500; ++i) {
            if (subscriber->receive(out)) {
                return true;
            }
        }
        return false;
    }

    std::unique_ptr<UdpSubscriber> subscriber;
    std::unique_ptr<UdpPublisher> publisher;
};

TEST(MulticastAddress, RecognisesTheReservedRange) {
    EXPECT_TRUE(is_multicast_address("224.0.0.1"));
    EXPECT_TRUE(is_multicast_address("239.255.255.255"));
    EXPECT_FALSE(is_multicast_address("223.255.255.255"));
    EXPECT_FALSE(is_multicast_address("240.0.0.1"));
    EXPECT_FALSE(is_multicast_address("127.0.0.1"));
    EXPECT_FALSE(is_multicast_address("not-an-address"));
}

TEST(UdpFeed, DeliversAPublishedMessage) {
    Feed feed;
    ASSERT_TRUE(feed.ready()) << feed.subscriber->last_error();

    std::vector<std::uint8_t> bytes;
    Fill fill;
    fill.id = TradeId{1};
    fill.instrument = InstrumentId{2};
    fill.price = 12'345;
    fill.quantity = 99;
    protocol::encode_trade(bytes, 1, fill);
    ASSERT_TRUE(feed.publisher->publish(bytes)) << feed.publisher->last_error();

    std::vector<std::uint8_t> received;
    ASSERT_TRUE(feed.receive(received));

    const protocol::DecodeResult decoded = protocol::decode(received);
    ASSERT_EQ(decoded.status, protocol::DecodeStatus::Ok);
    EXPECT_EQ(decoded.message.type, protocol::MessageType::Trade);
    EXPECT_EQ(decoded.message.fill.price, 12'345);
    EXPECT_EQ(decoded.message.fill.quantity, 99u);
}

TEST(UdpFeed, PreservesMessageBoundaries) {
    Feed feed;
    ASSERT_TRUE(feed.ready());

    // Unlike TCP, each datagram arrives whole or not at all, so a reader never
    // has to reassemble one.
    for (SeqNum i = 1; i <= 5; ++i) {
        std::vector<std::uint8_t> bytes;
        protocol::encode_heartbeat(bytes, i);
        ASSERT_TRUE(feed.publisher->publish(bytes));
    }

    for (SeqNum i = 1; i <= 5; ++i) {
        std::vector<std::uint8_t> received;
        ASSERT_TRUE(feed.receive(received)) << "datagram " << i;
        const protocol::DecodeResult decoded = protocol::decode(received);
        ASSERT_EQ(decoded.status, protocol::DecodeStatus::Ok);
        EXPECT_EQ(decoded.consumed, received.size()) << "one message per datagram, exactly";
        EXPECT_EQ(decoded.message.sequence, i);
    }
}

TEST(UdpFeed, RefusesAnOversizedDatagramRatherThanFragmentingIt) {
    FeedConfig config;
    config.address = "127.0.0.1";
    config.port = 9999;
    config.max_datagram = 64;
    UdpPublisher publisher(config);
    ASSERT_TRUE(publisher.open()) << publisher.last_error();

    const std::vector<std::uint8_t> oversized(128, 0);
    // A fragmented datagram is lost entirely when any one fragment is, so
    // oversized market data does not degrade under loss -- it disappears.
    EXPECT_FALSE(publisher.publish(oversized));
    EXPECT_EQ(publisher.send_failures(), 1u);
    EXPECT_EQ(publisher.datagrams_sent(), 0u);
}

TEST(UdpFeed, ReceiveReportsNothingWhenTheSocketIsEmpty) {
    Feed feed;
    ASSERT_TRUE(feed.ready());

    // Non-blocking: the caller must be able to poll without stalling.
    std::vector<std::uint8_t> received;
    EXPECT_FALSE(feed.subscriber->receive(received));
    EXPECT_TRUE(received.empty());
}

TEST(UdpFeed, CountsWhatItSent) {
    Feed feed;
    ASSERT_TRUE(feed.ready());

    std::vector<std::uint8_t> bytes;
    protocol::encode_heartbeat(bytes, 1);
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(feed.publisher->publish(bytes));
    }
    EXPECT_EQ(feed.publisher->datagrams_sent(), 10u);
    EXPECT_EQ(feed.publisher->bytes_sent(), bytes.size() * 10);
    EXPECT_EQ(feed.publisher->send_failures(), 0u);
}

TEST(UdpFeed, ASubscriberDetectsLossFromSequenceNumbersAlone) {
    Feed feed;
    ASSERT_TRUE(feed.ready());

    // Publish 1, 2 and then 6: the shape of three datagrams dropped in flight,
    // which is the failure UDP cannot prevent and the protocol therefore has to
    // make visible.
    for (const SeqNum sequence : {SeqNum{1}, SeqNum{2}, SeqNum{6}}) {
        std::vector<std::uint8_t> bytes;
        protocol::encode_heartbeat(bytes, sequence);
        ASSERT_TRUE(feed.publisher->publish(bytes));
    }

    protocol::GapDetector detector;
    std::vector<protocol::SequenceCheck> checks;
    for (int i = 0; i < 3; ++i) {
        std::vector<std::uint8_t> received;
        ASSERT_TRUE(feed.receive(received));
        const protocol::DecodeResult decoded = protocol::decode(received);
        ASSERT_EQ(decoded.status, protocol::DecodeStatus::Ok);
        checks.push_back(detector.observe(decoded.message.sequence));
    }

    EXPECT_EQ(checks[0], protocol::SequenceCheck::InOrder);
    EXPECT_EQ(checks[1], protocol::SequenceCheck::InOrder);
    EXPECT_EQ(checks[2], protocol::SequenceCheck::Gap);
    EXPECT_EQ(detector.missing(), 3u);
}

TEST(UdpFeed, FailsToOpenOnAnInvalidAddress) {
    FeedConfig config;
    config.address = "this is not an address";
    UdpPublisher publisher(config);
    EXPECT_FALSE(publisher.open());
    EXPECT_FALSE(publisher.last_error().empty());
}

TEST(UdpFeed, PublishingBeforeOpenFails) {
    FeedConfig config;
    UdpPublisher publisher(config);
    const std::vector<std::uint8_t> bytes(8, 0);
    EXPECT_FALSE(publisher.publish(bytes));
}

}  // namespace
}  // namespace xc::net
