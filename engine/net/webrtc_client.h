#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "engine/net/client_transport.h"

// Browser-side WebRTC DataChannel transport.
//
// Emscripten ships no WebRTC binding, so the RTCPeerConnection lives in
// hand-written JS (see webrtc_client.cpp) behind a small C boundary. This
// class is the C++ half.
//
// It OWNS a WebSocket transport and uses it purely for signalling: the offer
// goes out over the socket, the answer and ICE candidates come back, and once
// the DataChannel opens every byte of game traffic moves to WebRTC. The
// socket stays connected because ICE can renegotiate, but the game never
// sends over it again.
//
// Connected is not reported until the DataChannel is actually open, so the
// game's handshake happens over WebRTC and the server sees exactly one
// session per player.
namespace eng {

#if defined(__EMSCRIPTEN__)

class WebRtcClientTransport final : public IClientTransport {
public:
    // `signalling_url` is the ws:// or wss:// URL of the same server.
    static std::unique_ptr<WebRtcClientTransport> create(const std::string& signalling_url);

    ~WebRtcClientTransport() override;

    void poll(std::vector<NetEvent>& out) override;
    void send(std::span<const std::uint8_t> data, NetChannel channel, bool reliable) override;
    void disconnect() override;
    std::uint32_t rtt_ms() const override;
    const NetStats& stats() const override;

private:
    WebRtcClientTransport();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif  // __EMSCRIPTEN__

}  // namespace eng
