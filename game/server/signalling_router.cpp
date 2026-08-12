#include "game/server/signalling_router.h"

#include "engine/core/log.h"
#include "engine/net/byte_buffer.h"
#include "game/shared/protocol.h"

namespace game {

bool SignallingRouter::intercept(const eng::NetEvent& event) {
    if (event.type == eng::NetEvent::Type::Disconnected) {
        if (const auto found = socket_to_rtc_.find(event.peer); found != socket_to_rtc_.end()) {
            rtc_to_socket_.erase(found->second);
            socket_to_rtc_.erase(found);
        }
        return false;  // ServerGame still wants to know
    }
    if (event.type != eng::NetEvent::Type::Message) {
        return false;
    }

    eng::ByteReader reader{{event.data.data(), event.data.size()}};
    const auto type = read_message_type(reader);
    if (!type) {
        return false;  // not ours; let the game reject it as malformed
    }

    if (*type == MessageType::RtcOffer) {
        const auto offer = read_rtc_offer(reader);
        if (!offer) {
            eng::log::warn("Signalling peer {}: malformed offer", event.peer);
            return true;
        }
        // A second offer on the same socket would strand the first peer
        // inside the host with nothing routing its signals. One socket
        // negotiates one DataChannel.
        if (socket_to_rtc_.contains(event.peer)) {
            eng::log::warn("Signalling peer {}: already negotiating; ignoring a second offer",
                           event.peer);
            return true;
        }
        const auto rtc_peer = host_.accept_offer(offer->sdp);
        if (!rtc_peer) {
            eng::log::warn("Signalling peer {}: offer rejected (host full?)", event.peer);
            return true;
        }
        socket_to_rtc_[event.peer] = *rtc_peer;
        rtc_to_socket_[*rtc_peer] = event.peer;
        eng::log::info("Signalling peer {} -> WebRTC peer {}", event.peer, *rtc_peer);
        return true;
    }

    if (*type == MessageType::RtcCandidate) {
        const auto candidate = read_rtc_candidate(reader);
        const auto found = socket_to_rtc_.find(event.peer);
        if (candidate && found != socket_to_rtc_.end()) {
            host_.add_remote_candidate(found->second, candidate->candidate, candidate->mid);
        }
        // Swallowed either way: a candidate before an offer is a client bug
        // or a race, not something the game should see.
        return true;
    }

    return false;
}

void SignallingRouter::pump(eng::IServerTransport& net) {
    signals_.clear();
    host_.take_signals(signals_);
    for (const SignallingHost::Signal& signal : signals_) {
        const auto found = rtc_to_socket_.find(signal.peer);
        if (found == rtc_to_socket_.end()) {
            continue;  // the socket went away mid-negotiation
        }
        eng::ByteWriter writer;
        if (signal.type == SignallingHost::Signal::Type::Answer) {
            write(writer, RtcAnswerMsg{signal.data});
        } else {
            write(writer, RtcCandidateMsg{signal.data, signal.mid});
        }
        const std::vector<std::uint8_t> bytes{writer.data().begin(), writer.data().end()};
        net.send(found->second, bytes, eng::NetChannel::Reliable, true);
    }
}

}  // namespace game
