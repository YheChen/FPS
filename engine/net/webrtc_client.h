#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/net/transport.h"

// Browser-side WebRTC DataChannel, the counterpart to WebRtcHost.
//
// WHY: browsers cannot open UDP sockets, so the browser client reaches the
// server over WebSockets -- which is TCP, and therefore head-of-line blocks.
// One lost packet stalls every snapshot queued behind it, which is exactly
// the failure mode a 60 Hz shooter cannot absorb. A DataChannel configured
// unordered with no retransmits is UDP semantics the browser will give us.
//
// Emscripten ships no WebRTC binding, so the RTCPeerConnection lives in
// hand-written JS behind a small C boundary (see the .cpp). Everything
// crossing that boundary is a plain int, a C string or a byte buffer -- no
// embind, and no JS objects held on the C++ side.
//
// SIGNALLING IS DELIBERATELY NOT BUILT IN, exactly as on the server: this
// class produces and consumes SDP and ICE candidates as opaque strings, and
// the caller carries them over whatever channel it likes. That is what keeps
// engine/ from having to know the game's wire protocol -- the alternative is
// hardcoding message-type bytes here, where nothing would catch them going
// stale if the protocol is renumbered.
namespace eng {

#if defined(__EMSCRIPTEN__)

class WebRtcClient {
public:
    // Creates the peer connection and both channels and begins negotiation.
    // Returns nullptr when the browser has no RTCPeerConnection.
    static std::unique_ptr<WebRtcClient> create();

    ~WebRtcClient();
    WebRtcClient(const WebRtcClient&) = delete;
    WebRtcClient& operator=(const WebRtcClient&) = delete;

    // --- signalling out ---------------------------------------------------
    // The local offer, once the browser has produced it. Returns a value
    // exactly once; nullopt until then.
    std::optional<std::string> take_local_offer();
    // Pops one local ICE candidate. False when none is waiting.
    bool take_local_candidate(std::string& candidate, std::string& mid);

    // --- signalling in ----------------------------------------------------
    void set_remote_answer(const std::string& sdp);
    void add_remote_candidate(const std::string& candidate, const std::string& mid);

    // --- state ------------------------------------------------------------
    // True once BOTH channels are open. The caller must not report the
    // connection up before this: a half-open pair would drop whichever kind
    // of traffic went to the channel that never opened.
    bool is_open() const;
    bool failed() const;

    // --- data -------------------------------------------------------------
    void send(NetChannel channel, std::span<const std::uint8_t> data);
    // Pops one received message. False when the queue is empty.
    bool take_message(NetChannel& channel, std::vector<std::uint8_t>& out);

    void close();

private:
    WebRtcClient() = default;

    bool closed_ = false;
};

#endif  // __EMSCRIPTEN__

}  // namespace eng
