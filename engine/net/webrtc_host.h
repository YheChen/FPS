#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/net/transport.h"

// WebRTC DataChannel transport for the dedicated server.
//
// Why it exists: browsers cannot open UDP sockets, so the browser client
// currently reaches the server over WebSockets -- which is TCP, and therefore
// head-of-line blocks. One lost packet stalls every snapshot behind it, which
// is exactly the failure mode a 60 Hz shooter cannot absorb. A DataChannel
// configured unordered and unreliable is UDP semantics the browser will
// actually give us.
//
// Signalling is deliberately NOT built in. This class produces and consumes
// SDP and ICE candidates as opaque strings, and the caller carries them over
// whatever channel it likes -- the existing WebSocket transport, in the game's
// case. That keeps the transport testable in-process (a test can shuttle the
// strings directly) and stops a second listening socket appearing here.
namespace eng {

class WebRtcHost final : public IServerTransport {
public:
    struct Config {
        std::size_t max_peers = 8;
        // STUN/TURN URLs. Empty is correct for localhost and LAN, where host
        // candidates alone connect; a public deployment needs at least a STUN
        // server to discover its reflexive address.
        std::vector<std::string> ice_servers;
    };

    // One signalling message to hand to the remote peer, in order.
    struct Signal {
        enum class Type : std::uint8_t { Answer, Candidate };
        std::uint32_t peer = 0;
        Type type = Type::Answer;
        std::string data;  // SDP for Answer, candidate string for Candidate
        std::string mid;   // media id, only meaningful for Candidate
    };

    static std::optional<WebRtcHost> create(const Config& config);

    ~WebRtcHost() override;
    WebRtcHost(WebRtcHost&& other) noexcept;
    WebRtcHost& operator=(WebRtcHost&& other) noexcept;
    WebRtcHost(const WebRtcHost&) = delete;
    WebRtcHost& operator=(const WebRtcHost&) = delete;

    // --- signalling in ----------------------------------------------------
    // Accepts a client's offer and allocates a peer. Returns nullopt when the
    // host is full or the SDP is rejected. The answer and any candidates come
    // back out of take_signals().
    std::optional<std::uint32_t> accept_offer(const std::string& sdp);
    void add_remote_candidate(std::uint32_t peer, const std::string& candidate,
                              const std::string& mid);

    // --- signalling out ---------------------------------------------------
    // Moves queued signalling messages to `out`. Call every frame and forward
    // them; ICE will not complete if they are dropped.
    void take_signals(std::vector<Signal>& out);

    // --- IServerTransport --------------------------------------------------
    void poll(std::vector<NetEvent>& out) override;
    void send(std::uint32_t peer, std::span<const std::uint8_t> data, NetChannel channel,
              bool reliable) override;
    void broadcast(std::span<const std::uint8_t> data, NetChannel channel, bool reliable) override;
    void disconnect(std::uint32_t peer) override;
    std::size_t peer_count() const override;
    const NetStats& stats() const override;

private:
    WebRtcHost();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eng
