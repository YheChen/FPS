#include "game/client/rtc_transport.h"

#if defined(__EMSCRIPTEN__)

#include <utility>
#include <vector>

#include "engine/core/log.h"
#include "engine/net/byte_buffer.h"
#include "engine/net/webrtc_client.h"
#include "game/shared/protocol.h"

namespace game {

namespace {

std::vector<std::uint8_t> encode(const auto& message) {
    eng::ByteWriter writer;
    write(writer, message);
    return {writer.data().begin(), writer.data().end()};
}

class RtcClientTransport final : public eng::IClientTransport {
public:
    RtcClientTransport(std::unique_ptr<eng::IClientTransport> signalling,
                       std::unique_ptr<eng::WebRtcClient> rtc)
        : signalling_(std::move(signalling)), rtc_(std::move(rtc)) {}

    void poll(std::vector<eng::NetEvent>& out) override {
        if (dead_) {
            return;
        }
        pump_signalling(out);
        if (dead_) {
            return;
        }
        if (rtc_->failed()) {
            fail(out, "the peer connection failed");
            return;
        }
        pump_negotiation();
        pump_data(out);
    }

    void send(std::span<const std::uint8_t> data, eng::NetChannel channel, bool) override {
        // Reliability is fixed per channel at negotiation time, exactly as on
        // the server side, so the per-send flag has nowhere to go.
        if (dead_ || !announced_) {
            return;
        }
        rtc_->send(channel, data);
        ++stats_.packets_sent;
        stats_.bytes_sent += data.size();
    }

    void disconnect() override {
        rtc_->close();
        signalling_->disconnect();
        dead_ = true;
    }

    // RTCPeerConnection exposes RTT only through the async getStats() API,
    // which does not fit a synchronous per-frame call. The HUD shows 0 here
    // for the same reason it does over WebSockets.
    std::uint32_t rtt_ms() const override { return 0; }
    const eng::NetStats& stats() const override { return stats_; }

private:
    void fail(std::vector<eng::NetEvent>& out, const char* why) {
        eng::log::error("WebRTC: {}", why);
        dead_ = true;
        out.push_back({eng::NetEvent::Type::Disconnected, 0, eng::NetChannel::Reliable, {}});
    }

    // Queued until the socket reports open. emscripten_websocket_send_binary
    // silently drops before then, and the offer is usually ready first --
    // ICE gathering starts the moment the peer connection is created, while
    // the socket is still completing its HTTP upgrade.
    void signal(std::vector<std::uint8_t> message) {
        if (socket_open_) {
            signalling_->send(message, eng::NetChannel::Reliable, true);
        } else {
            pending_.push_back(std::move(message));
        }
    }

    void pump_signalling(std::vector<eng::NetEvent>& out) {
        std::vector<eng::NetEvent> events;
        signalling_->poll(events);
        for (eng::NetEvent& event : events) {
            switch (event.type) {
                case eng::NetEvent::Type::Connected:
                    socket_open_ = true;
                    eng::log::info("WebRTC: signalling socket open, {} message(s) queued",
                                   pending_.size());
                    for (std::vector<std::uint8_t>& message : pending_) {
                        signalling_->send(message, eng::NetChannel::Reliable, true);
                    }
                    pending_.clear();
                    // NOT forwarded: the game connects when WebRTC opens.
                    break;
                case eng::NetEvent::Type::Disconnected:
                    // Losing signalling before the DataChannel opens is fatal.
                    // After, it is survivable -- the channel carries the game
                    // and the socket has nothing left to do.
                    if (!announced_) {
                        fail(out, "signalling closed before the DataChannel opened");
                        return;
                    }
                    socket_open_ = false;
                    break;
                case eng::NetEvent::Type::Message:
                    handle_signalling_message(event.data);
                    break;
            }
        }
    }

    void handle_signalling_message(const std::vector<std::uint8_t>& data) {
        eng::ByteReader reader{{data.data(), data.size()}};
        const auto type = read_message_type(reader);
        if (!type) {
            return;
        }
        if (*type == MessageType::RtcAnswer) {
            if (const auto answer = read_rtc_answer(reader)) {
                eng::log::info("WebRTC: answer received ({} bytes of SDP)", answer->sdp.size());
                rtc_->set_remote_answer(answer->sdp);
            }
        } else if (*type == MessageType::RtcCandidate) {
            if (const auto candidate = read_rtc_candidate(reader)) {
                rtc_->add_remote_candidate(candidate->candidate, candidate->mid);
            }
        }
        // Anything else on the signalling socket is not ours to interpret.
        // The server does not send game traffic there.
    }

    void pump_negotiation() {
        if (!offer_sent_) {
            if (const auto sdp = rtc_->take_local_offer()) {
                eng::log::info("WebRTC: sending offer ({} bytes of SDP)", sdp->size());
                signal(encode(RtcOfferMsg{*sdp}));
                offer_sent_ = true;
            }
        }
        // Trickle candidates out as the browser finds them. Sending before
        // the offer is fine: the server queues them against the peer.
        std::string candidate;
        std::string mid;
        while (rtc_->take_local_candidate(candidate, mid)) {
            signal(encode(RtcCandidateMsg{candidate, mid}));
        }
    }

    void pump_data(std::vector<eng::NetEvent>& out) {
        if (!announced_) {
            if (!rtc_->is_open()) {
                return;
            }
            announced_ = true;
            eng::log::info("WebRTC: DataChannel open; game traffic moves off the socket");
            out.push_back({eng::NetEvent::Type::Connected, 0, eng::NetChannel::Reliable, {}});
        }

        eng::NetEvent event;
        event.type = eng::NetEvent::Type::Message;
        while (rtc_->take_message(event.channel, event.data)) {
            if (event.data.empty()) {
                continue;  // a zero-length frame carries nothing to decode
            }
            ++stats_.packets_received;
            stats_.bytes_received += event.data.size();
            out.push_back(event);
        }
    }

    std::unique_ptr<eng::IClientTransport> signalling_;
    std::unique_ptr<eng::WebRtcClient> rtc_;
    std::vector<std::vector<std::uint8_t>> pending_;
    bool socket_open_ = false;
    bool offer_sent_ = false;
    bool announced_ = false;  // Connected already reported upward
    bool dead_ = false;
    eng::NetStats stats_;
};

}  // namespace

std::unique_ptr<eng::IClientTransport> make_rtc_client_transport(
    const std::string& signalling_url) {
    // The peer connection comes first: ICE gathering is the slow half, and
    // starting it while the socket completes its upgrade means the offer is
    // usually ready by the time there is somewhere to send it.
    auto rtc = eng::WebRtcClient::create();
    if (!rtc) {
        return nullptr;
    }
    // Port 0: a browser URL always carries its own.
    auto signalling = eng::make_client_transport(signalling_url, 0);
    if (!signalling) {
        eng::log::error("WebRTC: could not open the signalling socket '{}'", signalling_url);
        return nullptr;
    }
    eng::log::info("WebRTC: signalling over '{}'", signalling_url);
    return std::make_unique<RtcClientTransport>(std::move(signalling), std::move(rtc));
}

}  // namespace game

#endif  // __EMSCRIPTEN__
