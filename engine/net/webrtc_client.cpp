#include "engine/net/webrtc_client.h"

#if defined(__EMSCRIPTEN__)

#include <emscripten/emscripten.h>

#include <string>

#include "engine/core/log.h"

// The RTCPeerConnection lives in JavaScript, because Emscripten has no
// binding for it. State lives in one JS-side object (Module.__fpsRtc) holding
// queues that the C++ side drains once per frame, which keeps an async
// browser API away from a synchronous game loop.
// clang-format off
EM_JS(int, fps_rtc_start, (), {
    if (typeof RTCPeerConnection === 'undefined') {
        return 0;
    }
    try {
        var s = {
            pc: null,
            reliable: null,
            sequenced: null,
            offer: null,      // local SDP, once setLocalDescription resolves
            candidates: [],   // local ICE candidates awaiting pickup
            messages: [],     // {channel: 0|1, data: Uint8Array}
            failed: false,
        };
        Module.__fpsRtc = s;

        s.pc = new RTCPeerConnection({ iceServers: [] });

        // Labels and semantics must match WebRtcHost. `sequenced` is the whole
        // point: unordered with no retransmits is UDP behaviour, which is
        // what a WebSocket cannot give us.
        s.reliable = s.pc.createDataChannel('reliable');
        s.sequenced = s.pc.createDataChannel('sequenced',
                                             { ordered: false, maxRetransmits: 0 });
        s.reliable.binaryType = 'arraybuffer';
        s.sequenced.binaryType = 'arraybuffer';

        var receive = function(channel) {
            return function(event) {
                s.messages.push({ channel: channel, data: new Uint8Array(event.data) });
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
            if (s.pc && (s.pc.connectionState === 'failed' ||
                         s.pc.connectionState === 'closed')) {
                s.failed = true;
            }
        };

        s.pc.createOffer().then(function(offer) {
            return s.pc.setLocalDescription(offer);
        }).then(function() {
            // localDescription rather than the offer we were handed: the
            // browser may rewrite it, and the server must answer the SDP
            // this peer actually committed to.
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

// Length of the pending offer in bytes (0 when none), so the C++ side can
// size its buffer before asking for the bytes. An SDP is a couple of KB and
// there is no fixed upper bound worth guessing at.
EM_JS(int, fps_rtc_offer_length, (), {
    var s = Module.__fpsRtc;
    return (s && s.offer) ? lengthBytesUTF8(s.offer) : 0;
});

EM_JS(void, fps_rtc_take_offer, (char* out, int max), {
    var s = Module.__fpsRtc;
    if (!s || !s.offer) { return; }
    stringToUTF8(s.offer, out, max);
    s.offer = null;
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
    s.pc.addIceCandidate({ candidate: UTF8ToString(candidate), sdpMid: UTF8ToString(mid) })
        .catch(function(error) { console.error('WebRTC addIceCandidate', error); });
});

// Pops one local candidate. Returns 1 when one was written.
EM_JS(int, fps_rtc_take_candidate, (char* candidate_out, int candidate_max,
                                    char* mid_out, int mid_max), {
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
    // Copy out of the WASM heap: the caller reuses its buffer as soon as this
    // returns, and send() may queue.
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
    var entry = s.messages.shift();
    if (out) { HEAPU8.set(entry.data, out); }
});

EM_JS(void, fps_rtc_close, (), {
    var s = Module.__fpsRtc;
    if (!s) { return; }
    // Drop the handlers before close(): onconnectionstatechange would
    // otherwise fire during teardown and set failed, turning a clean
    // disconnect into a reported failure.
    if (s.pc) {
        s.pc.onicecandidate = null;
        s.pc.onconnectionstatechange = null;
        s.pc.close();
    }
    Module.__fpsRtc = null;
});
// clang-format on

namespace eng {

namespace {
constexpr int kMaxCandidateBytes = 512;
constexpr int kMaxMidBytes = 64;
}  // namespace

std::unique_ptr<WebRtcClient> WebRtcClient::create() {
    if (fps_rtc_start() == 0) {
        log::error("WebRTC: this browser has no usable RTCPeerConnection");
        return nullptr;
    }
    log::info("WebRTC: negotiating a DataChannel");
    return std::unique_ptr<WebRtcClient>(new WebRtcClient());
}

WebRtcClient::~WebRtcClient() {
    close();
}

std::optional<std::string> WebRtcClient::take_local_offer() {
    const int length = fps_rtc_offer_length();
    if (length <= 0) {
        return std::nullopt;
    }
    // +1 for the NUL stringToUTF8 always writes.
    std::string sdp(static_cast<std::size_t>(length) + 1, '\0');
    fps_rtc_take_offer(sdp.data(), length + 1);
    sdp.resize(static_cast<std::size_t>(length));
    return sdp;
}

bool WebRtcClient::take_local_candidate(std::string& candidate, std::string& mid) {
    std::string candidate_buffer(kMaxCandidateBytes, '\0');
    std::string mid_buffer(kMaxMidBytes, '\0');
    if (fps_rtc_take_candidate(candidate_buffer.data(), kMaxCandidateBytes, mid_buffer.data(),
                               kMaxMidBytes) == 0) {
        return false;
    }
    candidate = candidate_buffer.c_str();
    mid = mid_buffer.c_str();
    return !candidate.empty() && !mid.empty();
}

void WebRtcClient::set_remote_answer(const std::string& sdp) {
    fps_rtc_set_answer(sdp.c_str());
}

void WebRtcClient::add_remote_candidate(const std::string& candidate, const std::string& mid) {
    fps_rtc_add_candidate(candidate.c_str(), mid.c_str());
}

bool WebRtcClient::is_open() const {
    return !closed_ && fps_rtc_is_open() != 0;
}

bool WebRtcClient::failed() const {
    return !closed_ && fps_rtc_failed() != 0;
}

void WebRtcClient::send(NetChannel channel, std::span<const std::uint8_t> data) {
    if (closed_ || data.empty()) {
        return;
    }
    fps_rtc_send(channel == NetChannel::Sequenced ? 1 : 0, data.data(),
                 static_cast<int>(data.size()));
}

bool WebRtcClient::take_message(NetChannel& channel, std::vector<std::uint8_t>& out) {
    if (closed_) {
        return false;
    }
    int raw_channel = 0;
    const int size = fps_rtc_peek_message(&raw_channel);
    if (size < 0) {
        return false;
    }
    channel = raw_channel == 1 ? NetChannel::Sequenced : NetChannel::Reliable;
    out.resize(static_cast<std::size_t>(size));
    fps_rtc_take_message(size > 0 ? out.data() : nullptr);
    return true;
}

void WebRtcClient::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    fps_rtc_close();
}

}  // namespace eng

#endif  // __EMSCRIPTEN__
