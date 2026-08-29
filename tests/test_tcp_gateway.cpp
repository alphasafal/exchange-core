#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "xc/net/tcp_gateway.hpp"

namespace xc::net {
namespace {

/// A blocking client socket, so tests read and write in a straight line rather
/// than driving two event loops against each other.
class TestClient {
  public:
    explicit TestClient(std::uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) {
            return;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~TestClient() { close(); }

    bool connected() const { return fd_ >= 0; }
    int fd() const { return fd_; }

    bool write(std::span<const std::uint8_t> bytes) {
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

    /// Reads until `count` bytes arrive or the socket errors.
    std::vector<std::uint8_t> read(std::size_t count) {
        std::vector<std::uint8_t> data;
        data.reserve(count);
        while (data.size() < count) {
            std::uint8_t chunk[4096];
            const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) {
                break;
            }
            data.insert(data.end(), chunk, chunk + n);
        }
        return data;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_ = -1;
};

NewOrder sample_order(std::uint64_t id, std::uint64_t account = 7) {
    NewOrder command;
    command.id = OrderId{id};
    command.account = AccountId{account};
    command.instrument = InstrumentId{1};
    command.side = Side::Buy;
    command.price = 100;
    command.quantity = 10;
    return command;
}

/// Polls until `predicate` holds or the budget runs out, so tests never depend
/// on a fixed sleep.
template<typename Predicate>
bool pump_until(TcpGateway& gateway, Predicate predicate, int max_iterations = 200) {
    for (int i = 0; i < max_iterations; ++i) {
        if (predicate()) {
            return true;
        }
        gateway.poll(10);
    }
    return predicate();
}

class GatewayTest : public ::testing::Test {
  protected:
    // Port zero: the kernel picks an ephemeral one, so concurrent test runs
    // cannot collide on a fixed port.
    TcpGateway gateway{GatewayConfig{.bind_address = "127.0.0.1", .port = 0}};

    std::vector<protocol::Message> received;
    std::vector<ConnectionId> connected;
    std::vector<std::string> disconnect_reasons;

    void SetUp() override {
        gateway.on_message([this](ConnectionId, const protocol::Message& message) {
            received.push_back(message);
        });
        gateway.on_connect([this](ConnectionId id) { connected.push_back(id); });
        gateway.on_disconnect([this](ConnectionId, std::string_view reason) {
            disconnect_reasons.emplace_back(reason);
        });
        ASSERT_TRUE(gateway.start()) << gateway.last_error();
        ASSERT_NE(gateway.port(), 0);
    }
};

TEST_F(GatewayTest, AcceptsAConnection) {
    TestClient client(gateway.port());
    ASSERT_TRUE(client.connected());

    EXPECT_TRUE(pump_until(gateway, [&] { return gateway.connection_count() == 1; }));
    EXPECT_EQ(gateway.connections_accepted(), 1u);
    EXPECT_EQ(connected.size(), 1u);
}

TEST_F(GatewayTest, DecodesAMessageFromAClient) {
    TestClient client(gateway.port());
    ASSERT_TRUE(client.connected());

    std::vector<std::uint8_t> bytes;
    protocol::encode_new_order(bytes, 1, sample_order(42));
    ASSERT_TRUE(client.write(bytes));

    ASSERT_TRUE(pump_until(gateway, [&] { return received.size() == 1; }));
    EXPECT_EQ(received[0].type, protocol::MessageType::NewOrder);
    EXPECT_EQ(received[0].new_order.id, OrderId{42});
}

TEST_F(GatewayTest, ReassemblesAMessageSplitAcrossPackets) {
    TestClient client(gateway.port());
    ASSERT_TRUE(client.connected());

    std::vector<std::uint8_t> bytes;
    protocol::encode_new_order(bytes, 1, sample_order(7));

    // TCP delivers bytes, not messages. Sending one byte at a time is the
    // extreme of a split that happens routinely at any size.
    for (const std::uint8_t byte : bytes) {
        ASSERT_TRUE(client.write(std::span<const std::uint8_t>(&byte, 1)));
        gateway.poll(1);
    }

    ASSERT_TRUE(pump_until(gateway, [&] { return received.size() == 1; }));
    EXPECT_EQ(received[0].new_order.id, OrderId{7});
}

TEST_F(GatewayTest, DecodesSeveralMessagesArrivingInOneRead) {
    TestClient client(gateway.port());
    ASSERT_TRUE(client.connected());

    std::vector<std::uint8_t> bytes;
    for (std::uint64_t i = 1; i <= 50; ++i) {
        protocol::encode_new_order(bytes, i, sample_order(i));
    }
    ASSERT_TRUE(client.write(bytes));

    ASSERT_TRUE(pump_until(gateway, [&] { return received.size() == 50; }));
    for (std::size_t i = 0; i < received.size(); ++i) {
        EXPECT_EQ(received[i].new_order.id, OrderId{i + 1});
    }
}

TEST_F(GatewayTest, SendsAReplyBackToTheClient) {
    TestClient client(gateway.port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(pump_until(gateway, [&] { return gateway.connection_count() == 1; }));

    std::vector<std::uint8_t> reply;
    protocol::encode_accepted(
        reply, 1, protocol::OrderAck{OrderId{5}, AccountId{7}, InstrumentId{1}, 0, 10, 123});
    ASSERT_TRUE(gateway.send(connected.front(), reply));

    const std::vector<std::uint8_t> got = client.read(reply.size());
    ASSERT_EQ(got.size(), reply.size());
    const protocol::DecodeResult decoded = protocol::decode(got);
    ASSERT_EQ(decoded.status, protocol::DecodeStatus::Ok);
    EXPECT_EQ(decoded.message.type, protocol::MessageType::OrderAccepted);
    EXPECT_EQ(decoded.message.ack.order, OrderId{5});
}

TEST_F(GatewayTest, NoticesWhenAClientDisconnects) {
    {
        TestClient client(gateway.port());
        ASSERT_TRUE(client.connected());
        ASSERT_TRUE(pump_until(gateway, [&] { return gateway.connection_count() == 1; }));
    }
    EXPECT_TRUE(pump_until(gateway, [&] { return gateway.connection_count() == 0; }));
    EXPECT_FALSE(disconnect_reasons.empty());
}

TEST_F(GatewayTest, ClosesAConnectionThatSendsGarbage) {
    TestClient client(gateway.port());
    ASSERT_TRUE(client.connected());
    // Waited for explicitly: without it the "no connections" assertion below
    // would pass before the client had even been accepted.
    ASSERT_TRUE(pump_until(gateway, [&] { return gateway.connection_count() == 1; }));

    // A declared version this build does not know. There is no framing marker
    // to resynchronise on, so nothing after it can be trusted to start a
    // message and the connection has to go.
    std::vector<std::uint8_t> bytes;
    protocol::encode_new_order(bytes, 1, sample_order(1));
    bytes[3] = 99;
    ASSERT_TRUE(client.write(bytes));

    EXPECT_TRUE(pump_until(gateway, [&] { return gateway.connection_count() == 0; }));
    ASSERT_FALSE(disconnect_reasons.empty());
    EXPECT_NE(disconnect_reasons.back().find("malformed"), std::string::npos);
    EXPECT_TRUE(received.empty());
}

TEST_F(GatewayTest, ClosesAConnectionThatSwitchesAccount) {
    TestClient client(gateway.port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(pump_until(gateway, [&] { return gateway.connection_count() == 1; }));

    std::vector<std::uint8_t> bytes;
    protocol::encode_new_order(bytes, 1, sample_order(1, /*account=*/7));
    protocol::encode_new_order(bytes, 2, sample_order(2, /*account=*/8));
    ASSERT_TRUE(client.write(bytes));

    // Not authentication -- the account is still whatever the client claimed
    // first -- but one session must not interleave orders for several firms.
    EXPECT_TRUE(pump_until(gateway, [&] { return gateway.connection_count() == 0; }));
    ASSERT_FALSE(disconnect_reasons.empty());
    EXPECT_NE(disconnect_reasons.back().find("account"), std::string::npos);
    EXPECT_EQ(received.size(), 1u) << "the first order was accepted before the switch";
}

TEST_F(GatewayTest, ServesManyConnectionsAtOnce) {
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 16; ++i) {
        clients.push_back(std::make_unique<TestClient>(gateway.port()));
        ASSERT_TRUE(clients.back()->connected());
    }
    ASSERT_TRUE(pump_until(gateway, [&] { return gateway.connection_count() == 16; }));

    for (std::size_t i = 0; i < clients.size(); ++i) {
        std::vector<std::uint8_t> bytes;
        protocol::encode_new_order(bytes, 1, sample_order(i + 1));
        ASSERT_TRUE(clients[i]->write(bytes));
    }

    ASSERT_TRUE(pump_until(gateway, [&] { return received.size() == 16; }));
    EXPECT_EQ(gateway.messages_received(), 16u);
}

TEST_F(GatewayTest, RefusesConnectionsBeyondItsLimit) {
    TcpGateway limited{GatewayConfig{.bind_address = "127.0.0.1", .port = 0, .max_connections = 2}};
    ASSERT_TRUE(limited.start()) << limited.last_error();

    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 5; ++i) {
        clients.push_back(std::make_unique<TestClient>(limited.port()));
    }
    for (int i = 0; i < 50; ++i) {
        limited.poll(5);
    }

    // Refused at connect time rather than accepted and quietly dropped: a
    // client told no can retry elsewhere, one silently dropped cannot tell what
    // happened.
    EXPECT_LE(limited.connection_count(), 2u);
}

TEST_F(GatewayTest, DisconnectsAClientThatWillNotReadItsReplies) {
    TcpGateway strict{
        GatewayConfig{.bind_address = "127.0.0.1", .port = 0, .max_outbound_bytes = 8 * 1024}};
    ConnectionId established = 0;
    std::vector<std::string> reasons;
    strict.on_connect([&](ConnectionId id) { established = id; });
    strict.on_disconnect(
        [&](ConnectionId, std::string_view reason) { reasons.emplace_back(reason); });
    ASSERT_TRUE(strict.start()) << strict.last_error();

    TestClient client(strict.port());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(pump_until(strict, [&] { return established != 0; }));

    // The client never reads. Once the kernel buffers fill, replies queue in
    // the venue -- and a client that cannot keep up with its own
    // acknowledgements must not be allowed to consume the venue's memory.
    std::vector<std::uint8_t> reply;
    protocol::encode_trade(reply, 1, Fill{});
    bool dropped = false;
    for (int i = 0; i < 100'000 && !dropped; ++i) {
        if (!strict.send(established, reply)) {
            dropped = true;
        }
        if (i % 500 == 0) {
            strict.poll(0);
        }
    }

    EXPECT_TRUE(dropped);
    EXPECT_EQ(strict.connection_count(), 0u);
    EXPECT_EQ(strict.connections_dropped_slow(), 1u);
    ASSERT_FALSE(reasons.empty());
    EXPECT_NE(reasons.back().find("outbound"), std::string::npos);
}

TEST_F(GatewayTest, SendingToAnUnknownConnectionFailsWithoutCrashing) {
    std::vector<std::uint8_t> reply;
    protocol::encode_heartbeat(reply, 1);
    EXPECT_FALSE(gateway.send(9999, reply));
}

TEST_F(GatewayTest, ReportsTheBoundPortWhenAskedForAnEphemeralOne) {
    EXPECT_NE(gateway.port(), 0) << "the kernel's choice has to be discoverable";
}

}  // namespace
}  // namespace xc::net
