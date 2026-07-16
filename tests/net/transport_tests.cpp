#include "engine/net/transport.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

namespace {

// Real loopback UDP through ENet, both endpoints pumped in-process.
TEST_CASE("client connects, exchanges messages, and disconnects", "[transport]") {
    constexpr std::uint16_t kPort = 47799;
    auto server = eng::NetHost::create_server(kPort, 4);
    REQUIRE(server.has_value());
    auto client = eng::NetHost::create_client();
    REQUIRE(client.has_value());

    const auto server_peer = client->connect("127.0.0.1", kPort);
    REQUIRE(server_peer.has_value());

    // Pump both sides until the handshake completes (or time out).
    bool client_connected = false;
    std::uint32_t client_peer_on_server = 0;
    std::vector<eng::NetEvent> events;
    for (int i = 0; i < 500 && (!client_connected || client_peer_on_server == 0); ++i) {
        events.clear();
        client->poll(events);
        for (const auto& e : events) {
            if (e.type == eng::NetEvent::Type::Connected) {
                client_connected = true;
            }
        }
        events.clear();
        server->poll(events);
        for (const auto& e : events) {
            if (e.type == eng::NetEvent::Type::Connected) {
                client_peer_on_server = e.peer;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    REQUIRE(client_connected);
    REQUIRE(client_peer_on_server != 0);
    CHECK(server->peer_count() == 1);

    // Client -> server on the reliable channel.
    const std::vector<std::uint8_t> hello = {1, 2, 3, 4};
    client->send(*server_peer, hello, eng::NetChannel::Reliable, true);

    std::vector<std::uint8_t> received;
    for (int i = 0; i < 500 && received.empty(); ++i) {
        events.clear();
        client->poll(events);
        events.clear();
        server->poll(events);
        for (const auto& e : events) {
            if (e.type == eng::NetEvent::Type::Message) {
                received = e.data;
                CHECK(e.peer == client_peer_on_server);
                CHECK(e.channel == eng::NetChannel::Reliable);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(received == hello);

    // Server -> client broadcast on the sequenced channel.
    const std::vector<std::uint8_t> pong = {9, 9};
    server->broadcast(pong, eng::NetChannel::Sequenced, false);
    bool got_pong = false;
    for (int i = 0; i < 500 && !got_pong; ++i) {
        events.clear();
        server->poll(events);
        events.clear();
        client->poll(events);
        for (const auto& e : events) {
            if (e.type == eng::NetEvent::Type::Message && e.data == pong) {
                got_pong = true;
                CHECK(e.channel == eng::NetChannel::Sequenced);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(got_pong);

    // Graceful disconnect reaches the server.
    client->disconnect(*server_peer);
    bool server_saw_disconnect = false;
    for (int i = 0; i < 500 && !server_saw_disconnect; ++i) {
        events.clear();
        client->poll(events);
        events.clear();
        server->poll(events);
        for (const auto& e : events) {
            if (e.type == eng::NetEvent::Type::Disconnected) {
                server_saw_disconnect = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(server_saw_disconnect);
    CHECK(server->peer_count() == 0);
}

}  // namespace
