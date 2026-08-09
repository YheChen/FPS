#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "engine/net/transport.h"

// Minimal RFC 6455 WebSocket server implementing IServerTransport, so the
// dedicated server can accept browser (WASM) clients. Plain ws:// only:
// TLS is terminated by a reverse proxy in production (docs/deploy.md), which
// keeps this code small and dependency-free.
//
// WebSockets are reliable + ordered (TCP underneath), so the NetChannel /
// reliable arguments are ignored on send - every message is delivered
// reliably in order. Binary frames carry the same serialized protocol
// messages as the ENet transport. Main thread only.
namespace eng {

class WebSocketHost final : public IServerTransport {
public:
    // How long a connection may sit accepted-but-not-upgraded before it is
    // dropped. A socket that never speaks still holds a peer slot, so without
    // a deadline `max_peers` idle connections -- which cost an attacker
    // nothing and need no valid protocol -- lock every real player out. Ten
    // seconds is far past what a real upgrade takes and far short of what an
    // attacker needs.
    static constexpr std::chrono::milliseconds kDefaultHandshakeTimeout{10'000};

    static std::optional<WebSocketHost> create_server(
        std::uint16_t port, std::size_t max_peers,
        std::chrono::milliseconds handshake_timeout = kDefaultHandshakeTimeout);

    ~WebSocketHost() override;
    WebSocketHost(WebSocketHost&&) noexcept;
    WebSocketHost& operator=(WebSocketHost&&) noexcept;
    WebSocketHost(const WebSocketHost&) = delete;
    WebSocketHost& operator=(const WebSocketHost&) = delete;

    void poll(std::vector<NetEvent>& out) override;
    void send(std::uint32_t peer, std::span<const std::uint8_t> data, NetChannel channel,
              bool reliable) override;
    void broadcast(std::span<const std::uint8_t> data, NetChannel channel, bool reliable) override;
    void disconnect(std::uint32_t peer) override;
    std::size_t peer_count() const override;
    const NetStats& stats() const override;

private:
    WebSocketHost();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace eng
