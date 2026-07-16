#include "engine/net/transport.h"

#include <enet/enet.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <random>
#include <unordered_map>

#include "engine/core/log.h"

namespace eng {

namespace {

void ensure_enet_initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (enet_initialize() != 0) {
            log::error("enet_initialize failed");
            return;
        }
        std::atexit(enet_deinitialize);
    });
}

std::uint32_t peer_id_of(const ENetPeer* peer) {
    return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(peer->data));
}

}  // namespace

struct NetHost::Impl {
    ENetHost* host = nullptr;
    std::unordered_map<std::uint32_t, ENetPeer*> peers;
    std::uint32_t next_peer_id = 1;
    NetStats stats;

    // Network condition simulation (see NetSimConfig).
    NetSimConfig sim;
    std::mt19937 rng{0xC0FFEEu};
    struct DelayedSend {
        std::chrono::steady_clock::time_point when;
        std::uint32_t peer;
        std::vector<std::uint8_t> data;
        NetChannel channel;
        bool reliable;
    };
    struct DelayedEvent {
        std::chrono::steady_clock::time_point when;
        NetEvent event;
    };
    std::vector<DelayedSend> delayed_out;
    std::vector<DelayedEvent> delayed_in;

    std::chrono::steady_clock::time_point delivery_time() {
        auto when = std::chrono::steady_clock::now() + std::chrono::milliseconds(sim.latency_ms);
        if (sim.jitter_ms > 0) {
            std::uniform_int_distribution<int> jitter(0, sim.jitter_ms);
            when += std::chrono::milliseconds(jitter(rng));
        }
        return when;
    }

    bool should_drop(NetChannel channel, bool reliable) {
        if (sim.loss_percent <= 0.0f || reliable || channel != NetChannel::Sequenced) {
            return false;
        }
        std::uniform_real_distribution<float> roll(0.0f, 100.0f);
        return roll(rng) < sim.loss_percent;
    }

    void raw_send(std::uint32_t peer_id, std::span<const std::uint8_t> data, NetChannel channel,
                  bool reliable) {
        const auto it = peers.find(peer_id);
        if (it == peers.end()) {
            return;
        }
        ENetPacket* packet =
            enet_packet_create(data.data(), data.size(), reliable ? ENET_PACKET_FLAG_RELIABLE : 0u);
        if (enet_peer_send(it->second, static_cast<enet_uint8>(channel), packet) != 0) {
            enet_packet_destroy(packet);
            return;
        }
        stats.bytes_sent += data.size();
        ++stats.packets_sent;
    }

    ~Impl() {
        if (host != nullptr) {
            enet_host_destroy(host);
        }
    }
};

NetHost::NetHost() : impl_(std::make_unique<Impl>()) {}
NetHost::~NetHost() = default;
NetHost::NetHost(NetHost&&) noexcept = default;
NetHost& NetHost::operator=(NetHost&&) noexcept = default;

std::optional<NetHost> NetHost::create_server(std::uint16_t port, std::size_t max_peers) {
    ensure_enet_initialized();
    ENetAddress address{};
    address.host = ENET_HOST_ANY;
    address.port = port;

    NetHost net;
    net.impl_->host = enet_host_create(&address, max_peers, 2 /*channels*/, 0, 0);
    if (net.impl_->host == nullptr) {
        log::error("enet_host_create failed for port {} (already in use?)", port);
        return std::nullopt;
    }
    log::info("Server listening on UDP port {}", port);
    return net;
}

std::optional<NetHost> NetHost::create_client() {
    ensure_enet_initialized();
    NetHost net;
    net.impl_->host = enet_host_create(nullptr, 1, 2 /*channels*/, 0, 0);
    if (net.impl_->host == nullptr) {
        log::error("enet_host_create (client) failed");
        return std::nullopt;
    }
    return net;
}

std::optional<std::uint32_t> NetHost::connect(const std::string& host, std::uint16_t port) {
    ENetAddress address{};
    if (enet_address_set_host(&address, host.c_str()) != 0) {
        log::error("Could not resolve host '{}'", host);
        return std::nullopt;
    }
    address.port = port;

    ENetPeer* peer = enet_host_connect(impl_->host, &address, 2, 0);
    if (peer == nullptr) {
        log::error("enet_host_connect failed");
        return std::nullopt;
    }
    const std::uint32_t id = impl_->next_peer_id++;
    peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
    impl_->peers.emplace(id, peer);
    log::info("Connecting to {}:{} ...", host, port);
    return id;
}

void NetHost::poll(std::vector<NetEvent>& out) {
    const auto now = std::chrono::steady_clock::now();

    // Release delayed outgoing sends that are due.
    std::erase_if(impl_->delayed_out, [&](Impl::DelayedSend& delayed) {
        if (delayed.when <= now) {
            impl_->raw_send(delayed.peer, delayed.data, delayed.channel, delayed.reliable);
            return true;
        }
        return false;
    });

    ENetEvent event;
    while (enet_host_service(impl_->host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT: {
                std::uint32_t id = peer_id_of(event.peer);
                if (id == 0) {  // incoming connection on a server
                    id = impl_->next_peer_id++;
                    event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
                    impl_->peers.emplace(id, event.peer);
                }
                out.push_back({NetEvent::Type::Connected, id, NetChannel::Reliable, {}});
                break;
            }
            case ENET_EVENT_TYPE_DISCONNECT: {
                const std::uint32_t id = peer_id_of(event.peer);
                impl_->peers.erase(id);
                event.peer->data = nullptr;
                out.push_back({NetEvent::Type::Disconnected, id, NetChannel::Reliable, {}});
                break;
            }
            case ENET_EVENT_TYPE_RECEIVE: {
                NetEvent message;
                message.type = NetEvent::Type::Message;
                message.peer = peer_id_of(event.peer);
                message.channel =
                    event.channelID == 0 ? NetChannel::Reliable : NetChannel::Sequenced;
                message.data.assign(event.packet->data,
                                    event.packet->data + event.packet->dataLength);
                impl_->stats.bytes_received += event.packet->dataLength;
                ++impl_->stats.packets_received;
                enet_packet_destroy(event.packet);
                if (impl_->sim.enabled()) {
                    if (!impl_->should_drop(message.channel, false)) {
                        impl_->delayed_in.push_back({impl_->delivery_time(), std::move(message)});
                    }
                } else {
                    out.push_back(std::move(message));
                }
                break;
            }
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    // Release delayed incoming messages that are due.
    std::erase_if(impl_->delayed_in, [&](Impl::DelayedEvent& delayed) {
        if (delayed.when <= now) {
            out.push_back(std::move(delayed.event));
            return true;
        }
        return false;
    });
}

void NetHost::send(std::uint32_t peer, std::span<const std::uint8_t> data, NetChannel channel,
                   bool reliable) {
    if (impl_->sim.enabled()) {
        if (impl_->should_drop(channel, reliable)) {
            return;
        }
        impl_->delayed_out.push_back(
            {impl_->delivery_time(), peer, {data.begin(), data.end()}, channel, reliable});
        return;
    }
    impl_->raw_send(peer, data, channel, reliable);
}

void NetHost::set_simulation(const NetSimConfig& config) {
    impl_->sim = config;
}

const NetSimConfig& NetHost::simulation() const {
    return impl_->sim;
}

void NetHost::broadcast(std::span<const std::uint8_t> data, NetChannel channel, bool reliable) {
    for (const auto& [id, peer] : impl_->peers) {
        send(id, data, channel, reliable);
    }
}

void NetHost::disconnect(std::uint32_t peer) {
    const auto it = impl_->peers.find(peer);
    if (it != impl_->peers.end()) {
        enet_peer_disconnect(it->second, 0);
    }
}

std::uint32_t NetHost::rtt_ms(std::uint32_t peer) const {
    const auto it = impl_->peers.find(peer);
    return it != impl_->peers.end() ? it->second->roundTripTime : 0;
}

std::size_t NetHost::peer_count() const {
    return impl_->peers.size();
}

const NetStats& NetHost::stats() const {
    return impl_->stats;
}

}  // namespace eng
