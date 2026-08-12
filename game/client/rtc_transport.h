#pragma once

#if defined(__EMSCRIPTEN__)

#include <memory>
#include <string>

#include "engine/net/client_transport.h"

// Joins the two halves of a browser WebRTC connection: eng::WebRtcClient,
// which knows RTCPeerConnection but not the game's wire format, and a
// WebSocket transport, which carries the signalling.
//
// It lives in game/ rather than engine/ because encoding an offer as
// MessageType::RtcOffer is game protocol, and engine/ must not know that. The
// server is split the same way -- eng::WebRtcHost produces opaque SDP strings
// and a router in game/server addresses them.
//
// From the outside this is an ordinary IClientTransport. It reports Connected
// only once the DataChannel is open, so NetClient's handshake happens over
// WebRTC and the server sees exactly one session per player -- never a
// half-negotiated one.
namespace game {

std::unique_ptr<eng::IClientTransport> make_rtc_client_transport(const std::string& signalling_url);

}  // namespace game

#endif  // __EMSCRIPTEN__
