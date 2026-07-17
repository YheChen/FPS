#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "engine/net/transport.h"

// Fans one ServerGame out over several transports at once (e.g. ENet for
// native clients + WebSocket for browser clients), presenting them as a
// single IServerTransport. Peer ids are made globally unique by tagging the
// child index into the top 8 bits, so ids never collide and send/disconnect
// route back to the right child. Main thread only.
namespace eng {

class CompositeTransport final : public IServerTransport {
public:
    explicit CompositeTransport(std::vector<std::unique_ptr<IServerTransport>> children);

    void poll(std::vector<NetEvent>& out) override;
    void send(std::uint32_t peer, std::span<const std::uint8_t> data, NetChannel channel,
              bool reliable) override;
    void broadcast(std::span<const std::uint8_t> data, NetChannel channel, bool reliable) override;
    void disconnect(std::uint32_t peer) override;
    std::size_t peer_count() const override;
    const NetStats& stats() const override;

    // Global peer id layout: top 8 bits = child index, low 24 = child-local.
    static constexpr std::uint32_t kLocalBits = 24;
    static std::uint32_t make_global(std::size_t child, std::uint32_t local) {
        return (static_cast<std::uint32_t>(child) << kLocalBits) | (local & 0x00FFFFFFu);
    }
    static std::size_t child_of(std::uint32_t global) { return global >> kLocalBits; }
    static std::uint32_t local_of(std::uint32_t global) { return global & 0x00FFFFFFu; }

private:
    std::vector<std::unique_ptr<IServerTransport>> children_;
    std::vector<NetEvent> scratch_;
    mutable NetStats stats_;
};

}  // namespace eng
