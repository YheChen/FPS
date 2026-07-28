#include "engine/net/webrtc_client.h"

#if defined(__EMSCRIPTEN__)

#include <emscripten/emscripten.h>

#include <deque>
#include <utility>

#include "engine/core/log.h"
#include "engine/net/byte_buffer.h"

// The RTCPeerConnection itself lives in JavaScript, because Emscripten has no
// binding for it. Everything crossing the boundary is a plain int, a C string
// or a byte buffer -- no embind, no JS objects held on the C++ side.
//
// State lives in a single JS-side object (Module.__fpsRtc) with queues the C++
// side drains once per frame, which keeps the async browser API away from the
// synchronous game loop.
// clang-format off
EM_JS(void, fps_rtc_init, (), {
    if (Module.__fpsRtc) { return; }
    Module.__fpsRtc = {
        pc: null,
        reliable: null,
        sequenced: null,
        offer: null,           // local SDP, once gathering is done
        candidates: [],        // local ICE candidates awaiting pickup
        messages: [],          // {channel: 0|1, data: Uint8Array}
        failed: false,
    };
});

// Creates the peer connection and both channels, and starts negotiation.
// Returns 1 on success. The offer is picked up later via fps_rtc_take_offer.
EM_JS(int, fps_rtc_start, (), {
    try {
        var s = Module.__fpsRtc;
        s.pc = new RTCPeerConnection({ iceServers: [] });

        // Labels and semantics must match WebRtcHost. `sequenced` is the
        // whole point: unordered with no retransmits is UDP behaviour, which
        // is what a WebSocket cannot give us.
        s.reliable = s.pc.createDataChannel('reliable');
        s.sequenced = s.pc.createDataChannel('sequenced',
                                             { ordered: false, maxRetransmits: 0 });
        s.reliable.binaryType = 'arraybuffer';
        s.sequenced.binaryType = 'arraybuffer';

        var receive = function(channel) {
            return function(event) {
                s.messages.push({ channel: channel,
                                  data: new Uint8Array(event.data) });
            };
        };
        s.reliable.onmessage = receive(0);
        s.sequenced.onmessage = receive(1);

        s.pc.onicecandidate = function(event) {
            if (event.candidate && event.candidate.candidate) {
                s.candidates.push({ candidate: event.candidate.candidate,
                                    mid: event.candidate.sdpMid || '0' });
            }
        };
        s.pc.onconnectionstatechange = function() {
            if (s.pc.connectionState === 'failed' ||
                s.pc.connectionState === 'closed') {
                s.failed = true;
            }
        };

        s.pc.createOffer().then(function(offer) {
            return s.pc.setLocalDescription(offer);
        }).then(function() {
            s.offer = s.pc.localDescription.sdp;
        }).catch(function(error) {
            console.error('WebRTC offer failed', error);
            s.failed = true;
        });
        return 1;
    } catch (error) {
        console.error('WebRTC start failed', error);
        return 0;
    }
});

EM_JS(int, fps_rtc_take_offer, (char* out, int max), {
    var s = Module.__fpsRtc;
    if (!s || !s.offer) { return 0; }
    var written = stringToUTF8(s.offer, out, max);
    s.offer = null;
    return written > 0 ? 1 : 0;
});

EM_JS(void, fps_rtc_set_answer, (const char* sdp), {
    var s = Module.__fpsRtc;
    if (!s || !s.pc) { return; }
    s.pc.setRemoteDescription({ type: 'answer', sdp: UTF8ToString(sdp) })
        .catch(function(error) { console.error('WebRTC setRemoteDescription', error); });
});

EM_JS(void, fps_rtc_add_candidate, (const char* candidate, const char* mid), {
    var s = Module.__fpsRtc;
    if (!s || !s.pc) { return; }
    s.pc.addIceCandidate({ candidate: UTF8ToString(candidate),
                           sdpMid: UTF8ToString(mid) })
        .catch(function(error) { console.error('WebRTC addIceCandidate', error); });
});

// Pops one local candidate. Returns 1 when one was written.
EM_JS(int, fps_rtc_take_candidate, (char* candidate_out, int candidate_max, char* mid_out,
                                    int mid_max), {
    var s = Module.__fpsRtc;
    if (!s || s.candidates.length === 0) { return 0; }
    var entry = s.candidates.shift();
    stringToUTF8(entry.candidate, candidate_out, candidate_max);
    stringToUTF8(entry.mid, mid_out, mid_max);
    return 1;
});

EM_JS(int, fps_rtc_is_open, (), {
    var s = Module.__fpsRtc;
    return (s && s.reliable && s.sequenced &&
            s.reliable.readyState === 'open' &&
            s.sequenced.readyState === 'open') ? 1 : 0;
});

EM_JS(int, fps_rtc_failed, (), {
    var s = Module.__fpsRtc;
    return (s && s.failed) ? 1 : 0;
});

EM_JS(void, fps_rtc_send, (int channel, const unsigned char* data, int length), {
    var s = Module.__fpsRtc;
    if (!s) { return; }
    var target = (channel === 1) ? s.sequenced : s.reliable;
    if (!target || target.readyState !== 'open') { return; }
    // Copy out of the WASM heap: the buffer is reused by the caller straight
    // after this returns, and send() may queue it.
    target.send(new Uint8Array(HEAPU8.subarray(data, data + length)));
});

// Size of the next queued message, or -1 when the queue is empty. The channel
// is written through `channel_out`.
EM_JS(int, fps_rtc_peek_message, (int* channel_out), {
    var s = Module.__fpsRtc;
    if (!s || s.messages.length === 0) { return -1; }
    setValue(channel_out, s.messages[0].channel, 'i32');
    return s.messages[0].data.length;
});

EM_JS(void, fps_rtc_take_message, (unsigned char* out), {
    var s = Module.__fpsRtc;
    if (!s || s.messages.length === 0) { return; }
    HEAPU8.set(s.messages.shift().data, out);
});

EM_JS(void, fps_rtc_close, (), {
    var s = Module.__fpsRtc;
    if (!s) { return; }
    if (s.pc) { s.pc.close(); }
    s.pc = null;
    s.reliable = null;
    s.sequenced = null;
    s.messages = [];
    s.candidates = [];
});
// clang-format on

namespace eng {

namespace {

constexpr int kMaxSdpBytes = 8192;
constexpr int kMaxCandidateBytes = 512;
constexpr int kMaxMidBytes = 64;

std::vector<std::uint8_t> encode_offer(const std::string& sdp) {
    ByteWriter writer;
    writer.u8(15);  // MessageType::RtcOffer -- see game/shared/protocol.h
    writer.long_str(sdp);
    return {writer.data().begin(), writer.data().end()};
}

std::vector<std::uint8_t> encode_candidate(const std::string& candidate, const std::string& mid) {
    ByteWriter writer;
    writer.u8(17);  // MessageType::RtcCandidate
    writer.long_str(candidate);
    writer.str(mid);
    return {writer.data().begin(), writer.data().end()};
}

}  // namespace

struct WebRtcClientTransport::Impl {
    std::unique_ptr<IClientTransport> signalling;
    bool started = false;  // fps_rtc_start called
    bool offer_sent = false;
    bool announced = false;  // Connected already reported upward
    bool dead = false;
    NetStats stats;
};

WebRtcClientTransport::WebRtcClientTransport() : impl_(std::make_unique<Impl>()) {}
WebRtcClientTransport::~WebRtcClientTransport() {
    fps_rtc_close();
}

std::unique_ptr<WebRtcClientTransport> WebRtcClientTransport::create(
    const std::string& signalling_url) {
    auto transport = std::unique_ptr<WebRtcClientTransport>(new WebRtcClientTransport());
    // Port is ignored: a browser URL always carries its own.
    transport->impl_->signalling = make_client_transport(signalling_url, 0);
    if (!transport->impl_->signalling) {
        log::error("WebRTC client: could not open the signalling socket");
        return nullptr;
    }
    fps_rtc_init();
    log::info("WebRTC client: signalling over '{}'", signalling_url);
    return transport;
}

void WebRtcClientTransport::poll(std::vector<NetEvent>& out) {
    Impl& impl = *impl_;
    if (impl.dead) {
        return;
    }

    // --- signalling --------------------------------------------------------
    std::vector<NetEvent> signalling_events;
    impl.signalling->poll(signalling_events);
    if (!signalling_events.empty()) {
        log::info("WebRTC client: {} signalling event(s), first type {}", signalling_events.size(),
                  static_cast<int>(signalling_events[0].type));
    }
    for (NetEvent& event : signalling_events) {
        if (event.type == NetEvent::Type::Connected) {
            // The socket is up; begin negotiation.
            if (!impl.started) {
                impl.started = fps_rtc_start() != 0;
                if (!impl.started) {
                    log::error("WebRTC client: RTCPeerConnection unavailable");
                    impl.dead = true;
                    out.push_back({NetEvent::Type::Disconnected, 0, NetChannel::Reliable, {}});
                    return;
                }
            }
            continue;  // NOT forwarded: the game connects when WebRTC opens
        }
        if (event.type == NetEvent::Type::Disconnected) {
            // Losing signalling before the DataChannel opens is fatal; after,
            // it is survivable, so it is only reported when still negotiating.
            if (!impl.announced) {
                impl.dead = true;
                out.push_back({NetEvent::Type::Disconnected, 0, NetChannel::Reliable, {}});
                return;
            }
            continue;
        }

        ByteReader reader{{event.data.data(), event.data.size()}};
        const auto type = reader.u8();
        if (!type) {
            continue;
        }
        if (*type == 16) {  // RtcAnswer
            if (const auto sdp = reader.long_str(kMaxSdpBytes)) {
                fps_rtc_set_answer(sdp->c_str());
            }
        } else if (*type == 17) {  // RtcCandidate
            const auto candidate = reader.long_str(kMaxCandidateBytes);
            const auto mid = reader.str(kMaxMidBytes);
            if (candidate && mid) {
                fps_rtc_add_candidate(candidate->c_str(), mid->c_str());
            }
        }
        // Anything else on the signalling socket is not ours to interpret.
    }

    if (!impl.started) {
        return;
    }
    if (fps_rtc_failed() != 0 && !impl.dead) {
        log::error("WebRTC client: connection failed");
        impl.dead = true;
        out.push_back({NetEvent::Type::Disconnected, 0, NetChannel::Reliable, {}});
        return;
    }

    // Ship the offer as soon as it is ready.
    if (!impl.offer_sent) {
        std::string sdp(kMaxSdpBytes, '\0');
        if (fps_rtc_take_offer(sdp.data(), kMaxSdpBytes) != 0) {
            sdp.resize(std::char_traits<char>::length(sdp.c_str()));
            const std::vector<std::uint8_t> message = encode_offer(sdp);
            impl.signalling->send(message, NetChannel::Reliable, true);
            impl.offer_sent = true;
        }
    }

    // Trickle local candidates out as the browser finds them.
    std::string candidate(kMaxCandidateBytes, '\0');
    std::string mid(kMaxMidBytes, '\0');
    while (fps_rtc_take_candidate(candidate.data(), kMaxCandidateBytes, mid.data(), kMaxMidBytes) !=
           0) {
        const std::vector<std::uint8_t> message = encode_candidate(candidate.c_str(), mid.c_str());
        impl.signalling->send(message, NetChannel::Reliable, true);
    }

    // --- the DataChannel ---------------------------------------------------
    if (!impl.announced && fps_rtc_is_open() != 0) {
        impl.announced = true;
        log::info("WebRTC client: DataChannel open");
        out.push_back({NetEvent::Type::Connected, 0, NetChannel::Reliable, {}});
    }
    if (!impl.announced) {
        return;
    }

    int channel = 0;
    for (int size = fps_rtc_peek_message(&channel); size >= 0;
         size = fps_rtc_peek_message(&channel)) {
        NetEvent event;
        event.type = NetEvent::Type::Message;
        event.channel = channel == 1 ? NetChannel::Sequenced : NetChannel::Reliable;
        event.data.resize(static_cast<std::size_t>(size));
        if (size > 0) {
            fps_rtc_take_message(event.data.data());
        } else {
            fps_rtc_take_message(nullptr);  // drop a zero-length frame
            continue;
        }
        ++impl.stats.packets_received;
        impl.stats.bytes_received += event.data.size();
        out.push_back(std::move(event));
    }
}

void WebRtcClientTransport::send(std::span<const std::uint8_t> data, NetChannel channel,
                                 bool /*reliable*/) {
    // Reliability is fixed per channel at negotiation time, exactly as on the
    // server side, so the per-send flag has nowhere to go.
    if (impl_->dead || !impl_->announced) {
        return;
    }
    fps_rtc_send(channel == NetChannel::Sequenced ? 1 : 0, data.data(),
                 static_cast<int>(data.size()));
    ++impl_->stats.packets_sent;
    impl_->stats.bytes_sent += data.size();
}

void WebRtcClientTransport::disconnect() {
    fps_rtc_close();
    if (impl_->signalling) {
        impl_->signalling->disconnect();
    }
    impl_->dead = true;
}

std::uint32_t WebRtcClientTransport::rtt_ms() const {
    // RTCPeerConnection exposes RTT only through the async getStats() API,
    // which does not fit a synchronous per-frame call. The HUD shows 0 here
    // for the same reason it does over WebSockets.
    return 0;
}

const NetStats& WebRtcClientTransport::stats() const {
    return impl_->stats;
}

}  // namespace eng

#endif  // __EMSCRIPTEN__
