#include "game/server/signalling_router.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "engine/net/byte_buffer.h"
#include "game/shared/protocol.h"
#include "tests/server/fake_transport.h"

// The router is the piece that decides which bytes ServerGame is allowed to
// see, and which socket an answer goes back down. Both are addressing
// questions, and addressing bugs are invisible until two clients connect at
// once -- so most of these use two.
namespace {

// A WebRTC host with no ICE agent behind it: offers are accepted in order and
// every call is recorded.
class FakeHost final : public game::SignallingHost {
public:
    std::optional<std::uint32_t> accept_offer(const std::string& sdp) override {
        offers.push_back(sdp);
        if (full) {
            return std::nullopt;
        }
        return next_peer++;
    }

    void add_remote_candidate(std::uint32_t peer, const std::string& candidate,
                              const std::string& mid) override {
        candidates.push_back({peer, candidate, mid});
    }

    void take_signals(std::vector<Signal>& out) override {
        for (Signal& signal : queued) {
            out.push_back(std::move(signal));
        }
        queued.clear();
    }

    struct Candidate {
        std::uint32_t peer;
        std::string candidate;
        std::string mid;
    };

    bool full = false;
    std::uint32_t next_peer = 100;  // deliberately not the socket numbering
    std::vector<std::string> offers;
    std::vector<Candidate> candidates;
    std::vector<Signal> queued;
};

std::vector<std::uint8_t> bytes_of(const auto& message) {
    eng::ByteWriter writer;
    write(writer, message);
    return {writer.data().begin(), writer.data().end()};
}

eng::NetEvent message_from(std::uint32_t peer, std::vector<std::uint8_t> data) {
    return {eng::NetEvent::Type::Message, peer, eng::NetChannel::Reliable, std::move(data)};
}

TEST_CASE("an offer is consumed by the router and never reaches the game", "[signalling]") {
    FakeHost host;
    game::SignallingRouter router{host};

    const bool intercepted = router.intercept(message_from(7, bytes_of(game::RtcOfferMsg{"v=0"})));

    CHECK(intercepted);
    REQUIRE(host.offers.size() == 1);
    CHECK(host.offers[0] == "v=0");
    CHECK(router.negotiating() == 1);
}

TEST_CASE("the answer goes back down the socket that offered", "[signalling]") {
    FakeHost host;
    game::SignallingRouter router{host};
    test::FakeTransport net;

    // Two browsers negotiating at once, on socket peers that deliberately do
    // not match the WebRTC peer ids.
    router.intercept(message_from(7, bytes_of(game::RtcOfferMsg{"sdp-seven"})));
    router.intercept(message_from(9, bytes_of(game::RtcOfferMsg{"sdp-nine"})));
    REQUIRE(router.negotiating() == 2);

    // The host answers the SECOND peer first, which is the ordering that
    // catches a router that assumes signals come back in offer order.
    host.queued.push_back({101, game::SignallingHost::Signal::Type::Answer, "answer-nine", ""});
    host.queued.push_back({100, game::SignallingHost::Signal::Type::Answer, "answer-seven", ""});
    router.pump(net);

    REQUIRE(net.sent.size() == 2);
    CHECK(net.count_to(9, game::MessageType::RtcAnswer) == 1);
    CHECK(net.count_to(7, game::MessageType::RtcAnswer) == 1);

    // ...and each carries the SDP meant for it, not merely one each.
    for (const test::Sent& sent : net.sent) {
        eng::ByteReader reader{{sent.data.data(), sent.data.size()}};
        REQUIRE(game::read_message_type(reader) == game::MessageType::RtcAnswer);
        const auto answer = game::read_rtc_answer(reader);
        REQUIRE(answer);
        CHECK(answer->sdp == (sent.peer == 7 ? "answer-seven" : "answer-nine"));
    }
}

TEST_CASE("a candidate is routed to the peer that offered on that socket", "[signalling]") {
    FakeHost host;
    game::SignallingRouter router{host};

    router.intercept(message_from(7, bytes_of(game::RtcOfferMsg{"a"})));
    router.intercept(message_from(9, bytes_of(game::RtcOfferMsg{"b"})));
    const bool intercepted =
        router.intercept(message_from(9, bytes_of(game::RtcCandidateMsg{"cand-nine", "0"})));

    CHECK(intercepted);
    REQUIRE(host.candidates.size() == 1);
    CHECK(host.candidates[0].peer == 101);  // socket 9 -> the SECOND rtc peer
    CHECK(host.candidates[0].candidate == "cand-nine");
    CHECK(host.candidates[0].mid == "0");
}

TEST_CASE("a candidate with no offer before it is dropped, not misrouted", "[signalling]") {
    FakeHost host;
    game::SignallingRouter router{host};

    router.intercept(message_from(7, bytes_of(game::RtcOfferMsg{"a"})));
    // Socket 9 never offered. Routing this to peer 100 would feed one
    // browser's ICE candidate into another's connection.
    const bool intercepted =
        router.intercept(message_from(9, bytes_of(game::RtcCandidateMsg{"stray", "0"})));

    CHECK(intercepted);  // still not the game's business
    CHECK(host.candidates.empty());
}

TEST_CASE("a second offer on one socket cannot strand the first peer", "[signalling]") {
    FakeHost host;
    game::SignallingRouter router{host};

    router.intercept(message_from(7, bytes_of(game::RtcOfferMsg{"first"})));
    router.intercept(message_from(7, bytes_of(game::RtcOfferMsg{"second"})));

    // Only the first was accepted: a second peer inside the host would have
    // no socket routing its signals, so its ICE would never complete and the
    // slot would leak.
    CHECK(host.offers.size() == 1);
    CHECK(router.negotiating() == 1);
}

TEST_CASE("an offer the host refuses leaves no mapping behind", "[signalling]") {
    FakeHost host;
    host.full = true;
    game::SignallingRouter router{host};

    CHECK(router.intercept(message_from(7, bytes_of(game::RtcOfferMsg{"v=0"}))));
    CHECK(router.negotiating() == 0);
}

TEST_CASE("disconnect is forwarded to the game and clears the mapping", "[signalling]") {
    FakeHost host;
    game::SignallingRouter router{host};
    test::FakeTransport net;

    router.intercept(message_from(7, bytes_of(game::RtcOfferMsg{"v=0"})));
    const bool intercepted =
        router.intercept({eng::NetEvent::Type::Disconnected, 7, eng::NetChannel::Reliable, {}});

    // False: ServerGame has to hear about a peer leaving.
    CHECK_FALSE(intercepted);
    CHECK(router.negotiating() == 0);

    // A late signal for the departed peer must not be sent to whoever holds
    // socket 7 next.
    host.queued.push_back({100, game::SignallingHost::Signal::Type::Answer, "late", ""});
    router.pump(net);
    CHECK(net.sent.empty());
}

TEST_CASE("game traffic passes straight through", "[signalling]") {
    FakeHost host;
    game::SignallingRouter router{host};

    CHECK_FALSE(router.intercept(message_from(7, bytes_of(game::ClientHello{"player"}))));
    CHECK_FALSE(
        router.intercept({eng::NetEvent::Type::Connected, 7, eng::NetChannel::Reliable, {}}));
    // Garbage is not signalling either -- the game already has rules for it.
    CHECK_FALSE(router.intercept(message_from(7, {0xFF, 0xFF})));
    CHECK_FALSE(router.intercept(message_from(7, {})));
    CHECK(host.offers.empty());
}

TEST_CASE("a malformed offer is swallowed rather than passed on", "[signalling]") {
    FakeHost host;
    game::SignallingRouter router{host};

    // Right type byte, truncated body.
    std::vector<std::uint8_t> truncated{static_cast<std::uint8_t>(game::MessageType::RtcOffer),
                                        0x10};
    CHECK(router.intercept(message_from(7, truncated)));
    CHECK(host.offers.empty());
    CHECK(router.negotiating() == 0);
}

}  // namespace
