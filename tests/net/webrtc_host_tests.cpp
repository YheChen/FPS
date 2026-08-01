#include "engine/net/webrtc_host.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <rtc/rtc.hpp>

// A real DataChannel connection, offerer and answerer in one process.
//
// This is what makes M19a landable on its own: libdatachannel can play the
// browser's role, so the transport is provable end-to-end -- ICE, DTLS, SCTP
// and both channels -- without a browser, a signalling server or a network.
// M19b's browser client then only has to speak the same signalling.
namespace {

using namespace std::chrono_literals;

// Spins the main thread until `predicate` holds or the budget runs out.
// libdatachannel completes negotiation on its own threads, so the test has to
// wait on outcomes rather than assume anything is synchronous.
template <typename Predicate>
bool wait_for(Predicate predicate, std::chrono::milliseconds budget = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string text_of(const std::vector<std::uint8_t>& data) {
    return {data.begin(), data.end()};
}

// The host accepts binary only, which is what a browser's
// dataChannel.send(ArrayBuffer) produces. Sending a std::string through
// libdatachannel would arrive as a TEXT message and be rejected -- so the
// test has to send the same shape a real client does.
rtc::binary binary_of(std::string_view text) {
    rtc::binary out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<std::byte>(c));
    }
    return out;
}

// The client half: a plain libdatachannel peer standing in for the browser.
struct FakeClient {
    rtc::PeerConnection connection;
    std::shared_ptr<rtc::DataChannel> reliable;
    std::shared_ptr<rtc::DataChannel> sequenced;

    std::mutex mutex;
    std::vector<std::string> received;
    std::string offer;
    std::atomic<bool> offer_ready{false};
    std::vector<std::pair<std::string, std::string>> candidates;  // (candidate, mid)

    FakeClient() {
        connection.onLocalDescription([this](rtc::Description description) {
            offer = std::string(description);
            offer_ready = true;
        });
        connection.onLocalCandidate([this](rtc::Candidate candidate) {
            const std::lock_guard lock{mutex};
            candidates.emplace_back(std::string(candidate), candidate.mid());
        });

        // Labels and semantics must match what WebRtcHost expects. The
        // sequenced channel is unordered with no retransmits: that is the
        // whole reason this transport exists instead of the WebSocket one.
        reliable = connection.createDataChannel("reliable");
        rtc::DataChannelInit unreliable;
        unreliable.reliability.unordered = true;
        unreliable.reliability.maxRetransmits = 0;
        sequenced = connection.createDataChannel("sequenced", unreliable);

        const auto on_message = [this](rtc::binary data) {
            const std::lock_guard lock{mutex};
            std::string text;
            text.reserve(data.size());
            for (const std::byte b : data) {
                text.push_back(static_cast<char>(b));
            }
            received.push_back(std::move(text));
        };
        reliable->onMessage(on_message, [](rtc::string) {});
        sequenced->onMessage(on_message, [](rtc::string) {});
    }

    // The callbacks above capture `this` and touch `mutex`, `received` and
    // `candidates` -- all of which are declared AFTER `connection` and so are
    // destroyed BEFORE it. Destroying the PeerConnection is when
    // libdatachannel joins its callback threads, so without unregistering
    // first, a message still in flight writes into destroyed members. Same
    // defect as the one WebRtcHost::Impl guards against, on the other side of
    // the connection.
    ~FakeClient() {
        reliable->resetCallbacks();
        sequenced->resetCallbacks();
        connection.resetCallbacks();
    }

    FakeClient(const FakeClient&) = delete;
    FakeClient& operator=(const FakeClient&) = delete;

    bool open() const { return reliable->isOpen() && sequenced->isOpen(); }

    std::size_t received_count() {
        const std::lock_guard lock{mutex};
        return received.size();
    }

    bool has_received(std::string_view text) {
        const std::lock_guard lock{mutex};
        return std::find(received.begin(), received.end(), text) != received.end();
    }
};

TEST_CASE("a DataChannel peer connects, and both channels carry traffic", "[webrtc]") {
    auto host = eng::WebRtcHost::create({.max_peers = 4, .ice_servers = {}});
    REQUIRE(host.has_value());
    CHECK(host->peer_count() == 0);

    FakeClient client;
    // The offer is only complete once ICE gathering finishes, so wait for it
    // rather than reading a half-built SDP.
    REQUIRE(wait_for([&] {
        return client.offer_ready.load() &&
               client.connection.gatheringState() == rtc::PeerConnection::GatheringState::Complete;
    }));

    const auto peer = host->accept_offer(client.offer);
    REQUIRE(peer.has_value());
    CHECK(host->peer_count() == 1);

    // Shuttle signalling by hand -- exactly what the WebSocket transport will
    // do for real clients in M19b.
    std::vector<eng::WebRtcHost::Signal> signals;
    REQUIRE(wait_for([&] {
        host->take_signals(signals);
        return std::any_of(signals.begin(), signals.end(), [](const auto& signal) {
            return signal.type == eng::WebRtcHost::Signal::Type::Answer;
        });
    }));
    for (const auto& signal : signals) {
        if (signal.type == eng::WebRtcHost::Signal::Type::Answer) {
            client.connection.setRemoteDescription(
                rtc::Description(signal.data, rtc::Description::Type::Answer));
        }
    }
    {
        const std::lock_guard lock{client.mutex};
        for (const auto& [candidate, mid] : client.candidates) {
            host->add_remote_candidate(*peer, candidate, mid);
        }
    }

    // Both sides open.
    std::vector<eng::NetEvent> events;
    REQUIRE(wait_for([&] {
        host->take_signals(signals);
        host->poll(events);
        return std::any_of(events.begin(), events.end(), [](const eng::NetEvent& event) {
            return event.type == eng::NetEvent::Type::Connected;
        });
    }));
    REQUIRE(wait_for([&] { return client.open(); }));

    // Server -> client on both channels.
    host->send(*peer, bytes_of("hello-reliable"), eng::NetChannel::Reliable, true);
    host->send(*peer, bytes_of("hello-sequenced"), eng::NetChannel::Sequenced, false);
    REQUIRE(wait_for([&] { return client.received_count() >= 2; }));
    CHECK(client.has_received("hello-reliable"));
    CHECK(client.has_received("hello-sequenced"));

    // Client -> server, and the channel each arrived on is preserved: the
    // game routes by channel, so crossing them would misroute every packet.
    client.reliable->send(binary_of("from-client-reliable"));
    client.sequenced->send(binary_of("from-client-sequenced"));

    bool got_reliable = false;
    bool got_sequenced = false;
    REQUIRE(wait_for([&] {
        events.clear();
        host->poll(events);
        for (const eng::NetEvent& event : events) {
            if (event.type != eng::NetEvent::Type::Message) {
                continue;
            }
            if (text_of(event.data) == "from-client-reliable") {
                got_reliable = event.channel == eng::NetChannel::Reliable;
            } else if (text_of(event.data) == "from-client-sequenced") {
                got_sequenced = event.channel == eng::NetChannel::Sequenced;
            }
        }
        return got_reliable && got_sequenced;
    }));
    CHECK(got_reliable);
    CHECK(got_sequenced);

    CHECK(host->stats().packets_sent >= 2);
    CHECK(host->stats().packets_received >= 2);
    CHECK(host->stats().bytes_received > 0);
}

TEST_CASE("a full host refuses further offers", "[webrtc]") {
    auto host = eng::WebRtcHost::create({.max_peers = 1, .ice_servers = {}});
    REQUIRE(host.has_value());

    FakeClient first;
    REQUIRE(wait_for([&] { return first.offer_ready.load(); }));
    REQUIRE(host->accept_offer(first.offer).has_value());

    FakeClient second;
    REQUIRE(wait_for([&] { return second.offer_ready.load(); }));
    // Rejected rather than over-subscribed: the game sizes its player array
    // from this limit.
    CHECK_FALSE(host->accept_offer(second.offer).has_value());
    CHECK(host->peer_count() == 1);
}

TEST_CASE("a malformed offer is rejected without leaking a peer", "[webrtc]") {
    auto host = eng::WebRtcHost::create({.max_peers = 4, .ice_servers = {}});
    REQUIRE(host.has_value());

    CHECK_FALSE(host->accept_offer("this is not sdp").has_value());
    CHECK_FALSE(host->accept_offer("").has_value());
    // The half-built peer must be cleaned up, or a client can exhaust the
    // host by spamming garbage.
    CHECK(host->peer_count() == 0);
}

TEST_CASE("operations on an unknown peer are ignored", "[webrtc]") {
    auto host = eng::WebRtcHost::create({.max_peers = 4, .ice_servers = {}});
    REQUIRE(host.has_value());

    // None of these should throw or allocate a peer.
    host->send(999, bytes_of("nobody"), eng::NetChannel::Reliable, true);
    host->add_remote_candidate(999, "candidate:0 1 UDP 1 127.0.0.1 1 typ host", "0");
    host->disconnect(999);
    host->broadcast(bytes_of("nobody"), eng::NetChannel::Sequenced, false);
    CHECK(host->peer_count() == 0);

    std::vector<eng::NetEvent> events;
    host->poll(events);
    CHECK(events.empty());
}

}  // namespace
