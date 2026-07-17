#include "engine/net/composite_transport.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

namespace {

// A fake transport that records sends and emits scripted events, so we can
// verify the composite's id tagging and routing without real sockets.
class FakeTransport final : public eng::IServerTransport {
public:
    std::vector<eng::NetEvent> pending;                        // returned by poll()
    std::vector<std::pair<std::uint32_t, std::uint8_t>> sent;  // (peer, first byte)
    std::vector<std::uint32_t> disconnected;
    int broadcasts = 0;
    eng::NetStats fake_stats;

    void poll(std::vector<eng::NetEvent>& out) override {
        for (auto& e : pending) {
            out.push_back(e);
        }
        pending.clear();
    }
    void send(std::uint32_t peer, std::span<const std::uint8_t> data, eng::NetChannel,
              bool) override {
        sent.emplace_back(peer, data.empty() ? 0 : data[0]);
    }
    void broadcast(std::span<const std::uint8_t>, eng::NetChannel, bool) override { ++broadcasts; }
    void disconnect(std::uint32_t peer) override { disconnected.push_back(peer); }
    std::size_t peer_count() const override { return peer_count_value; }
    const eng::NetStats& stats() const override { return fake_stats; }

    std::size_t peer_count_value = 0;
};

eng::NetEvent connect_event(std::uint32_t peer) {
    return {eng::NetEvent::Type::Connected, peer, eng::NetChannel::Reliable, {}};
}

TEST_CASE("composite tags child index into polled peer ids", "[composite]") {
    auto a = std::make_unique<FakeTransport>();
    auto b = std::make_unique<FakeTransport>();
    a->pending.push_back(connect_event(1));  // child 0, local 1
    b->pending.push_back(connect_event(1));  // child 1, local 1 (would collide!)

    std::vector<std::unique_ptr<eng::IServerTransport>> children;
    children.push_back(std::move(a));
    children.push_back(std::move(b));
    eng::CompositeTransport composite{std::move(children)};

    std::vector<eng::NetEvent> events;
    composite.poll(events);
    REQUIRE(events.size() == 2);
    // Distinct global ids despite identical child-local ids.
    CHECK(events[0].peer != events[1].peer);
    CHECK(eng::CompositeTransport::child_of(events[0].peer) == 0);
    CHECK(eng::CompositeTransport::child_of(events[1].peer) == 1);
    CHECK(eng::CompositeTransport::local_of(events[0].peer) == 1);
    CHECK(eng::CompositeTransport::local_of(events[1].peer) == 1);
}

TEST_CASE("composite routes send/disconnect to the owning child", "[composite]") {
    auto a = std::make_unique<FakeTransport>();
    auto b = std::make_unique<FakeTransport>();
    FakeTransport* a_raw = a.get();
    FakeTransport* b_raw = b.get();

    std::vector<std::unique_ptr<eng::IServerTransport>> children;
    children.push_back(std::move(a));
    children.push_back(std::move(b));
    eng::CompositeTransport composite{std::move(children)};

    const std::uint32_t peer_a = eng::CompositeTransport::make_global(0, 7);
    const std::uint32_t peer_b = eng::CompositeTransport::make_global(1, 42);
    const std::uint8_t payload_a[] = {0xAA};
    const std::uint8_t payload_b[] = {0xBB};

    composite.send(peer_a, payload_a, eng::NetChannel::Reliable, true);
    composite.send(peer_b, payload_b, eng::NetChannel::Reliable, true);
    composite.disconnect(peer_b);

    REQUIRE(a_raw->sent.size() == 1);
    CHECK(a_raw->sent[0].first == 7);  // child-local id, not global
    CHECK(a_raw->sent[0].second == 0xAA);
    REQUIRE(b_raw->sent.size() == 1);
    CHECK(b_raw->sent[0].first == 42);
    CHECK(b_raw->sent[0].second == 0xBB);
    REQUIRE(b_raw->disconnected.size() == 1);
    CHECK(b_raw->disconnected[0] == 42);
    CHECK(a_raw->disconnected.empty());
}

TEST_CASE("composite broadcast hits every child; stats and counts aggregate", "[composite]") {
    auto a = std::make_unique<FakeTransport>();
    auto b = std::make_unique<FakeTransport>();
    a->peer_count_value = 2;
    b->peer_count_value = 3;
    a->fake_stats.bytes_sent = 100;
    b->fake_stats.bytes_sent = 50;
    FakeTransport* a_raw = a.get();
    FakeTransport* b_raw = b.get();

    std::vector<std::unique_ptr<eng::IServerTransport>> children;
    children.push_back(std::move(a));
    children.push_back(std::move(b));
    eng::CompositeTransport composite{std::move(children)};

    const std::uint8_t payload[] = {1, 2, 3};
    composite.broadcast(payload, eng::NetChannel::Sequenced, false);
    CHECK(a_raw->broadcasts == 1);
    CHECK(b_raw->broadcasts == 1);
    CHECK(composite.peer_count() == 5);
    CHECK(composite.stats().bytes_sent == 150);
}

}  // namespace
