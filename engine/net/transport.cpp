#include "engine/net/transport.h"

#include <enet/enet.h>

#include <cstdlib>
#include <mutex>
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
                message.channel = event.channelID == 0 ? NetChannel::Reliable
                                                       : NetChannel::Sequenced;
                message.data.assign(event.packet->data,
                                    event.packet->data + event.packet->dataLength);
                impl_->stats.bytes_received += event.packet->dataLength;
                ++impl_->stats.packets_received;
                enet_packet_destroy(event.packet);
                out.push_back(std::move(message));
                break;
            }
            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }
}

void NetHost::send(std::uint32_t peer, std::span<const std::uint8_t> data, NetChannel channel,
                   bool reliable) {
    const auto it = impl_->peers.find(peer);
    if (it == impl_->peers.end()) {
        return;
    }
    ENetPacket* packet = enet_packet_create(data.data(), data.size(),
                                            reliable ? ENET_PACKET_FLAG_RELIABLE : 0u);
    if (enet_peer_send(it->second, static_cast<enet_uint8>(channel), packet) != 0) {
        enet_packet_destroy(packet);
        return;
    }
    impl_->stats.bytes_sent += data.size();
    ++impl_->stats.packets_sent;
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
