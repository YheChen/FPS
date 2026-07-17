#pragma once

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
    static std::optional<WebSocketHost> create_server(std::uint16_t port, std::size_t max_peers);

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
