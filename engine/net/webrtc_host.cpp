#include "engine/net/webrtc_host.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>

#include <rtc/rtc.hpp>

#include "engine/core/log.h"

namespace eng {

namespace {

// Two DataChannels per peer, mirroring the ENet channel split so ServerGame
// needs no idea which transport it is talking over:
//   Reliable  - reliable + ordered: handshake, events
//   Sequenced - unreliable + unordered: inputs, snapshots
//
// The Sequenced channel is the entire point of this transport. Leaving it
// ordered would reproduce exactly the head-of-line blocking that WebSockets
// already suffer.
constexpr const char* kReliableLabel = "reliable";
constexpr const char* kSequencedLabel = "sequenced";

rtc::Configuration make_configuration(const WebRtcHost::Config& config) {
    rtc::Configuration rtc_config;
    for (const std::string& url : config.ice_servers) {
        rtc_config.iceServers.emplace_back(url);
    }
    return rtc_config;
}

}  // namespace

struct WebRtcHost::Impl {
    struct Peer {
        std::shared_ptr<rtc::PeerConnection> connection;
        std::shared_ptr<rtc::DataChannel> reliable;
        std::shared_ptr<rtc::DataChannel> sequenced;
        bool announced = false;  // Connected event already emitted
        bool closed = false;
    };

    Config config;
    std::unordered_map<std::uint32_t, Peer> peers;
    std::uint32_t next_peer = 1;  // 0 is reserved (bots use it as "no peer")
    NetStats stats;

    // libdatachannel fires callbacks on its own threads, so anything they
    // touch is guarded and drained on the main thread in poll(). Nothing in
    // the engine outside this file ever sees another thread.
    std::mutex mutex;
    std::vector<NetEvent> pending_events;
    std::vector<Signal> pending_signals;

    void queue_event(NetEvent event) {
        const std::lock_guard lock{mutex};
        pending_events.push_back(std::move(event));
    }

    void queue_signal(Signal signal) {
        const std::lock_guard lock{mutex};
        pending_signals.push_back(std::move(signal));
    }

    // Wires the message/open/close callbacks for one channel.
    void bind_channel(std::uint32_t peer_id, const std::shared_ptr<rtc::DataChannel>& channel,
                      NetChannel kind) {
        channel->onMessage(
            [this, peer_id, kind](rtc::binary data) {
                NetEvent event;
                event.type = NetEvent::Type::Message;
                event.peer = peer_id;
                event.channel = kind;
                event.data.resize(data.size());
                std::transform(data.begin(), data.end(), event.data.begin(),
                               [](std::byte b) { return static_cast<std::uint8_t>(b); });
                queue_event(std::move(event));
            },
            // The game speaks binary only; a string message means something
            // is talking to us that is not our client.
            [peer_id](rtc::string) {
                log::warn("WebRTC peer {}: ignoring unexpected text message", peer_id);
            });

        channel->onOpen([this, peer_id]() { announce_if_ready(peer_id); });
        channel->onClosed([this, peer_id]() { queue_close(peer_id); });
    }

    // A peer is only "connected" once BOTH channels are open, so ServerGame
    // never gets a Connected event for a peer it cannot reliably answer.
    void announce_if_ready(std::uint32_t peer_id) {
        const std::lock_guard lock{mutex};
        const auto found = peers.find(peer_id);
        if (found == peers.end() || found->second.announced) {
            return;
        }
        Peer& peer = found->second;
        if (!peer.reliable || !peer.sequenced || !peer.reliable->isOpen() ||
            !peer.sequenced->isOpen()) {
            return;
        }
        peer.announced = true;
        NetEvent event;
        event.type = NetEvent::Type::Connected;
        event.peer = peer_id;
        pending_events.push_back(std::move(event));
    }

    void queue_close(std::uint32_t peer_id) {
        const std::lock_guard lock{mutex};
        const auto found = peers.find(peer_id);
        if (found == peers.end() || found->second.closed) {
            return;
        }
        found->second.closed = true;
        // Only report a disconnect for a peer the game was told about.
        if (!found->second.announced) {
            return;
        }
        NetEvent event;
        event.type = NetEvent::Type::Disconnected;
        event.peer = peer_id;
        pending_events.push_back(std::move(event));
    }

    // Peers MUST die before the state their callbacks touch.
    //
    // Members are destroyed in reverse declaration order, so without this
    // `mutex`, `pending_events` and `pending_signals` are already gone by the
    // time `peers` is destroyed -- and destroying a PeerConnection is exactly
    // when libdatachannel joins its callback threads, so a message or state
    // change still in flight lands in queue_event() and locks a destroyed
    // mutex. That is an intermittent segfault on shutdown: it needs a callback
    // to be mid-flight at the moment of teardown, so it reproduced in roughly
    // one CI run in ten and never locally.
    //
    // Reordering the members would also work and is easy to undo by accident.
    // This is explicit about the requirement.
    ~Impl() {
        // Same move-out-then-destroy discipline as poll(), and for the same
        // reason: destroying a peer joins threads whose callbacks want this
        // mutex, so it must not be held while that happens.
        std::unordered_map<std::uint32_t, Peer> doomed;
        {
            const std::lock_guard lock{mutex};
            doomed.swap(peers);
        }
        for (auto& [id, peer] : doomed) {
            // Unregister before destroying, so a callback that has already
            // been dispatched cannot re-enter this half-destroyed Impl.
            if (peer.reliable) {
                peer.reliable->resetCallbacks();
            }
            if (peer.sequenced) {
                peer.sequenced->resetCallbacks();
            }
            if (peer.connection) {
                peer.connection->resetCallbacks();
            }
        }
        doomed.clear();
    }

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
};

WebRtcHost::WebRtcHost() : impl_(std::make_unique<Impl>()) {}

WebRtcHost::~WebRtcHost() = default;
WebRtcHost::WebRtcHost(WebRtcHost&& other) noexcept = default;
WebRtcHost& WebRtcHost::operator=(WebRtcHost&& other) noexcept = default;

std::optional<WebRtcHost> WebRtcHost::create(const Config& config) {
    WebRtcHost host;
    host.impl_->config = config;
    log::info("WebRTC host ready (max {} peers, {} ICE servers)", config.max_peers,
              config.ice_servers.size());
    return host;
}

std::optional<std::uint32_t> WebRtcHost::accept_offer(const std::string& sdp) {
    if (impl_->peers.size() >= impl_->config.max_peers) {
        log::warn("WebRTC: rejecting offer, host full ({} peers)", impl_->peers.size());
        return std::nullopt;
    }

    const std::uint32_t peer_id = impl_->next_peer++;
    Impl::Peer peer;
    peer.connection = std::make_shared<rtc::PeerConnection>(make_configuration(impl_->config));

    Impl* impl = impl_.get();
    peer.connection->onLocalDescription([impl, peer_id](rtc::Description description) {
        impl->queue_signal(
            Signal{peer_id, Signal::Type::Answer, std::string(description), std::string{}});
    });
    peer.connection->onLocalCandidate([impl, peer_id](rtc::Candidate candidate) {
        impl->queue_signal(
            Signal{peer_id, Signal::Type::Candidate, std::string(candidate), candidate.mid()});
    });
    peer.connection->onStateChange([impl, peer_id](rtc::PeerConnection::State state) {
        if (state == rtc::PeerConnection::State::Closed ||
            state == rtc::PeerConnection::State::Failed ||
            state == rtc::PeerConnection::State::Disconnected) {
            impl->queue_close(peer_id);
        }
    });

    // The client creates both channels; we adopt them as they arrive. Doing
    // it this way (rather than the server creating them) keeps the offerer in
    // charge of negotiation, which is what a browser client naturally does.
    peer.connection->onDataChannel(
        [impl, peer_id](const std::shared_ptr<rtc::DataChannel>& channel) {
            const std::string label = channel->label();
            NetChannel kind = NetChannel::Reliable;
            {
                const std::lock_guard lock{impl->mutex};
                const auto found = impl->peers.find(peer_id);
                if (found == impl->peers.end()) {
                    return;
                }
                if (label == kSequencedLabel) {
                    found->second.sequenced = channel;
                    kind = NetChannel::Sequenced;
                } else if (label == kReliableLabel) {
                    found->second.reliable = channel;
                } else {
                    log::warn("WebRTC peer {}: unknown channel '{}', ignoring", peer_id, label);
                    return;
                }
            }
            impl->bind_channel(peer_id, channel, kind);
            if (channel->isOpen()) {
                impl->announce_if_ready(peer_id);
            }
        });

    {
        const std::lock_guard lock{impl_->mutex};
        impl_->peers.emplace(peer_id, std::move(peer));
    }

    try {
        impl_->peers.at(peer_id).connection->setRemoteDescription(
            rtc::Description(sdp, rtc::Description::Type::Offer));
    } catch (const std::exception& error) {
        // A malformed offer is a client problem, not a server crash: drop the
        // half-built peer and carry on.
        log::warn("WebRTC: rejecting offer: {}", error.what());
        // Same rule as poll(): move the peer out under the lock, destroy it
        // outside, or the connection's threads deadlock against the mutex.
        Impl::Peer doomed;
        {
            const std::lock_guard lock{impl_->mutex};
            const auto found = impl_->peers.find(peer_id);
            if (found != impl_->peers.end()) {
                doomed = std::move(found->second);
                impl_->peers.erase(found);
            }
        }
        return std::nullopt;
    }
    return peer_id;
}

void WebRtcHost::add_remote_candidate(std::uint32_t peer, const std::string& candidate,
                                      const std::string& mid) {
    const auto found = impl_->peers.find(peer);
    if (found == impl_->peers.end()) {
        return;
    }
    try {
        found->second.connection->addRemoteCandidate(rtc::Candidate(candidate, mid));
    } catch (const std::exception& error) {
        log::warn("WebRTC peer {}: bad ICE candidate: {}", peer, error.what());
    }
}

void WebRtcHost::take_signals(std::vector<Signal>& out) {
    const std::lock_guard lock{impl_->mutex};
    out.insert(out.end(), std::make_move_iterator(impl_->pending_signals.begin()),
               std::make_move_iterator(impl_->pending_signals.end()));
    impl_->pending_signals.clear();
}

void WebRtcHost::poll(std::vector<NetEvent>& out) {
    std::vector<NetEvent> drained;
    {
        const std::lock_guard lock{impl_->mutex};
        drained.swap(impl_->pending_events);
    }
    for (NetEvent& event : drained) {
        if (event.type == NetEvent::Type::Message) {
            ++impl_->stats.packets_received;
            impl_->stats.bytes_received += event.data.size();
        }
        out.push_back(std::move(event));
    }

    // Reap peers whose connection has gone; their Disconnected event was
    // already queued by queue_close.
    //
    // Destroying an rtc::PeerConnection joins its callback threads, and those
    // callbacks take impl_->mutex. Destroying one while holding the mutex
    // therefore deadlocks: poll() waits for the thread, the thread waits for
    // the mutex. So the doomed peers are moved out under the lock and
    // destroyed after it is released.
    std::vector<Impl::Peer> doomed;
    {
        const std::lock_guard lock{impl_->mutex};
        for (auto it = impl_->peers.begin(); it != impl_->peers.end();) {
            if (it->second.closed) {
                doomed.push_back(std::move(it->second));
                it = impl_->peers.erase(it);
            } else {
                ++it;
            }
        }
    }
    doomed.clear();  // destructors run here, with no lock held
}

void WebRtcHost::send(std::uint32_t peer, std::span<const std::uint8_t> data, NetChannel channel,
                      bool /*reliable*/) {
    // Reliability is a property of the channel here, not of the call: the
    // DataChannels were created with fixed semantics at negotiation time, so
    // the per-send `reliable` flag ENet honours has nowhere to go.
    const auto found = impl_->peers.find(peer);
    if (found == impl_->peers.end()) {
        return;
    }
    const std::shared_ptr<rtc::DataChannel>& target =
        channel == NetChannel::Sequenced ? found->second.sequenced : found->second.reliable;
    if (!target || !target->isOpen()) {
        return;
    }

    std::vector<std::byte> bytes(data.size());
    std::transform(data.begin(), data.end(), bytes.begin(),
                   [](std::uint8_t b) { return static_cast<std::byte>(b); });
    try {
        target->send(bytes);
        ++impl_->stats.packets_sent;
        impl_->stats.bytes_sent += data.size();
    } catch (const std::exception& error) {
        log::warn("WebRTC peer {}: send failed: {}", peer, error.what());
    }
}

void WebRtcHost::broadcast(std::span<const std::uint8_t> data, NetChannel channel, bool reliable) {
    std::vector<std::uint32_t> ids;
    ids.reserve(impl_->peers.size());
    for (const auto& [id, peer] : impl_->peers) {
        ids.push_back(id);
    }
    for (const std::uint32_t id : ids) {
        send(id, data, channel, reliable);
    }
}

void WebRtcHost::disconnect(std::uint32_t peer) {
    const auto found = impl_->peers.find(peer);
    if (found == impl_->peers.end()) {
        return;
    }
    found->second.connection->close();
    impl_->queue_close(peer);
}

std::size_t WebRtcHost::peer_count() const {
    return impl_->peers.size();
}

const NetStats& WebRtcHost::stats() const {
    return impl_->stats;
}

}  // namespace eng
