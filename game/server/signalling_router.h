#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/net/transport.h"

// Routes WebRTC signalling that arrives over the WebSocket transport.
//
// It sits IN FRONT OF ServerGame rather than inside it. ServerGame has no
// business knowing which transports exist, and a signalling message reaching
// it would be counted as a malformed packet and eventually kick the client.
//
// A signalling socket is NOT a game session. The browser sends its
// ClientHello over the DataChannel once that opens, so the WebSocket peer
// stays forever "awaiting hello" and never becomes a player. The two peer
// spaces are different numbering schemes over different transports, which is
// the whole reason this class keeps a map in both directions.
namespace game {

// The slice of a WebRTC host that signalling actually needs.
//
// Narrow on purpose: libdatachannel is an optional dependency that the
// default build does not fetch, so binding the router directly to
// eng::WebRtcHost would make it unbuildable -- and untestable -- in exactly
// the configuration CI runs. A fake implements this in three lines.
class SignallingHost {
public:
    struct Signal {
        enum class Type : std::uint8_t { Answer, Candidate };
        std::uint32_t peer = 0;
        Type type = Type::Answer;
        std::string data;  // SDP for Answer, candidate string for Candidate
        std::string mid;   // media id, only meaningful for Candidate
    };

    virtual ~SignallingHost() = default;

    // Accepts a client's offer and allocates a peer, or nullopt when the host
    // is full or the SDP is rejected.
    virtual std::optional<std::uint32_t> accept_offer(const std::string& sdp) = 0;
    virtual void add_remote_candidate(std::uint32_t peer, const std::string& candidate,
                                      const std::string& mid) = 0;
    // Drains answers and candidates the host wants delivered to its peers.
    virtual void take_signals(std::vector<Signal>& out) = 0;
};

class SignallingRouter {
public:
    explicit SignallingRouter(SignallingHost& host) : host_(host) {}

    // True when the event was signalling and ServerGame must not see it.
    // A Disconnected event is never swallowed: it is forwarded so the game
    // can drop the peer, and only the signalling bookkeeping is cleaned up.
    bool intercept(const eng::NetEvent& event);

    // Pushes the host's queued answers and candidates back to the browser
    // over the socket it offered on. ICE stalls if these are not delivered,
    // so this has to run every frame, not only when something arrived.
    void pump(eng::IServerTransport& net);

    // Peers mid-negotiation. Exposed for tests and the server's status log.
    std::size_t negotiating() const { return socket_to_rtc_.size(); }

private:
    SignallingHost& host_;
    std::unordered_map<std::uint32_t, std::uint32_t> socket_to_rtc_;
    std::unordered_map<std::uint32_t, std::uint32_t> rtc_to_socket_;
    std::vector<SignallingHost::Signal> signals_;
};

}  // namespace game
